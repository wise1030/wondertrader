/*!
 * \file WtOptContext.cpp
 * \brief Option Context Implementation
 * 
 * Implements IOptStraCtx interface with data access and trading methods.
 */
#include "WtOptContext.h"
#include "OptStrategy.h"

#include "../WtCore/TraderAdapter.h"
#include "../WtCore/WtDtMgr.h"

#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
// #include "../Includes/WTSCommodityInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSVariant.hpp"
#include "../WTSTools/WTSLogger.h"
#include "OptionOrder.h"
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <fstream>

USING_NS_WTP;

//=============================================================================
// Constructor / Destructor
//=============================================================================

WtOptContext::WtOptContext(WtOptEngine* engine, const char* name, OptStrategy* stra)
    : IOptStraCtx(name)
    , _engine(engine)
    , _stra(stra)
    , _trader(nullptr)
    , _worker_running(false)
    , _queue_drops(0)
    , _total_events(0)
    , _last_warning_time(0)
{
    static std::atomic<uint32_t> _auto_id(0);
    _id = 1000 + _auto_id.fetch_add(1);
    
    // Create name with prefix for logging
    _name_with_prefix = "[" + std::string(name) + "] ";
}

WtOptContext::~WtOptContext()
{
    stop();
}

//=============================================================================
// Identity
//=============================================================================

uint32_t WtOptContext::id()
{
    return _id;
}

//=============================================================================
// Callbacks
//=============================================================================

void WtOptContext::on_init()
{
    if (_stra) _stra->on_init(this);
}

void WtOptContext::on_session_begin(uint32_t uTDate)
{
    if (_stra) _stra->on_session_begin(this, uTDate);
}

void WtOptContext::on_session_end(uint32_t uTDate)
{
    if (_stra) _stra->on_session_end(this, uTDate);
}

void WtOptContext::on_tick(const char* stdCode, WTSTickData* newTick)
{
    // Async Dispatch via Queue
    enqueue_tick(stdCode, newTick);
}

void WtOptContext::on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
    // Option strategies typically don't use bar data, but forward if needed
    // if (_stra) _stra->on_bar(this, stdCode, period, times, newBar);
}

void WtOptContext::on_calculate(uint32_t curDate, uint32_t curTime)
{
    if (_stra) _stra->on_calculate(this, curDate, curTime);
}

//=============================================================================
// Option Component Access
//=============================================================================

//=============================================================================
// Option Component Access
//=============================================================================

wt_option::OptionGrid* WtOptContext::stra_get_grid()
{
    return _grid.get();
}

wt_option::OptionRisk* WtOptContext::stra_get_risk()
{
    return _risk.get();
}

wt_option::GridOrderManager* WtOptContext::stra_get_order_manager()
{
    return _order_mgr.get();
}

//=============================================================================
// Internal Lifecycle & Thread Callbacks
//=============================================================================

bool WtOptContext::init(WTSVariant* cfg)
{
    _grid = std::make_shared<wt_option::OptionGrid>();
    _risk = std::make_shared<wt_option::OptionRisk>(_grid);
    _order_mgr = std::make_shared<wt_option::GridOrderManager>();

    if (cfg) {
        std::string underlying = cfg->getCString("underlyingCode");
        
        if (!underlying.empty()) {
            _grid->setUnderlyingCode(underlying);
            WTSLogger::log_by_cat("strategy", LL_INFO, 
                "{}Option grid configured for underlying: {}", _name_with_prefix, underlying);
            
            // Wire trading calendar for accurate time-to-expiry
            if (_engine) {
                _grid->setBaseDataMgr(_engine->get_basedata_mgr());
                
                // Derive product ID from underlying code for holiday lookup
                IBaseDataMgr* bdMgr = _engine->get_basedata_mgr();
                if (bdMgr) {
                    WTSContractInfo* cInfo = bdMgr->getContract(underlying.c_str());
                    if (cInfo) {
                        WTSCommodityInfo* commInfo = cInfo->getCommInfo();
                        if (commInfo) {
                            std::string stdPID = fmt::format("{}.{}", commInfo->getExchg(), commInfo->getProduct());
                            _grid->setProductId(stdPID);
                            
                            // Use underlying's session for minute-level time-to-expiry
                            WTSSessionInfo* sInfo = commInfo->getSessionInfo();
                            if (sInfo) {
                                _grid->setSessionInfo(sInfo);
                            }
                        }
                    }
                }
            }
        }
        
        if (_grid) {
            // Pricer is managed by Strategy
        }
    }
    
    // Initialize order executors
    if (_order_mgr) {
         _order_mgr->setOrderExecutor([this](const wt_option::BaseOrder& order) -> bool {
            if (!_trader || !_trader->isReady()) {
                stra_log_error("TraderAdapter not ready for order");
                return false;
            }
            
            OrderIDs ids;
            if (order.getDirection() == wt_option::OrderDir::Buy) {
                ids = _trader->buy(order.getCode().c_str(), order.getPrice(), order.getQuantity(), 0, false);
            } else {
                ids = _trader->sell(order.getCode().c_str(), order.getPrice(), order.getQuantity(), 0, false);
            }
            
            return !ids.empty();
        });
        
        _order_mgr->setCancelExecutor([this](uint32_t orderId) -> bool {
            if (!_trader) return false;
            return _trader->cancel(orderId);
        });
    }

    start();
    load_data(); // Recover checkpoint data
    return true;
}

void WtOptContext::update_risk()
{
    if (_risk) {
        _risk->update();
    }
}

void WtOptContext::update_pnl()
{
    // Simplified PnL update logic if needed at context level
    // Actual strategy PnL is usually handled within the strategy logic
}

//=============================================================================
// Logging
//=============================================================================

void WtOptContext::stra_log_info(const char* message) 
{ 
    WTSLogger::log_by_cat("strategy", LL_INFO, "{}{}", _name_with_prefix, message);
}

void WtOptContext::stra_log_error(const char* message) 
{ 
    WTSLogger::log_by_cat("strategy", LL_ERROR, "{}{}", _name_with_prefix, message);
}

void WtOptContext::stra_log_warn(const char* message) 
{ 
    WTSLogger::log_by_cat("strategy", LL_WARN, "{}{}", _name_with_prefix, message);
}

void WtOptContext::stra_log_debug(const char* message) 
{ 
    WTSLogger::log_by_cat("strategy", LL_DEBUG, "{}{}", _name_with_prefix, message);
}

//=============================================================================
// Time Access
//=============================================================================

uint32_t WtOptContext::stra_get_date() 
{ 
    return _engine ? _engine->get_date() : 0;
}

uint32_t WtOptContext::stra_get_time() 
{ 
    return _engine ? _engine->get_min_time() : 0;
}

uint32_t WtOptContext::stra_get_secs() 
{ 
    return _engine ? _engine->get_secs() : 0;
}

uint32_t WtOptContext::stra_get_tdate()
{
    return _engine ? _engine->get_trading_date() : 0;
}

//=============================================================================
// Data Access
//=============================================================================

WTSTickSlice* WtOptContext::stra_get_ticks(const char* stdCode, uint32_t count)
{
    if (!_engine) return nullptr;
    return _engine->get_tick_slice(_id, stdCode, count);
}

WTSTickData* WtOptContext::stra_get_last_tick(const char* stdCode)
{
    if (!_engine) return nullptr;
    return _engine->get_last_tick(_id, stdCode);
}

WTSKlineSlice* WtOptContext::stra_get_bars(const char* stdCode, const char* period, uint32_t count)
{
    if (!_engine) return nullptr;
    return _engine->get_kline_slice(_id, stdCode, period, count);
}

double WtOptContext::stra_get_price(const char* stdCode)
{
    if (!_engine) return 0.0;
    return _engine->get_cur_price(stdCode);
}

//=============================================================================
// Trading Actions
//=============================================================================

uint32_t WtOptContext::stra_buy(const char* stdCode, double price, double qty, const char* userTag)
{
    if (!_trader) {
        stra_log_error("Cannot buy: no trader adapter set");
        return 0;
    }
    
    // Use trader adapter to submit order
    // Note: In WonderTrader, orders go through TraderAdapter
    // For options, we need to support both futures and options contracts
    
    WTSLogger::log_by_cat("strategy", LL_INFO, 
        "{}Buy order: {} @ {} x {}, tag: {}", 
        _name_with_prefix, stdCode, price, qty, userTag ? userTag : "");
    
    // Submit via trader adapter
    // Returns local order ID
    OrderIDs ids = _trader->buy(stdCode, price, qty, 0, false);
    return ids.empty() ? 0 : ids[0];
}

uint32_t WtOptContext::stra_sell(const char* stdCode, double price, double qty, const char* userTag)
{
    if (!_trader) {
        stra_log_error("Cannot sell: no trader adapter set");
        return 0;
    }
    
    WTSLogger::log_by_cat("strategy", LL_INFO, 
        "{}Sell order: {} @ {} x {}, tag: {}", 
        _name_with_prefix, stdCode, price, qty, userTag ? userTag : "");
    
    OrderIDs ids = _trader->sell(stdCode, price, qty, 0, false);
    return ids.empty() ? 0 : ids[0];
}

uint32_t WtOptContext::stra_quote(const char* stdCode, double bidPrice, uint32_t bidQty, double askPrice, uint32_t askQty, const char* userTag)
{
    if (!_trader) {
        stra_log_error("Cannot quote: no trader adapter set");
        return 0;
    }
    
    WTSLogger::log_by_cat("strategy", LL_INFO, 
        "{}Quote: {} [Bid: {}x{}] [Ask: {}x{}], tag: {}", 
        _name_with_prefix, stdCode, bidPrice, bidQty, askPrice, askQty, userTag ? userTag : "");
    
    // Utilize native trader quote API
    // The TraderAdapter will generate a quote request through ITraderApi to CTP natively
    return _trader->quote(stdCode, bidPrice, bidQty, askPrice, askQty, 0, nullptr);
}

bool WtOptContext::stra_cancel(uint32_t localid)
{
    if (!_trader) {
        stra_log_error("Cannot cancel: no trader adapter set");
        return false;
    }
    
    WTSLogger::log_by_cat("strategy", LL_DEBUG, 
        "{}Cancel order: {}", _name_with_prefix, localid);
    
    return _trader->cancel(localid);
}

bool WtOptContext::stra_cancel_quote(uint32_t localid)
{
    if (!_trader) {
        stra_log_error("Cannot cancel quote: no trader adapter set");
        return false;
    }
    
    WTSLogger::log_by_cat("strategy", LL_DEBUG, 
        "{}Cancel quote: {}", _name_with_prefix, localid);
    
    return _trader->cancelQuote(localid);
}

uint32_t WtOptContext::stra_cancel_all(const char* stdCode)
{
    if (!_trader) {
        stra_log_error("Cannot cancel all: no trader adapter set");
        return 0;
    }
    
    WTSLogger::log_by_cat("strategy", LL_INFO, 
        "{}Cancel all orders for: {}", _name_with_prefix, stdCode);
    
    // Delegate to OrderManager to find and cancel orders for this code
    if (_order_mgr) {
        return _order_mgr->cancelOrdersByCode(stdCode);
    }
    
    // Fallback if no order manager (unlikely in this architecture)
    return 0;
}

double WtOptContext::stra_get_position(const char* stdCode)
{
    if (!_trader) {
        return 0.0;
    }
    
    // Get position from trader adapter
    return _trader->getPosition(stdCode, true);
}

//=============================================================================
// Subscription
//=============================================================================

void WtOptContext::stra_sub_ticks(const char* stdCode)
{
    if (_engine) {
        _engine->sub_tick(_id, stdCode);
        WTSLogger::log_by_cat("strategy", LL_DEBUG, 
            "{}Subscribed to ticks: {}", _name_with_prefix, stdCode);
    }
}
//=============================================================================
// Async Worker Infrastructure implementation
//=============================================================================

void WtOptContext::start()
{
    if (_worker_running) return;
    
    _worker_running = true;
    _worker = std::thread(&WtOptContext::worker_loop, this);
    
    WTSLogger::log_by_cat("strategy", LL_INFO, "{}Async worker started", _name_with_prefix);
}

void WtOptContext::stop()
{
    if (!_worker_running) return;
    
    _worker_running = false;
    _worker_cv.notify_all();
    
    if (_worker.joinable()) {
        _worker.join();
    }
    
    save_data(); // Persist checkout data
    WTSLogger::log_by_cat("strategy", LL_INFO, "{}Async worker stopped", _name_with_prefix);
}

void WtOptContext::enqueue_tick(const char* stdCode, WTSTickData* newTick)
{
    if (!_worker_running) return;
    
    uint32_t optId = _grid ? _grid->getOptionId(stdCode) : UINT32_MAX;
    if (_task_queue.push(AsyncEvent::make_tick(optId, newTick))) {
        _total_events.fetch_add(1, std::memory_order_relaxed);
        _worker_cv.notify_one();
    } else {
        _queue_drops.fetch_add(1, std::memory_order_relaxed);
    }
}

void WtOptContext::enqueue_timer(uint32_t curDate, uint32_t curTime)
{
    if (!_worker_running) return;
    if (_task_queue.push(AsyncEvent::make_timer(curDate, curTime))) {
        _total_events.fetch_add(1, std::memory_order_relaxed);
        _worker_cv.notify_one();
    } else {
        _queue_drops.fetch_add(1, std::memory_order_relaxed);
    }
}

void WtOptContext::enqueue_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
    if (!_worker_running) return;
    uint32_t optId = _grid ? _grid->getOptionId(stdCode) : UINT32_MAX;
    if (_task_queue.push(AsyncEvent::make_trade(optId, localid, isBuy, vol, price))) {
        _total_events.fetch_add(1, std::memory_order_relaxed);
        _worker_cv.notify_one();
    } else {
        _queue_drops.fetch_add(1, std::memory_order_relaxed);
    }
}

void WtOptContext::enqueue_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled)
{
    if (!_worker_running) return;
    uint32_t optId = _grid ? _grid->getOptionId(stdCode) : UINT32_MAX;
    if (_task_queue.push(AsyncEvent::make_order(optId, localid, isBuy, totalQty, leftQty, price, isCanceled))) {
        _total_events.fetch_add(1, std::memory_order_relaxed);
        _worker_cv.notify_one();
    } else {
        _queue_drops.fetch_add(1, std::memory_order_relaxed);
    }
}

void WtOptContext::worker_loop()
{
    while (_worker_running) {
        // Bottleneck Monitor 
        size_t available = _task_queue.read_available();
        if (available > 3200) { // ~78% saturation of 4096 capacity
            uint64_t now = TimeUtils::getLocalTimeNow();
            if (now - _last_warning_time > 5000) { // log at most every 5s
                stra_log_warn(fmt::format("WT OPTION SPSC QUEUE BOTTLENECK REACHED! Queue saturation: {}/4096 slots used. Registered Drops: {}", available, _queue_drops.load()).c_str());
                _last_warning_time = now;
            }
        }
        
        std::vector<AsyncEvent> events;
        events.reserve(128); // Pre-allocate for batch
        
        // 1. Drain Queue (Lock-Free)
        AsyncEvent ev;
        while (_task_queue.pop(ev)) {
            events.push_back(ev);
            if (events.size() >= 1024) break; // Limit batch size
        }
        
        // 2. Wait if empty (Hybrid Strategy: could add spin here)
        if (events.empty()) {
            std::unique_lock<std::mutex> lock(_worker_mtx);
            _worker_cv.wait(lock, [this]() { 
                return !_worker_running || _task_queue.read_available() > 0; 
            });
            
            if (!_worker_running) break;
            
            // Try pop again after wake up
            while (_task_queue.pop(ev)) {
                events.push_back(ev);
                if (events.size() >= 1024) break;
            }
        }
        
        if (events.empty()) continue;
        
        // Batch Processing & Conflation
        bool has_tick = false;
        std::map<uint32_t, WTSTickData*> active_ticks; // Keep track of latest tick per optId

        for (auto& event : events) {
            const char* stdCode = "";
            if (event.optId == UINT32_MAX) {
                if (_grid) stdCode = _grid->getUnderlyingCode().c_str();
            } else if (_grid) {
                auto opt = _grid->getOption(event.optId);
                if (opt) stdCode = opt->getCode().c_str();
            }
            
            if (event.type == AsyncEvent::Tick) {
                WTSTickData* curTick = (WTSTickData*)&event.tick; // Cast WTSTickStruct back to WTSTickData
                if (_grid && curTick) {
                    _grid->onTick(stdCode, curTick->price(), curTick->bidprice(0), curTick->askprice(0), curTick->bidqty(0), curTick->askqty(0));
                }
                
                active_ticks[event.optId] = curTick;
                has_tick = true;
            } else if (event.type == AsyncEvent::Timer) {
                if (_stra) _stra->on_calculate(this, event.timer.date, event.timer.time);
            } else if (event.type == AsyncEvent::Trade) {
                if (_order_mgr) {
                    _order_mgr->onFill(stdCode, event.trade.localid, event.trade.price, (uint32_t)event.trade.vol, TimeUtils::getLocalTimeNow());
                }
            } else if (event.type == AsyncEvent::Order) {
                if (_order_mgr) {
                    wt_option::OrderState state = wt_option::OrderState::Sent;
                    if (event.order.isCanceled) {
                        state = wt_option::OrderState::Cancelled;
                    } else if (event.order.leftQty <= 0) {
                        state = wt_option::OrderState::Filled;
                    } else if (event.order.leftQty < event.order.totalQty) {
                        state = wt_option::OrderState::PartialFill;
                    }
                    _order_mgr->onOrderStatus(stdCode, event.order.localid, state);
                }
            }
        }
        
        if (has_tick) {
            // 2. Compute values (now delegated to Strategy)
            if (_stra) {
                _stra->on_tick_batch(this);
            }

            // 3. Strategy Logic
            // Call on_tick for each instrument that had an update (using its latest tick)
            if (_stra) {
                for (auto& pair : active_ticks) {
                    const char* code = "";
                    if (pair.first == UINT32_MAX) {
                        if (_grid) code = _grid->getUnderlyingCode().c_str();
                    } else if (_grid) {
                        auto opt = _grid->getOption(pair.first);
                        if (opt) code = opt->getCode().c_str();
                    }
                    _stra->on_tick(this, code, pair.second);
                }
            }
            
            // 4. Update Risk ONCE
            if (_risk) {
                _risk->update();
            }
            
            // 5. Update Orders ONCE (Execution)
            update_orders();
        }
    }
}

void WtOptContext::update_orders()
{
    if (!_grid || !_order_mgr || !_engine) return;
    
    // Direct Execution for Low Latency
    // Avoid ThreadPool overhead (allocation, locking, scheduling)
    // Execute sequentially in the worker thread (hot cache)

    auto& expiries = _grid->getExpiries();
    for (auto& pair : expiries) {
        auto expiryData = pair.second;
        if (!expiryData) continue;
        
        // Update underlying orders
        auto undData = expiryData->getUnderlyingTradingData();
        if (undData) {
            auto mgr = _order_mgr->getUnderlyingOrderManager(undData->getCode());
            if (mgr) {
                wt_option::MultiLevelQuote quote;
                auto& v = undData->values();
                if (v.ourBid > 0 && v.ourBidSize > 0) {
                    quote.bids.push_back({v.ourBid, v.ourBidSize});
                }
                if (v.ourAsk > 0 && v.ourAskSize > 0) {
                    quote.asks.push_back({v.ourAsk, v.ourAskSize});
                }
                mgr->setDesiredQuote(quote);
                mgr->updateOrders(false);
            }
        }
        
        // Update option orders
        for (const auto& sPair : expiryData->getStrikes()) {
            auto sData = sPair.second;
            
            // Lambda to process an option (Call/Put)
            auto processOption = [this](wt_option::OptionDataPtr opt) {
                if (!opt) return;
                auto mgr = _order_mgr->getOrderManager(opt);
                if (mgr) {
                    wt_option::MultiLevelQuote quote;
                    auto& v = opt->values();
                    if (v.ourBid > 0 && v.ourBidSize > 0) {
                        quote.bids.push_back({v.ourBid, v.ourBidSize});
                    }
                    if (v.ourAsk > 0 && v.ourAskSize > 0) {
                        quote.asks.push_back({v.ourAsk, v.ourAskSize});
                    }
                    mgr->setDesiredQuote(quote);
                    mgr->updateOrders(false);
                }
            };
            
            processOption(sData->call());
            processOption(sData->put());
        }
    }
}

//=============================================================================
// Checkpoint / Recovery Data
//=============================================================================

void WtOptContext::save_data()
{
    if (!_grid || !_order_mgr) return;
    
    rapidjson::Document doc;
    doc.SetObject();
    rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();
    
    rapidjson::Value jPositions(rapidjson::kObjectType);
    rapidjson::Value jOrders(rapidjson::kArrayType);
    
    auto processMgr = [&](wt_option::IOrderManager* mgr, const std::string& code) {
        if (!mgr) return;
        
        // Save Position
        wt_option::OptionOrderManager* optMgr = dynamic_cast<wt_option::OptionOrderManager*>(mgr);
        wt_option::UnderlyingOrderManager* undMgr = dynamic_cast<wt_option::UnderlyingOrderManager*>(mgr);
        
        int32_t pos = 0;
        if (optMgr) pos = optMgr->getPosition();
        else if (undMgr) pos = undMgr->getPosition();
        
        if (pos != 0) {
            rapidjson::Value jCode(code.c_str(), allocator);
            jPositions.AddMember(jCode, pos, allocator);
        }
        
        // Save Active Orders
        auto activeOrders = mgr->getActiveOrders();
        for (auto& order : activeOrders) {
            if (!order) continue;
            rapidjson::Value jOrd(rapidjson::kObjectType);
            rapidjson::Value jC(order->getCode().c_str(), allocator);
            jOrd.AddMember("code", jC, allocator);
            jOrd.AddMember("localid", order->getOrderId(), allocator);
            
            rapidjson::Value jEntrust(order->getEntrustNo().c_str(), allocator); 
            jOrd.AddMember("entrustNo", jEntrust, allocator);
            
            jOrd.AddMember("price", order->getPrice(), allocator);
            jOrd.AddMember("qty", order->getQuantity(), allocator);
            jOrd.AddMember("left_qty", order->getQuantity(), allocator); // Original leftQuantity not preserved in legacy structures
            jOrd.AddMember("dir", (int)order->getDirection(), allocator);
            jOrders.PushBack(jOrd, allocator);
        }
    };
    
    auto& expiries = _grid->getExpiries();
    for (auto& pair : expiries) {
        auto expiryData = pair.second;
        if (!expiryData) continue;
        
        auto undData = expiryData->getUnderlyingTradingData();
        if (undData) {
            auto mgr = _order_mgr->getUnderlyingOrderManager(undData->getCode());
            if (mgr) processMgr(mgr.get(), undData->getCode());
        }
        
        for (const auto& sPair : expiryData->getStrikes()) {
            auto sData = sPair.second;
            if (sData->call()) {
                auto mgr = _order_mgr->getOrderManager(sData->call());
                if (mgr) processMgr(mgr.get(), sData->call()->getCode());
            }
            if (sData->put()) {
                auto mgr = _order_mgr->getOrderManager(sData->put());
                if (mgr) processMgr(mgr.get(), sData->put()->getCode());
            }
        }
    }
    
    doc.AddMember("positions", jPositions, allocator);
    doc.AddMember("orders", jOrders, allocator);
    
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    
    std::string filename = fmt::format("WtOptContext_{}_data.json", _id);
    std::ofstream out(filename);
    if (out.is_open()) {
        out << buffer.GetString();
        out.close();
        stra_log_info(fmt::format("Context recovery data saved to {}", filename).c_str());
    }
}

void WtOptContext::load_data()
{
    if (!_order_mgr) return;
    
    std::string filename = fmt::format("WtOptContext_{}_data.json", _id);
    std::ifstream in(filename);
    if (!in.is_open()) return;
    
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    
    rapidjson::Document doc;
    doc.Parse(content.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        stra_log_warn("Failed to parse recovery json data.");
        return;
    }
    
    if (doc.HasMember("positions") && doc["positions"].IsObject()) {
        const auto& positions = doc["positions"];
        for (auto itr = positions.MemberBegin(); itr != positions.MemberEnd(); ++itr) {
            std::string code = itr->name.GetString();
            int32_t pos = itr->value.GetInt();
            
            auto mgr = _order_mgr->getOrderManager(code);
            if (!mgr) {
                auto uMgr = _order_mgr->getUnderlyingOrderManager(code);
                if (uMgr) {
                    uMgr->setPositionOffset(pos); // Recover as offset to combine with live queries
                }
            } else {
                mgr->setPositionOffset(pos);
            }
        }
    }
    
    // Rebuild orders
    if (doc.HasMember("orders") && doc["orders"].IsArray()) {
        const auto& orders = doc["orders"];
        for (rapidjson::SizeType i = 0; i < orders.Size(); i++) {
            const auto& jOrd = orders[i];
            
            uint32_t orderId = jOrd["localid"].GetUint();
            std::string code = jOrd["code"].GetString();
            wt_option::OrderDir dir = (wt_option::OrderDir)jOrd["dir"].GetInt();
            double price = jOrd["price"].GetDouble();
            uint32_t qty = jOrd["qty"].GetUint();
            
            auto order = std::make_shared<wt_option::OptionOrder>(orderId, code, dir, price, qty);
            
            if (jOrd.HasMember("entrustNo")) {
                order->setEntrustNo(jOrd["entrustNo"].GetString());
            }
            
            uint32_t left_qty = jOrd["left_qty"].GetUint();
            // Assuming no direct setter for leftQuantity, we calculate filledQty
            if (left_qty < qty) {
                order->addFill(price, qty - left_qty, 0); 
            }
            
            order->setState(wt_option::OrderState::Sent); // Assume active if saved
            
            _order_mgr->injectOrder(order);
        }
        stra_log_info(fmt::format("Recovered {} orders from checkpoint.", doc["orders"].Size()).c_str());
    }
    
    stra_log_info("Context recovery data loaded successfully.");
}
