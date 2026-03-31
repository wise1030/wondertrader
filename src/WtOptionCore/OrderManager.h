/*!
 * \file OrderManager.h
 * \brief Order management for option trading
 * 
 * Migrated from longbeach/quantbox/strategy/optioncore/OptionTradingData.h
 * and stratlib/IOrderManager.h
 */

#pragma once

#include "BaseOrder.h"
#include "OptionOrder.h"
#include "OptionData.h"
#include "UnderlyingTradingData.h"
#include <map>
#include <vector>
#include <functional>
#include <mutex>
#include <set>
#include <boost/pool/object_pool.hpp>

namespace wt_option {

// Forward decl
struct OrderManagerParams;

struct MarketQuote {
    double bidPrice = 0.0;
    double askPrice = 0.0;
    int32_t bidSize = 0;
    int32_t askSize = 0;
};

/**
 * @brief Multi-level market quote (mimics MultiMarket)
 */
struct MultiLevelQuote {
    // Memory pooling optimizations
    static void* operator new(size_t size);
    static void operator delete(void* ptr);

    struct Level {
        double price = 0;
        int32_t size = 0;
        bool isValid() const { return size > 0 && price > 0; }
    };
    
    std::vector<Level> bids;
    std::vector<Level> asks;
    
    void clear() { bids.clear(); asks.clear(); }
    bool hasLevels() const { return !bids.empty() || !asks.empty(); }
    
    Level getBestBid() const { return bids.empty() ? Level() : bids.front(); }
    Level getBestAsk() const { return asks.empty() ? Level() : asks.front(); }
};

/**
 * @brief Order manager parameters (config)
 */
struct OrderManagerParams {
    int32_t max_contracts = 500;
    int32_t max_cancels = 500;
    int32_t max_order_size = 20;
    int32_t min_order_size = 1;
    bool enable_ioc = false;
    bool enable_post_only = false;
    uint32_t cancel_retry_interval_ms = 400;
    uint32_t order_delay_ms = 0;
    uint32_t min_quote_update_interval_ms = 100; // Micro-tick filter
};

/**
 * @brief Trading statistics
 */
struct TradingStats {
    int32_t numOrders = 0;
    int32_t numFills = 0;
    int32_t numCancels = 0;
    int32_t numRejects = 0;
    double totalVolume = 0;
    double realizedPnL = 0;
    
    // Detailed stats
    int32_t cancelSent = 0;
    int32_t cancelDropped = 0;
    int32_t cancelInProgress = 0;
};

/**
 * @brief Order manager interface
 */
class IOrderManager {
public:
    virtual ~IOrderManager() = default;
    
    virtual uint32_t sendOrder(OptionDataPtr option, OrderDir dir, 
                               double price, uint32_t qty) = 0;
    virtual bool cancelOrder(uint32_t orderId) = 0;
    virtual bool cancelAll() = 0;
    
    virtual BaseOrderPtr getOrder(uint32_t orderId) = 0;
    virtual std::vector<BaseOrderPtr> getActiveOrders() = 0;
    virtual std::vector<BaseOrderPtr> getOrdersByOption(const std::string& code) = 0;
    virtual void injectOrder(BaseOrderPtr order) = 0;
    
    virtual void enable() = 0;
    virtual void disable() = 0;
    virtual bool isEnabled() const = 0;
    
    virtual const TradingStats& getStats() const = 0;
};

using IOrderManagerPtr = std::shared_ptr<IOrderManager>;
using OrderExecutor = std::function<bool(const BaseOrder& order)>;
using CancelExecutor = std::function<bool(uint32_t orderId)>;
using QuoteExecutor = std::function<uint32_t(const std::string& code, double bidPrice, uint32_t bidQty, double askPrice, uint32_t askQty)>;

/**
 * @brief Robust Option Order Manager
 * Matches longbeach DefaultOrderManager logic
 */
class OptionOrderManager : public IOrderManager {
public:
    OptionOrderManager(OptionDataPtr option, const OrderManagerParams& params = OrderManagerParams());
    
    // IOrderManager implementation
    uint32_t sendOrder(OptionDataPtr option, OrderDir dir,
                       double price, uint32_t qty) override;
    bool cancelOrder(uint32_t orderId) override;
    bool cancelAll() override;
    
    BaseOrderPtr getOrder(uint32_t orderId) override;
    std::vector<BaseOrderPtr> getActiveOrders() override;
    std::vector<BaseOrderPtr> getOrdersByOption(const std::string& code) override;
    void injectOrder(BaseOrderPtr order) override;
    
    void enable() override;
    void disable() override;
    bool isEnabled() const override { return m_enabled; }
    
    const TradingStats& getStats() const override { return m_stats; }
    
    // Market making interface (Now uses MultiLevelQuote)
    void setDesiredQuote(const MultiLevelQuote& quote);
    const MultiLevelQuote& getDesiredQuote() const { return m_desiredQuote; }
    
    // Compatibility accessor for simple quotes
    MarketQuote getCurrentQuoteLegacy() const;
    MarketQuote getDesiredQuoteLegacy() const;
    
    // Core update loop
    // Returns number of transactions generated
    int32_t updateOrders(bool cancelOnly = false);
    
    // Set executors
    void setOrderExecutor(OrderExecutor executor) { m_orderExecutor = executor; }
    void setCancelExecutor(CancelExecutor executor) { m_cancelExecutor = executor; }
    void setQuoteExecutor(QuoteExecutor executor) { m_quoteExecutor = executor; }
    
    // Callbacks
    void onFill(uint32_t orderId, double fillPrice, uint32_t fillQty, uint64_t fillTime);
    void onCancel(uint32_t orderId);
    void onReject(uint32_t orderId, const std::string& reason);
    void onOrderStatusChange(uint32_t orderId, OrderState newState);
    
    void addListener(IOrderListener* listener);
    void removeListener(IOrderListener* listener);
    
    // Position tracking
    int32_t getPosition() const { return m_position; }
    void setPositionOffset(int32_t offset) { m_positionOffset = offset; }
    int32_t getAbsolutePosition() const { return m_position + m_positionOffset; }
    
    OptionDataPtr getOption() const { return m_option.lock(); }
    
protected:
    // Internal logic helpers
    void checkRetryLogin();
    void retryUpdateOrders();
    bool __sendCancel(BaseOrderPtr order);
    void __delayedCancel(BaseOrderPtr order);
    
    /**
     * @brief Smart order diffing
     * Returns size needed at price level (positive = add, negative = cancel)
     */
    int32_t getMissingPriceLevelSize(double price, uint32_t desiredSize, OrderDir dir);
    
    // Cancel strategies
    void cancelByPriceRange(double minPrice, double maxPrice, OrderDir dir); 
    int32_t partialCancelByPriceRange(int32_t sizeToCancel, double minPrice, double maxPrice, OrderDir dir);
    
private:
    uint32_t generateOrderId();
    void notifyListeners(const std::function<void(IOrderListener*)>& op);

    std::weak_ptr<OptionData> m_option;
    std::string m_code;
    OrderManagerParams m_params;
    
    std::map<uint32_t, OptionOrderPtr> m_orders;
    std::vector<uint32_t> m_activeOrderIds;
    
    // State tracking
    MultiLevelQuote m_desiredQuote;
    bool m_enabled;
    int32_t m_position;
    int32_t m_positionOffset;
    
    TradingStats m_stats;
    
    OrderExecutor m_orderExecutor;
    CancelExecutor m_cancelExecutor;
    QuoteExecutor m_quoteExecutor;
    std::vector<IOrderListener*> m_listeners;
    
    uint32_t m_nextOrderId;
    
    // Retry/Wait logic
    uint64_t m_lastUpdateTime;
    uint64_t m_rejectRetryTime;
    uint64_t m_lastQuoteTime;
    std::set<uint32_t> m_pendingCancels;
};

using OptionOrderManagerPtr = std::shared_ptr<OptionOrderManager>;

class UnderlyingTradingData;
using UnderlyingTradingDataPtr = std::shared_ptr<UnderlyingTradingData>;

/**
 * @brief Order manager for Underlying
 */
class UnderlyingOrderManager : public IOrderManager {
public:
    UnderlyingOrderManager(UnderlyingTradingDataPtr data, const OrderManagerParams& params = OrderManagerParams());

    // IOrderManager implementation
    uint32_t sendOrder(OptionDataPtr option, OrderDir dir, double price, uint32_t qty) override { return 0; } // Not used for OptionData
    uint32_t sendOrder(OrderDir dir, double price, uint32_t qty); 
    
    bool cancelOrder(uint32_t orderId) override;
    bool cancelAll() override;

    BaseOrderPtr getOrder(uint32_t orderId) override;
    std::vector<BaseOrderPtr> getActiveOrders() override;
    std::vector<BaseOrderPtr> getOrdersByOption(const std::string& code) override;
    void injectOrder(BaseOrderPtr order) override;

    void enable() override;
    void disable() override;
    bool isEnabled() const override { return m_enabled; }

    const TradingStats& getStats() const override { return m_stats; }
    
     // Market making interface 
    void setDesiredQuote(const MultiLevelQuote& quote);
    
    // Position tracking
    int32_t getPosition() const { return m_position + m_positionOffset; }
    void setPositionOffset(int32_t offset) { m_positionOffset = offset; }
    
    int32_t updateOrders(bool cancelOnly = false);
    
    // Callbacks
    void onFill(uint32_t orderId, double fillPrice, uint32_t fillQty, uint64_t fillTime);
    void onCancel(uint32_t orderId);
    void onReject(uint32_t orderId, const std::string& reason);
    void onOrderStatusChange(uint32_t orderId, OrderState newState);
    
    void setOrderExecutor(OrderExecutor executor) { m_orderExecutor = executor; }
    void setCancelExecutor(CancelExecutor executor) { m_cancelExecutor = executor; }
    void setQuoteExecutor(QuoteExecutor executor) { m_quoteExecutor = executor; }
    
private:
    uint32_t generateOrderId();

    std::weak_ptr<UnderlyingTradingData> m_data;
    std::string m_code;
    OrderManagerParams m_params;

    std::map<uint32_t, BaseOrderPtr> m_orders;
    std::vector<uint32_t> m_activeOrderIds;

    MultiLevelQuote m_desiredQuote;
    bool m_enabled;
    int32_t m_position;
    int32_t m_positionOffset;
    
    TradingStats m_stats;
    
    OrderExecutor m_orderExecutor;
    CancelExecutor m_cancelExecutor;
    QuoteExecutor m_quoteExecutor;
    
    uint32_t m_nextOrderId;
    uint64_t m_lastQuoteTime;
};

using UnderlyingOrderManagerPtr = std::shared_ptr<UnderlyingOrderManager>;

/**
 * @brief Grid-level order manager
 * 
 * Manages orders across all options in the grid.
 */
class GridOrderManager {
public:
    GridOrderManager();
    
    // Get or create order manager for option
    OptionOrderManagerPtr getOrderManager(OptionDataPtr option);
    OptionOrderManagerPtr getOrderManager(const std::string& code);
    
    // Grid-level operations
    void enableAll();
    void disableAll();
    bool cancelAll();
    uint32_t cancelOrdersByCode(const std::string& code);
    
    // Update all order managers
    int32_t updateAllOrders(bool cancelOnly = false);
    
    // Recovery mechanism
    void injectOrder(BaseOrderPtr order);
    
    // Aggregate statistics
    TradingStats getAggregateStats() const;
    
    // Position queries
    int32_t getTotalPosition() const;
    int32_t getPositionByExpiry(uint32_t expiry) const;
    
    // Fill routing
    void onFill(const std::string& code, uint32_t orderId, 
                double fillPrice, uint32_t fillQty, uint64_t fillTime);
    void onCancel(const std::string& code, uint32_t orderId);
    void onReject(const std::string& code, uint32_t orderId, const std::string& reason);
    void onOrderStatus(const std::string& code, uint32_t orderId, OrderState newState);
    
    // Set global executors
    void setOrderExecutor(OrderExecutor executor);
    void setCancelExecutor(CancelExecutor executor);
    void setQuoteExecutor(QuoteExecutor executor);
    
    // Underlying support
    UnderlyingOrderManagerPtr getUnderlyingOrderManager(const std::string& code);
    
    // Session callbacks
    void onSessionBegin(uint32_t tradingDate);
    void onSessionEnd(uint32_t tradingDate);
    
private:
    std::map<std::string, OptionOrderManagerPtr> m_managers;
    std::vector<OptionOrderManagerPtr> m_managersByIndex; // Fast mapping
    std::map<std::string, UnderlyingOrderManagerPtr> m_underlyingManagers;
    OrderExecutor m_globalOrderExecutor;
    CancelExecutor m_globalCancelExecutor;
    QuoteExecutor m_globalQuoteExecutor;
};

using GridOrderManagerPtr = std::shared_ptr<GridOrderManager>;

} // namespace wt_option
