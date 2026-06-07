/*!
 * \file SelfTradePrevention.h
 * \brief Self-Trade Prevention Module (UnifiedOrderTracker Wrapper)
 * 
 * Provides self-trade detection and prevention between:
 *   - Market making orders (limit orders on both sides)
 *   - Spread arbitrage orders (cross-leg trades)
 * 
 * This is now a lightweight wrapper around UnifiedOrderTracker.
 * All order tracking is delegated to the unified tracker for:
 *   - Single source of truth
 *   - Better memory efficiency
 *   - Consistent statistics
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include "FutuConfig.h"
#include "UnifiedOrderTracker.h"
#include <string>
#include <vector>
#include <cstdint>

namespace futu {

//==============================================================================
// Legacy Types (for API compatibility)
//==============================================================================

/// Legacy active order info (for getMMBuyOrders/getMMSellOrders API)
struct ActiveOrder
{
    std::string code;
    uint32_t order_id;
    double price;
    double qty;
    bool is_buy;
    uint64_t timestamp;
    
    ActiveOrder()
        : order_id(0), price(0), qty(0), is_buy(true), timestamp(0) {}
    
    ActiveOrder(const std::string& c, uint32_t id, double p, double q, bool buy, uint64_t ts)
        : code(c), order_id(id), price(p), qty(q), is_buy(buy), timestamp(ts) {}
};

// Note: ArbitrageOrderRequest and SelfTradeCheckResult are defined in UnifiedOrderTracker.h
// We reuse those definitions via the include above.

/// Legacy configuration
struct StpConfig
{
    bool enabled;
    bool allow_same_price;
    double min_price_gap;
    
    enum class Strategy : uint8_t
    {
        REJECT_ARB,
        CANCEL_MM,
        ADJUST_ARB_PRICE
    } strategy;
    
    double price_adjust_ticks;
    
    StpConfig()
        : enabled(true), allow_same_price(false), min_price_gap(1.0),
          strategy(Strategy::CANCEL_MM), price_adjust_ticks(1.0) {}
    
    static StpConfig fromVariant(wtp::WTSVariant* v) {
        StpConfig c;
        c.enabled = FutuConfig::readBool(v, "enabled", true);
        c.min_price_gap = FutuConfig::readDouble(v, "minPriceGap", 1.0);
        c.allow_same_price = FutuConfig::readBool(v, "allowSamePrice", false);
        c.price_adjust_ticks = FutuConfig::readDouble(v, "priceAdjustTicks", 1.0);
        return c;
    }
};

//==============================================================================
// Self-Trade Prevention Manager (UnifiedOrderTracker Wrapper)
//==============================================================================

class SelfTradePrevention
{
public:
    SelfTradePrevention() : _tracker(nullptr) {}
    ~SelfTradePrevention() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const StpConfig& config) { _config = config; }
    const StpConfig& getConfig() const { return _config; }
    
    /// Set the unified order tracker (must be called before use)
    void setUnifiedTracker(UnifiedOrderTracker* tracker) { _tracker = tracker; }
    UnifiedOrderTracker* getUnifiedTracker() const { return _tracker; }
    
    //==========================================================================
    // Order Tracking (delegated to UnifiedOrderTracker)
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
    
    size_t totalOrders() const;
    size_t ordersForContract(const std::string& code) const;

private:
    StpConfig _config;
    UnifiedOrderTracker* _tracker;  ///< Not owned, shared with strategy
};

} // namespace futu
