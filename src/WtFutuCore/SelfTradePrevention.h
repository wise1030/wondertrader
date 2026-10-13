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

namespace futu
{

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

    ActiveOrder() : order_id(0), price(0), qty(0), is_buy(true), timestamp(0) {}

    ActiveOrder(const std::string& c, uint32_t id, double p, double q, bool buy, uint64_t ts)
        : code(c), order_id(id), price(p), qty(q), is_buy(buy), timestamp(ts)
    {}
};

// Note: ArbitrageOrderRequest and SelfTradeCheckResult are defined in UnifiedOrderTracker.h
// We reuse those definitions via the include above.

/// Legacy configuration
struct StpConfig
{
    bool enabled;

    // 2026-09-19③ 批次E: 删除 allow_same_price/price_adjust_ticks/strategy/min_price_gap
    // 四个死字段 —— 实际判定链 UnifiedOrderTracker::checkSelfTrade 只读 stp_enabled
    // (价格比较 <=/>= 天然含同价), 价差/策略/同价容忍语义从未实现 (DIAGNOSTIC_REPORT_V10
    // 已预警 "配了不生效, 最误导"); stpMinPriceGap 死链: coordinator.yaml→StpConfig→
    // _config.modules→UnifiedTrackerConfig, 三层传递零消费。

    StpConfig() : enabled(true) {}

    static StpConfig fromVariant(wtp::WTSVariant* v)
    {
        StpConfig c;
        c.enabled = FutuConfig::readBool(v, "enabled", true);
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

    //==========================================================================
    // Order Tracking (delegated to UnifiedOrderTracker)
    //==========================================================================

    /// Track a new market making order
    void
    trackMMOrder(const std::string& code, uint32_t order_id, double price, double qty, bool is_buy, uint64_t timestamp);

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
    SelfTradeCheckResult
    checkOrder(const std::string& code, bool is_buy, double price, bool is_market_order = false) const;

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

private:
    StpConfig _config;
    UnifiedOrderTracker* _tracker; ///< Not owned, shared with strategy
};

} // namespace futu
