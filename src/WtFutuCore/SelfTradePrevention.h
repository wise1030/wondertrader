/*!
 * \file SelfTradePrevention.h
 * \brief Self-Trade Prevention Module for Combined Market Making and Spread Arbitrage
 * 
 * Provides self-trade detection and prevention between:
 *   - Market making orders (limit orders on both sides)
 *   - Spread arbitrage orders (cross-leg trades)
 * 
 * Prevention strategies:
 *   - Price check: Reject if arbitrage order would cross MM orders
 *   - Cancel first: Cancel conflicting MM orders before placing arbitrage orders
 *   - Adjust price: Adjust arbitrage order price to avoid crossing
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include "../Includes/FasterDefs.h"
#include <string>
#include <vector>
#include <cstdint>

namespace futu {

//==============================================================================
// Active Order Info
//==============================================================================

struct ActiveOrder
{
    std::string code;           ///< Contract code
    uint32_t order_id;          ///< Order ID
    double price;               ///< Order price
    double qty;                 ///< Remaining quantity
    bool is_buy;                ///< true = buy, false = sell
    uint64_t timestamp;         ///< Order timestamp
    
    ActiveOrder()
        : order_id(0), price(0), qty(0), is_buy(true), timestamp(0)
    {}
    
    ActiveOrder(const std::string& c, uint32_t id, double p, double q, bool buy, uint64_t ts)
        : code(c), order_id(id), price(p), qty(q), is_buy(buy), timestamp(ts)
    {}
};

//==============================================================================
// Arbitrage Order Request
//==============================================================================

struct ArbitrageOrderRequest
{
    std::string code;           ///< Contract code
    bool is_buy;                ///< Direction
    double price;               ///< Requested price (0 = market order)
    double qty;                 ///< Quantity
    bool is_market_order;       ///< Is this a market order?
    
    ArbitrageOrderRequest()
        : price(0), qty(0), is_buy(true), is_market_order(false)
    {}
};

//==============================================================================
// Self-Trade Check Result
//==============================================================================

struct SelfTradeCheckResult
{
    bool has_risk;              ///< Has self-trade risk
    std::string risk_code;      ///< Code with risk
    double conflict_price;      ///< Price level of conflict
    double conflict_qty;        ///< Quantity at risk
    
    // Conflicting orders
    std::vector<uint32_t> conflicting_order_ids;
    
    // Recommended action
    enum class Action : uint8_t
    {
        ALLOW,                  ///< No conflict, allow order
        REJECT,                 ///< Reject arbitrage order
        CANCEL_FIRST,           ///< Cancel MM orders first
        ADJUST_PRICE            ///< Adjust arbitrage price
    } recommended_action;
    
    double adjusted_price;      ///< Adjusted price if ADJUST_PRICE
    
    SelfTradeCheckResult()
        : has_risk(false)
        , conflict_price(0)
        , conflict_qty(0)
        , recommended_action(Action::ALLOW)
        , adjusted_price(0)
    {}
};

//==============================================================================
// Self-Trade Prevention Configuration
//==============================================================================

struct StpConfig
{
    bool enabled;               ///< Enable self-trade prevention
    bool allow_same_price;      ///< Allow orders at same price (no cross)
    double min_price_gap;       ///< Minimum price gap (in ticks)
    
    // Prevention strategy
    enum class Strategy : uint8_t
    {
        REJECT_ARB,             ///< Reject arbitrage order
        CANCEL_MM,              ///< Cancel MM orders first
        ADJUST_ARB_PRICE        ///< Adjust arbitrage price
    } strategy;
    
    // Price adjustment
    double price_adjust_ticks;  ///< How many ticks to adjust
    
    StpConfig()
        : enabled(true)
        , allow_same_price(false)
        , min_price_gap(1.0)
        , strategy(Strategy::CANCEL_MM)
        , price_adjust_ticks(1.0)
    {}
};

//==============================================================================
// Self-Trade Prevention Manager
//==============================================================================

class SelfTradePrevention
{
public:
    SelfTradePrevention();
    ~SelfTradePrevention() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const StpConfig& config) { _config = config; }
    const StpConfig& getConfig() const { return _config; }
    
    //==========================================================================
    // Order Tracking
    //==========================================================================
    
    /// Track a new market making order
    void trackMMOrder(const std::string& code, uint32_t order_id, 
                      double price, double qty, bool is_buy, uint64_t timestamp);
    
    /// Untrack an order (filled or canceled)
    void untrackOrder(uint32_t order_id);
    
    /// Update order quantity
    void updateOrderQty(uint32_t order_id, double new_qty);
    
    /// Clear all tracked orders
    void clear();
    
    //==========================================================================
    // Self-Trade Detection
    //==========================================================================
    
    /// Check if an arbitrage order would cause self-trade
    SelfTradeCheckResult checkArbitrageOrder(const ArbitrageOrderRequest& request) const;
    
    /// Check for a specific contract
    SelfTradeCheckResult checkOrder(const std::string& code, bool is_buy, 
                                     double price, bool is_market_order = false) const;
    
    //==========================================================================
    // Query
    //==========================================================================
    
    /// Get all active MM buy orders for a contract
    std::vector<ActiveOrder> getMMBuyOrders(const std::string& code) const;
    
    /// Get all active MM sell orders for a contract
    std::vector<ActiveOrder> getMMSellOrders(const std::string& code) const;
    
    /// Get best MM buy price for a contract
    double getBestMMBuy(const std::string& code) const;
    
    /// Get best MM sell price for a contract
    double getBestMMSell(const std::string& code) const;
    
    /// Check if contract has active MM orders
    bool hasMMOrders(const std::string& code) const;
    
    //==========================================================================
    // Statistics
    //==========================================================================
    
    size_t totalOrders() const { return _orders_by_id.size(); }
    size_t ordersForContract(const std::string& code) const;
    
private:
    //==========================================================================
    // Internal Methods
    //==========================================================================
    
    double computeAdjustedPrice(double original_price, bool is_buy) const;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    StpConfig _config;
    
    //==========================================================================
    // Order Storage
    //==========================================================================
    
    // All orders by order ID
    wtp::wt_hashmap<uint32_t, ActiveOrder> _orders_by_id;
    
    // Orders by contract code
    // For buy orders: map<code, map<order_id, ActiveOrder>>
    // For sell orders: same structure
    wtp::wt_hashmap<std::string, wtp::wt_hashmap<uint32_t, ActiveOrder>> _buy_orders_by_code;
    wtp::wt_hashmap<std::string, wtp::wt_hashmap<uint32_t, ActiveOrder>> _sell_orders_by_code;
    
    // Best prices cache
    wtp::wt_hashmap<std::string, double> _best_buy_price;
    wtp::wt_hashmap<std::string, double> _best_sell_price;
};

} // namespace futu
