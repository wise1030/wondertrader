/*!
 * \file OrderManager.cpp
 * \brief Order management implementation
 */

#include "OrderManager.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <boost/pool/object_pool.hpp>

namespace wt_option {

//=============================================================================
// Memory Pool for MultiLevelQuote
//=============================================================================
namespace {
    struct QuotePool {
        std::mutex mtx;
        boost::object_pool<MultiLevelQuote> pool;
        
        static QuotePool& instance() {
            static QuotePool inst;
            return inst;
        }
    };
} // namespace

void* MultiLevelQuote::operator new(size_t size) {
    if (size != sizeof(MultiLevelQuote)) {
        return ::operator new(size);
    }
    auto& pool = QuotePool::instance();
    std::lock_guard<std::mutex> lock(pool.mtx);
    return pool.pool.malloc();
}

void MultiLevelQuote::operator delete(void* ptr) {
    if (ptr) {
        auto& pool = QuotePool::instance();
        std::lock_guard<std::mutex> lock(pool.mtx);
        pool.pool.free(static_cast<MultiLevelQuote*>(ptr));
    }
}

//=============================================================================
// OptionOrderManager implementation
//=============================================================================


OptionOrderManager::OptionOrderManager(OptionDataPtr option, const OrderManagerParams& params)
    : m_params(params)
    , m_enabled(false)
    , m_position(0)
    , m_positionOffset(0)
    , m_nextOrderId(1)
{
    m_option = option;
    if (option) {
        m_code = option->getCode();
    }
}

uint32_t OptionOrderManager::sendOrder(OptionDataPtr option, OrderDir dir,
                                        double price, uint32_t qty) {
    if (!m_enabled) return 0;
    
    
    uint32_t orderId = generateOrderId();
    auto order = std::make_shared<OptionOrder>(orderId, option, dir, price, qty);
    
    // Capture values at issue
    order->captureValuesAtIssue();
    
    // Store order
    m_orders[orderId] = order;
    m_activeOrderIds.push_back(orderId);
    
    // Execute order
    bool result = false;
    if (m_orderExecutor) {
        result = m_orderExecutor(*order);
    } else {
        result = true; // Sim mode
    }
    
    if (result) {
        order->setState(OrderState::Sent);
        using namespace std::chrono;
        order->setSendTime(duration_cast<microseconds>(
            system_clock::now().time_since_epoch()
        ).count());
        
        notifyListeners([&](IOrderListener* l){ l->onOrderSent(*order); });
        m_stats.numOrders++;
    } else {
        order->setState(OrderState::Rejected);
        notifyListeners([&](IOrderListener* l){ l->onOrderRejected(*order, "Executor rejected"); });
        m_stats.numRejects++;
    }
    
    return orderId;
}

void OptionOrderManager::setDesiredQuote(const MultiLevelQuote& quote) {
    m_desiredQuote = quote;
}

MarketQuote OptionOrderManager::getCurrentQuoteLegacy() const {
    MarketQuote q;
    // Simplistic: Assume we are tracking currentQuote elsewhere or reconstruct it
    // For now we don't reconstruct full depth
    return q;
}

MarketQuote OptionOrderManager::getDesiredQuoteLegacy() const {
    MarketQuote q;
    if (!m_desiredQuote.bids.empty()) {
        q.bidPrice = m_desiredQuote.bids[0].price;
        q.bidSize = m_desiredQuote.bids[0].size;
    }
    if (!m_desiredQuote.asks.empty()) {
        q.askPrice = m_desiredQuote.asks[0].price;
        q.askSize = m_desiredQuote.asks[0].size;
    }
    return q;
}

int32_t OptionOrderManager::updateOrders(bool cancelOnly) {
    if (!m_enabled && !cancelOnly) return 0;
    
    int32_t transactions = 0;
    
    if (cancelOnly) {
        if (cancelAll()) transactions++;
        return transactions;
    }
    
    auto processSide = [&](const std::vector<MultiLevelQuote::Level>& levels, OrderDir dir) {
        if (levels.empty()) {
            // Cancel all on this side
            cancelByPriceRange(0, 1e9, dir); 
            return;
        }
        
        // Multi-level support: Iterate desired levels
        // Simple logic: We only support maintaining the TOP level for now to avoid order spam
        // But logic structure allows iteration.
        
        const auto& best = levels[0];
        if (!best.isValid()) return;
        
        // Cancellations first: Cancel any order NOT at desired price
        // (This is 'strict' mode, keeps order book clean)
        std::vector<uint32_t> toCancel;
        for (uint32_t id : m_activeOrderIds) {
            auto it = m_orders.find(id);
            if (it != m_orders.end()) {
                auto& order = it->second;
                if (order->getDirection() == dir && 
                    std::abs(order->getPrice() - best.price) > 1e-6) {
                    toCancel.push_back(id);
                }
            }
        }
        
        for(uint32_t id : toCancel) {
            cancelOrder(id);
            transactions++;
        }
        
        // Add/Reduce size at target price
        int32_t missing = getMissingPriceLevelSize(best.price, best.size, dir);
        if (missing < 0) {
             partialCancelByPriceRange(-missing, best.price, best.price, dir);
             transactions++;
        } else if (missing > 0) {
            auto opt = m_option.lock();
            if (opt) {
                // If dual-quote API is not registered, or we have an unbalanced side, fallback to sendOrder.
                sendOrder(opt, dir, best.price, missing);
                transactions++;
            }
        }
    };
    
    // Check if we can submit a bulk dual-sided Quote instead of individual orders
    if (!cancelOnly && m_quoteExecutor && m_desiredQuote.hasLevels() && !m_desiredQuote.bids.empty() && !m_desiredQuote.asks.empty()) {
        using namespace std::chrono;
        uint64_t now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        
        const auto& bestBid = m_desiredQuote.bids[0];
        const auto& bestAsk = m_desiredQuote.asks[0];
        
        int32_t bidMissing = getMissingPriceLevelSize(bestBid.price, bestBid.size, OrderDir::Buy);
        int32_t askMissing = getMissingPriceLevelSize(bestAsk.price, bestAsk.size, OrderDir::Sell);
        
        if (bidMissing > 0 && askMissing > 0) {
             if (now - m_lastQuoteTime < m_params.min_quote_update_interval_ms) {
                 // Rate limit exceeded: return without transactions
                 return 0;
             }
             
             // We need to establish completely new quotes
             uint32_t quoteId = m_quoteExecutor(m_code, bestBid.price, bidMissing, bestAsk.price, askMissing);
             if (quoteId > 0) {
                 transactions += 2;
                 m_lastQuoteTime = now;
             }
             
             // Handle cancellations for anything not matching this new quote block
             processSide(m_desiredQuote.bids, OrderDir::Buy);
             processSide(m_desiredQuote.asks, OrderDir::Sell);
             return transactions;
        }
    }
    
    processSide(m_desiredQuote.bids, OrderDir::Buy);
    processSide(m_desiredQuote.asks, OrderDir::Sell);
    
    return transactions;
}

int32_t OptionOrderManager::getMissingPriceLevelSize(double price, uint32_t desiredSize, OrderDir dir) {
    uint32_t currentSize = 0;
    for (uint32_t id : m_activeOrderIds) {
        auto it = m_orders.find(id);
        if (it != m_orders.end()) {
            auto& order = it->second;
            if (order->getDirection() == dir && 
                std::abs(order->getPrice() - price) < 1e-6 &&
                !order->isCancelled()) {
                currentSize += order->getRemainingQty();
            }
        }
    }
    return static_cast<int32_t>(desiredSize) - static_cast<int32_t>(currentSize);
}

void OptionOrderManager::cancelByPriceRange(double minPrice, double maxPrice, OrderDir dir) {
    std::vector<uint32_t> toCancel;
    for (uint32_t id : m_activeOrderIds) {
        auto it = m_orders.find(id);
        if (it != m_orders.end()) {
            auto& order = it->second;
            double p = order->getPrice();
            if (order->getDirection() == dir && p >= minPrice && p <= maxPrice) {
                 toCancel.push_back(id);
            }
        }
    }
    for(uint32_t id : toCancel) cancelOrder(id);
}

int32_t OptionOrderManager::partialCancelByPriceRange(int32_t sizeToCancel, double minPrice, double maxPrice, OrderDir dir) {
    int32_t cancelled = 0;
    std::vector<uint32_t> toCancel; // Collect first to avoid iterator invalidation issues if active ID list changes
    
    for (uint32_t id : m_activeOrderIds) {
         if (cancelled >= sizeToCancel) break;
         auto it = m_orders.find(id);
         if (it != m_orders.end()) {
             auto& order = it->second;
             double p = order->getPrice();
             if (order->getDirection() == dir && p >= minPrice && p <= maxPrice) {
                 uint32_t rem = order->getRemainingQty();
                 toCancel.push_back(id);
                 cancelled += rem; // Simplified assumption: We cancel whole order
             }
         }
    }
    
    for(uint32_t id : toCancel) cancelOrder(id);
    return cancelled;
}

void OptionOrderManager::notifyListeners(const std::function<void(IOrderListener*)>& op) {
    for (auto* l : m_listeners) op(l);
}

void OptionOrderManager::onFill(uint32_t orderId, double fillPrice, uint32_t fillQty, uint64_t fillTime) {
    auto it = m_orders.find(orderId);
    if (it != m_orders.end()) {
        auto& order = it->second;
        order->addFill(fillPrice, fillQty, fillTime);
        
        m_position += (order->getDirection() == OrderDir::Buy) ? fillQty : -((int32_t)fillQty);
        m_stats.numFills++;
        m_stats.totalVolume += fillPrice * fillQty;
        
        notifyListeners([&](IOrderListener* l){ l->onOrderFill(*order, order->getFills().back()); });
    }
}

void OptionOrderManager::onCancel(uint32_t orderId) {
    auto it = m_orders.find(orderId);
    if (it != m_orders.end()) {
        auto& order = it->second;
        order->setState(OrderState::Cancelled);
        
        // Remove from active list
        auto itList = std::find(m_activeOrderIds.begin(), m_activeOrderIds.end(), orderId);
        if (itList != m_activeOrderIds.end()) {
            m_activeOrderIds.erase(itList);
        }
        
        notifyListeners([&](IOrderListener* l){ l->onOrderCancelled(*order); });
        m_stats.numCancels++;
    }
}

void OptionOrderManager::onReject(uint32_t orderId, const std::string& reason) {
    auto it = m_orders.find(orderId);
    if (it != m_orders.end()) {
        auto& order = it->second;
        order->setState(OrderState::Rejected);
        
        auto itList = std::find(m_activeOrderIds.begin(), m_activeOrderIds.end(), orderId);
        if (itList != m_activeOrderIds.end()) {
            m_activeOrderIds.erase(itList);
        }
        
        notifyListeners([&](IOrderListener* l){ l->onOrderRejected(*order, reason); });
        m_stats.numRejects++;
    }
}

void OptionOrderManager::onOrderStatusChange(uint32_t orderId, OrderState newState) {
    auto it = m_orders.find(orderId);
    if (it != m_orders.end()) {
        auto& order = it->second;
        // Only update if not already in final state
        if (!order->isFilled() && !order->isCancelled()) {
            order->setState(newState);
            
            // If filled or cancelled via status update, remove from active list
            if (newState == OrderState::Filled || newState == OrderState::Cancelled || newState == OrderState::Rejected) {
                 auto itList = std::find(m_activeOrderIds.begin(), m_activeOrderIds.end(), orderId);
                 if (itList != m_activeOrderIds.end()) m_activeOrderIds.erase(itList);
            }
        }
    }
}

bool OptionOrderManager::cancelOrder(uint32_t orderId) {
    // Delegate to executor to send Cancel Request
    if (m_cancelExecutor) {
        if(m_cancelExecutor(orderId)) {
            // Optimistically or wait for event?
            // Wait for onCancel callback usually.
            // But mark as pending cancel?
            return true;
        }
    }
    return false;
}

bool OptionOrderManager::cancelAll() {
    bool success = true;
    for (uint32_t id : m_activeOrderIds) {
        if (!cancelOrder(id)) success = false;
    }
    return success;
}

BaseOrderPtr OptionOrderManager::getOrder(uint32_t orderId) {
    auto it = m_orders.find(orderId);
    return (it != m_orders.end()) ? it->second : nullptr;
}

std::vector<BaseOrderPtr> OptionOrderManager::getActiveOrders() {
    std::vector<BaseOrderPtr> result;
    for (uint32_t id : m_activeOrderIds) {
         result.push_back(m_orders[id]);
    }
    return result;
}

std::vector<BaseOrderPtr> OptionOrderManager::getOrdersByOption(const std::string& code) {
    if (code == m_code) return getActiveOrders();
    return {};
}

void OptionOrderManager::enable() {
    m_enabled = true;
}

void OptionOrderManager::disable() {
    m_enabled = false;
    cancelAll();
}

uint32_t OptionOrderManager::generateOrderId() {
    return m_nextOrderId++;
}

void OptionOrderManager::addListener(IOrderListener* listener) {
    m_listeners.push_back(listener);
}

void OptionOrderManager::removeListener(IOrderListener* listener) {
    m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), listener), m_listeners.end());
}


//=============================================================================
// UnderlyingOrderManager implementation
//=============================================================================

UnderlyingOrderManager::UnderlyingOrderManager(UnderlyingTradingDataPtr data, const OrderManagerParams& params)
    : m_params(params)
    , m_enabled(false)
    , m_position(0)
    , m_positionOffset(0)
    , m_nextOrderId(1)
    , m_lastQuoteTime(0)
{
    m_data = data;
    if (data) {
        m_code = data->getCode();
    }
}

uint32_t UnderlyingOrderManager::sendOrder(OrderDir dir, double price, uint32_t qty) {
    if (!m_enabled) return 0;
    
    uint32_t orderId = generateOrderId();
    // Use new constructor that takes Code string
    auto order = std::make_shared<BaseOrder>(orderId, m_code, dir, price, qty);
    
    m_orders[orderId] = order;
    m_activeOrderIds.push_back(orderId);
    
    bool result = false;
    if (m_orderExecutor) {
        result = m_orderExecutor(*order);
    } else {
        result = true; 
    }
    
    if (result) {
        order->setState(OrderState::Sent);
        using namespace std::chrono;
        order->setSendTime(duration_cast<microseconds>(
            system_clock::now().time_since_epoch()
        ).count());
        m_stats.numOrders++;
    } else {
        order->setState(OrderState::Rejected);
        m_stats.numRejects++;
    }
    return orderId;
}

// Logic similar to OptionOrderManager but for Underlying
int32_t UnderlyingOrderManager::updateOrders(bool cancelOnly) {
    if (!m_enabled && !cancelOnly) return 0;
    int32_t transactions = 0;
    
    if (cancelOnly) {
        if (cancelAll()) transactions++;
        return transactions;
    }
    
    // Helper to calculate missing size
    auto getMissing = [&](double price, uint32_t desired, OrderDir dir) {
        uint32_t current = 0;
        for (uint32_t id : m_activeOrderIds) {
            auto it = m_orders.find(id);
            if (it != m_orders.end()) {
                auto& o = it->second;
                if (o->getDirection() == dir && std::abs(o->getPrice() - price) < 1e-6 && !o->isCancelled()) {
                    current += o->getRemainingQty();
                }
            }
        }
        return (int32_t)desired - (int32_t)current;
    };
    
    // Process Bids
    if (!m_desiredQuote.bids.empty() && m_desiredQuote.asks.empty() && m_quoteExecutor) {
         // Not strictly dual side here, but if quoteApi is used:
         // Note: Usually quote executor is for dual side. If single, maybe fallback to sendOrder.
    }
    
    // We only deal with quotes if executor is defined and we want both sides
    if (!cancelOnly && m_quoteExecutor && !m_desiredQuote.bids.empty() && !m_desiredQuote.asks.empty()) {
        using namespace std::chrono;
        uint64_t now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

        auto& bestBid = m_desiredQuote.bids[0];
        auto& bestAsk = m_desiredQuote.asks[0];
        
        int32_t bidMissing = getMissing(bestBid.price, bestBid.size, OrderDir::Buy);
        int32_t askMissing = getMissing(bestAsk.price, bestAsk.size, OrderDir::Sell);
        
        if (bidMissing > 0 && askMissing > 0) {
            if (now - m_lastQuoteTime < m_params.min_quote_update_interval_ms) {
                return 0; // Throttle
            }
            uint32_t quoteId = m_quoteExecutor(m_code, bestBid.price, bidMissing, bestAsk.price, askMissing);
            if (quoteId > 0) {
                 transactions += 2;
                 m_lastQuoteTime = now;
            }
            
            // Still cancel unmatched
            std::vector<uint32_t> toCancel;
            for (uint32_t id : m_activeOrderIds) {
                auto& o = m_orders[id];
                if ((o->getDirection() == OrderDir::Buy && std::abs(o->getPrice() - bestBid.price) > 1e-6) ||
                    (o->getDirection() == OrderDir::Sell && std::abs(o->getPrice() - bestAsk.price) > 1e-6)) {
                    toCancel.push_back(id);
                }
            }
            for(auto id:toCancel) { cancelOrder(id); transactions++; }
            return transactions;
        }
    }
    
    // Process Bids only (Fallback / single side updates)
    if (!m_desiredQuote.bids.empty()) {
        auto& best = m_desiredQuote.bids[0];
        if (best.isValid()) {
            // Cancel non-matching
            std::vector<uint32_t> toCancel;
            for (uint32_t id : m_activeOrderIds) {
                auto& o = m_orders[id];
                if (o->getDirection() == OrderDir::Buy && std::abs(o->getPrice() - best.price) > 1e-6) toCancel.push_back(id);
            }
            for(auto id:toCancel) { cancelOrder(id); transactions++; }
            
            int32_t missing = getMissing(best.price, best.size, OrderDir::Buy);
            if (missing > 0) {
                sendOrder(OrderDir::Buy, best.price, missing);
                transactions++;
            }
        }
    } else {
        // Cancel all bids
         std::vector<uint32_t> toCancel;
         for (uint32_t id : m_activeOrderIds) if (m_orders[id]->getDirection() == OrderDir::Buy) toCancel.push_back(id);
         for(auto id:toCancel) { cancelOrder(id); transactions++; }
    }

    // Process Asks
    if (!m_desiredQuote.asks.empty()) {
        auto& best = m_desiredQuote.asks[0];
        if (best.isValid()) {
             // Cancel non-matching
            std::vector<uint32_t> toCancel;
            for (uint32_t id : m_activeOrderIds) {
                auto& o = m_orders[id];
                if (o->getDirection() == OrderDir::Sell && std::abs(o->getPrice() - best.price) > 1e-6) toCancel.push_back(id);
            }
            for(auto id:toCancel) { cancelOrder(id); transactions++; }
            
            int32_t missing = getMissing(best.price, best.size, OrderDir::Sell);
            if (missing > 0) {
                sendOrder(OrderDir::Sell, best.price, missing);
                transactions++;
            }
        }
    } else {
         std::vector<uint32_t> toCancel;
         for (uint32_t id : m_activeOrderIds) if (m_orders[id]->getDirection() == OrderDir::Sell) toCancel.push_back(id);
         for(auto id:toCancel) { cancelOrder(id); transactions++; }
    }
    
    return transactions;
}

void UnderlyingOrderManager::setDesiredQuote(const MultiLevelQuote& quote) {
    m_desiredQuote = quote;
}

bool UnderlyingOrderManager::cancelOrder(uint32_t orderId) {
    if (m_cancelExecutor) return m_cancelExecutor(orderId);
    return false;
}

bool UnderlyingOrderManager::cancelAll() {
    bool s = true;
    auto copyIds = m_activeOrderIds; // Copy
    for(auto id : copyIds) if(!cancelOrder(id)) s=false;
    return s;
}

BaseOrderPtr UnderlyingOrderManager::getOrder(uint32_t orderId) {
    auto it = m_orders.find(orderId);
    return (it != m_orders.end()) ? it->second : nullptr;
}

std::vector<BaseOrderPtr> UnderlyingOrderManager::getActiveOrders() {
    std::vector<BaseOrderPtr> res;
    for(auto id : m_activeOrderIds) res.push_back(m_orders[id]);
    return res;
}

std::vector<BaseOrderPtr> UnderlyingOrderManager::getOrdersByOption(const std::string& code) {
    if (code == m_code) return getActiveOrders();
    return {};
}

void UnderlyingOrderManager::enable() { m_enabled = true; }
void UnderlyingOrderManager::disable() { m_enabled = false; cancelAll(); }

void UnderlyingOrderManager::onFill(uint32_t orderId, double fillPrice, uint32_t fillQty, uint64_t fillTime) {
    auto it = m_orders.find(orderId);
    if (it != m_orders.end()) {
        auto& order = it->second;
        order->addFill(fillPrice, fillQty, fillTime);
        m_position += (order->getDirection() == OrderDir::Buy) ? fillQty : -((int32_t)fillQty);
        m_stats.numFills++;
        m_stats.totalVolume += fillPrice * fillQty;
    }
}

void UnderlyingOrderManager::onCancel(uint32_t orderId) {
    auto it = m_orders.find(orderId);
    if (it != m_orders.end()) {
        it->second->setState(OrderState::Cancelled);
         auto itList = std::find(m_activeOrderIds.begin(), m_activeOrderIds.end(), orderId);
        if (itList != m_activeOrderIds.end()) m_activeOrderIds.erase(itList);
        m_stats.numCancels++;
    }
}

void UnderlyingOrderManager::onReject(uint32_t orderId, const std::string& reason) {
    auto it = m_orders.find(orderId);
    if (it != m_orders.end()) {
        it->second->setState(OrderState::Rejected);
        auto itList = std::find(m_activeOrderIds.begin(), m_activeOrderIds.end(), orderId);
        if (itList != m_activeOrderIds.end()) m_activeOrderIds.erase(itList);
        m_stats.numRejects++;
    }
}

void UnderlyingOrderManager::onOrderStatusChange(uint32_t orderId, OrderState newState) {
    auto it = m_orders.find(orderId);
    if (it != m_orders.end()) {
        auto& order = it->second;
        if (!order->isFilled() && !order->isCancelled()) {
            order->setState(newState);
             if (newState == OrderState::Filled || newState == OrderState::Cancelled || newState == OrderState::Rejected) {
                 auto itList = std::find(m_activeOrderIds.begin(), m_activeOrderIds.end(), orderId);
                 if (itList != m_activeOrderIds.end()) m_activeOrderIds.erase(itList);
            }
        }
    }
}

uint32_t UnderlyingOrderManager::generateOrderId() { return m_nextOrderId++; }


//=============================================================================
// GridOrderManager implementation
//=============================================================================

GridOrderManager::GridOrderManager() {
}

OptionOrderManagerPtr GridOrderManager::getOrderManager(OptionDataPtr option) {
    if (!option) return nullptr;
    
    uint32_t id = option->getInternalId();
    if (id >= m_managersByIndex.size()) {
        m_managersByIndex.resize(id + 1);
    }
    
    if (m_managersByIndex[id]) {
        return m_managersByIndex[id];
    }
    
    // Create new manager and link it to both caches
    auto manager = std::make_shared<OptionOrderManager>(option);
    manager->setOrderExecutor(m_globalOrderExecutor);
    manager->setCancelExecutor(m_globalCancelExecutor);
    
    const std::string& code = option->getCode();
    auto it = m_managers.find(code);
    if (it != m_managers.end()) {
        return it->second;
    }
    
    // Create new
    auto mgr = std::make_shared<OptionOrderManager>(option);
    
    // Set global executors if available
    if (m_globalOrderExecutor) mgr->setOrderExecutor(m_globalOrderExecutor);
    if (m_globalCancelExecutor) mgr->setCancelExecutor(m_globalCancelExecutor);
    if (m_globalQuoteExecutor) mgr->setQuoteExecutor(m_globalQuoteExecutor);
    
    m_managers[code] = mgr;
    m_managersByIndex.push_back(mgr);
    
    return mgr;
}

OptionOrderManagerPtr GridOrderManager::getOrderManager(const std::string& code) {
    auto it = m_managers.find(code);
    if (it != m_managers.end()) return it->second;
    
    // Fallback: created by string only without OptionData (rare/legacy)
    auto manager = std::make_shared<OptionOrderManager>(nullptr);
    manager->setOrderExecutor(m_globalOrderExecutor);
    manager->setCancelExecutor(m_globalCancelExecutor);
    m_managers[code] = manager;
    return manager;
}

UnderlyingOrderManagerPtr GridOrderManager::getUnderlyingOrderManager(const std::string& code) {
    auto it = m_underlyingManagers.find(code);
    if (it != m_underlyingManagers.end()) return it->second;
    
    // Create new manager
    // We need UnderlyingTradingData... normally passed from outside? 
    // GridOrderManager doesn't have access to Grid directly here
    // But we can create manager with empty data? Or require caller to set it.
    // For now, create with nullptr data if missing
    auto manager = std::make_shared<UnderlyingOrderManager>(nullptr); 
    // Wait, UnderlyingOrderManager ctor needs DataPtr.
    // Ideally WtOptEngine should create UnderlyingOrderManager correctly using Grid Data.
    // But GridOrderManager provides access.
    // If we assume we only use this for existing managers...
    // Let's allow creating empty one.
    manager->setOrderExecutor(m_globalOrderExecutor);
    manager->setCancelExecutor(m_globalCancelExecutor);
    m_underlyingManagers[code] = manager;
    return manager;
}

void GridOrderManager::enableAll() {
    for (auto& pair : m_managers) pair.second->enable();
    for (auto& pair : m_underlyingManagers) pair.second->enable();
}

void GridOrderManager::disableAll() {
    for (auto& pair : m_managers) pair.second->disable();
    for (auto& pair : m_underlyingManagers) pair.second->disable();
}

bool GridOrderManager::cancelAll() {
    bool success = true;
    for (auto& pair : m_managers) if (!pair.second->cancelAll()) success = false;
    for (auto& pair : m_underlyingManagers) if (!pair.second->cancelAll()) success = false;
    return success;
}

uint32_t GridOrderManager::cancelOrdersByCode(const std::string& code) {
    uint32_t count = 0;
    
    // Check Option Managers
    auto it = m_managers.find(code);
    if (it != m_managers.end()) {
        auto orders = it->second->getActiveOrders();
        for (auto& ord : orders) {
            if (it->second->cancelOrder(ord->getOrderId())) count++;
        }
        return count;
    }
    
    // Check Underlying Managers
    auto it2 = m_underlyingManagers.find(code);
    if (it2 != m_underlyingManagers.end()) {
        auto orders = it2->second->getActiveOrders();
        for (auto& ord : orders) {
            if (it2->second->cancelOrder(ord->getOrderId())) count++;
        }
        return count;
    }
    
    return count;
}

// Helper for ranking
static int32_t calculateRank(OptionOrderManager* manager) {
    int32_t rank = 0;
    const auto& current = manager->getCurrentQuoteLegacy();
    const auto& desired = manager->getDesiredQuoteLegacy();
    
    if ( std::abs(current.bidPrice - desired.bidPrice) > 1e-6 ||
         current.bidSize != desired.bidSize ||
         std::abs(current.askPrice - desired.askPrice) > 1e-6 ||
         current.askSize != desired.askSize ) 
    {
         rank = 100;
    }
    return rank;
}

int32_t GridOrderManager::updateAllOrders(bool cancelOnly) {
    int32_t total = 0;
    
    // Process Options (fast array)
    for (auto& manager : m_managersByIndex) {
        if (manager) {
            total += manager->updateOrders(cancelOnly);
        }
    }
    
    // Process any orphaned managers created by string only
    for (auto& pair : m_managers) {
        if (!pair.second->getOption()) {
            total += pair.second->updateOrders(cancelOnly);
        }
    }
    
    // Process Underlying
    for (auto& pair : m_underlyingManagers) {
        total += pair.second->updateOrders(cancelOnly);
    }
    
    return total;
}

TradingStats GridOrderManager::getAggregateStats() const {
    TradingStats total;
    auto accumulate = [&total](const TradingStats& stats) {
        total.numOrders += stats.numOrders;
        total.numFills += stats.numFills;
        total.numCancels += stats.numCancels;
        total.numRejects += stats.numRejects;
        total.totalVolume += stats.totalVolume;
        total.realizedPnL += stats.realizedPnL;
        total.cancelSent += stats.cancelSent;
        total.cancelDropped += stats.cancelDropped;
        total.cancelInProgress += stats.cancelInProgress;
    };
    for (const auto& pair : m_managers) {
        accumulate(pair.second->getStats());
    }
    for (const auto& pair : m_underlyingManagers) {
        accumulate(pair.second->getStats());
    }
    return total;
}

int32_t GridOrderManager::getTotalPosition() const {
    int32_t total = 0;
    for (const auto& pair : m_managers) total += pair.second->getPosition();
    return total;
}

// Routes
void GridOrderManager::onFill(const std::string& code, uint32_t orderId,
                               double fillPrice, uint32_t fillQty, uint64_t fillTime) {
    // Check options first
    {
        auto it = m_managers.find(code);
        if (it != m_managers.end()) {
            it->second->onFill(orderId, fillPrice, fillQty, fillTime);
            return;
        }
    }
    // Check underlying
    {
        auto it = m_underlyingManagers.find(code);
        if (it != m_underlyingManagers.end()) {
            it->second->onFill(orderId, fillPrice, fillQty, fillTime);
            return;
        }
    }
}

void GridOrderManager::onCancel(const std::string& code, uint32_t orderId) {
    // Similar routing...
    auto it = m_managers.find(code);
    if (it != m_managers.end()) { it->second->onCancel(orderId); return; }
    auto it2 = m_underlyingManagers.find(code);
    if (it2 != m_underlyingManagers.end()) { it2->second->onCancel(orderId); return; }
}

void GridOrderManager::onReject(const std::string& code, uint32_t orderId, const std::string& reason) {
    auto it = m_managers.find(code);
    if (it != m_managers.end()) { it->second->onReject(orderId, reason); return; }
    auto it2 = m_underlyingManagers.find(code);
    if (it2 != m_underlyingManagers.end()) { it2->second->onReject(orderId, reason); return; }
}

void GridOrderManager::onOrderStatus(const std::string& code, uint32_t orderId, OrderState newState) {
    auto it = m_managers.find(code);
    if (it != m_managers.end()) { it->second->onOrderStatusChange(orderId, newState); return; }
    auto it2 = m_underlyingManagers.find(code);
    if (it2 != m_underlyingManagers.end()) { it2->second->onOrderStatusChange(orderId, newState); return; }
}

void GridOrderManager::setOrderExecutor(OrderExecutor executor) {
    m_globalOrderExecutor = executor;
    for (auto& pair : m_managers) {
        pair.second->setOrderExecutor(executor);
    }
    for (auto& pair : m_underlyingManagers) {
        pair.second->setOrderExecutor(executor);
    }
}

void GridOrderManager::setCancelExecutor(CancelExecutor executor) {
    m_globalCancelExecutor = executor;
    for (auto& pair : m_managers) {
        pair.second->setCancelExecutor(executor);
    }
    for (auto& pair : m_underlyingManagers) {
        pair.second->setCancelExecutor(executor);
    }
}

void GridOrderManager::setQuoteExecutor(QuoteExecutor executor) {
    m_globalQuoteExecutor = executor;
    for (auto& pair : m_managers) {
        pair.second->setQuoteExecutor(executor);
    }
    for (auto& pair : m_underlyingManagers) {
        pair.second->setQuoteExecutor(executor);
    }
}

//================================================================================
// Recovery Mechanics
//================================================================================

void OptionOrderManager::injectOrder(BaseOrderPtr order) {
    if (!order) return;
    
    auto optOrder = std::dynamic_pointer_cast<OptionOrder>(order);
    if (!optOrder) return;
    
    if (m_orders.find(optOrder->getOrderId()) == m_orders.end()) {
        m_orders[optOrder->getOrderId()] = optOrder;
        
        auto st = optOrder->getState();
        if (st == OrderState::Sent || st == OrderState::PartialFill) {
            m_activeOrderIds.push_back(optOrder->getOrderId());
        }
    }
}

void UnderlyingOrderManager::injectOrder(BaseOrderPtr order) {
    if (!order) return;
    
    if (m_orders.find(order->getOrderId()) == m_orders.end()) {
        m_orders[order->getOrderId()] = order;
        
        auto st = order->getState();
        if (st == OrderState::Sent || st == OrderState::PartialFill) {
            m_activeOrderIds.push_back(order->getOrderId());
        }
    }
}

void GridOrderManager::injectOrder(BaseOrderPtr order) {
    if (!order) return;
    // Fast path: find existing manager
    auto it = m_managers.find(order->getCode());
    if (it != m_managers.end()) {
        it->second->injectOrder(order);
        return;
    }
    
    // Check underlying manager
    auto uit = m_underlyingManagers.find(order->getCode());
    if (uit != m_underlyingManagers.end()) {
        uit->second->injectOrder(order);
    }
}

} // namespace wt_option
