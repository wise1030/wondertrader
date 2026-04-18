/*!
 * \file UnifiedOrderTracker.h
 * \brief Unified Order Tracking for High-Frequency Market Making
 * 
 * Single source of truth for order state, combining:
 *   - Order lifecycle management (from legacy SharedOrderTracker)
 *   - Self-trade detection (from legacy SelfTradePrevention)
 *   - Single source of truth for all order state
 *   - O(1) lookup by order ID
 *   - Efficient per-contract iteration
 *   - Self-trade detection between MM and arbitrage orders
 * 
 * Performance optimizations:
 *   - Continuous memory layout (vector-based)
 *   - Lightweight per-contract indices (only order IDs)
 *   - Cache-friendly iteration
 *   - Inline access methods
 * 
 * Thread Safety: NOT thread-safe. Must be called from single thread
 * or use external synchronization.
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <chrono>
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN
class IUftStraCtx;
NS_WTP_END

namespace futu {

//==============================================================================
// Order Flags
//==============================================================================

/// Order flags for state tracking
enum class OrderFlags : uint8_t
{
    NONE           = 0,
    PENDING_CANCEL = 1 << 0,
    IS_BID         = 1 << 1,    // Buy order
    IS_ACTIVE      = 1 << 2,
    IS_MM_ORDER    = 1 << 3,    // Market making order (vs arbitrage)
    IS_ARB_ORDER   = 1 << 4    // Arbitrage order
};

inline OrderFlags operator|(OrderFlags a, OrderFlags b) {
    return static_cast<OrderFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline OrderFlags operator&(OrderFlags a, OrderFlags b) {
    return static_cast<OrderFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline bool hasFlag(OrderFlags flags, OrderFlags flag) {
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(flag)) != 0;
}

//==============================================================================
// Cancel Reason
//==============================================================================

/// Cancellation reason - for analysis and statistics
enum class CancelReason : uint8_t
{
    NONE,
    PRICE_DEVIATION,    ///< Price deviated from mid beyond threshold
    STALE,              ///< Order age exceeded max_age_ms
    TIMEOUT,            ///< Order not filled within time limit
    STATE_CHANGE,       ///< Market state changed (volatility, etc.)
    INVENTORY_LIMIT,    ///< Inventory limit breached
    RISK_BREACH,        ///< Risk limit breach
    MARKET_STATE_CHANGE,///< Market state changed to abnormal
    CLOSEOUT,           ///< Closeout before session end
    SELF_TRADE,         ///< Self-trade prevention triggered
    MANUAL,             ///< Manual cancellation
    COUNT               ///< Number of cancel reasons (for stats array)
};

inline const char* cancelReasonToString(CancelReason reason)
{
    static const char* names[] = {
        "NONE", "PRICE_DEVIATION", "STALE", "TIMEOUT", "STATE_CHANGE",
        "INVENTORY_LIMIT", "RISK_BREACH", "MARKET_STATE_CHANGE", "CLOSEOUT",
        "SELF_TRADE", "MANUAL"
    };
    size_t idx = static_cast<size_t>(reason);
    if (idx < sizeof(names) / sizeof(names[0])) return names[idx];
    return "UNKNOWN";
}

//==============================================================================
// Order Information
//==============================================================================

/// Fixed-size code buffer to avoid dynamic allocation
constexpr size_t MAX_CODE_LEN = 32;

/// Unified order information
struct UnifiedOrderInfo
{
    uint32_t    order_id;
    uint32_t    level_index;    ///< Quote level (for MM orders)
    char        code[MAX_CODE_LEN];
    double      price;
    double      qty;
    double      place_mid;      ///< Mid price at placement
    uint64_t    place_time;
    uint64_t    last_check;
    uint64_t    last_inv_cancel_check;
    OrderFlags  flags;
    CancelReason cancel_reason;
    
    // Inline helpers
    inline bool isBid() const { return hasFlag(flags, OrderFlags::IS_BID); }
    inline bool isActive() const { return hasFlag(flags, OrderFlags::IS_ACTIVE); }
    inline bool isPendingCancel() const { return hasFlag(flags, OrderFlags::PENDING_CANCEL); }
    inline bool isMMOrder() const { return hasFlag(flags, OrderFlags::IS_MM_ORDER); }
    inline bool isArbOrder() const { return hasFlag(flags, OrderFlags::IS_ARB_ORDER); }
    
    inline void setPendingCancel(CancelReason reason) {
        flags = flags | OrderFlags::PENDING_CANCEL;
        cancel_reason = reason;
    }
    
    inline void clearPendingCancel() {
        flags = static_cast<OrderFlags>(static_cast<uint8_t>(flags) & ~static_cast<uint8_t>(OrderFlags::PENDING_CANCEL));
        cancel_reason = CancelReason::NONE;
    }
    
    UnifiedOrderInfo() : order_id(0), level_index(0), price(0), qty(0), place_mid(0),
        place_time(0), last_check(0), last_inv_cancel_check(0),
        flags(OrderFlags::NONE), cancel_reason(CancelReason::NONE) {
        memset(code, 0, MAX_CODE_LEN);
    }
};

//==============================================================================
// Configuration
//==============================================================================

struct UnifiedTrackerConfig
{
    uint32_t    max_orders;
    uint32_t    max_age_ms;
    double      price_deviation;
    double      sticky_threshold;
    uint32_t    inv_limit_cooldown_ms;
    uint32_t    max_cancel_rate;
    
    // Self-trade prevention
    bool        stp_enabled;
    bool        stp_allow_same_price;
    double      stp_min_price_gap;
    
    UnifiedTrackerConfig() 
        : max_orders(20), max_age_ms(10000), price_deviation(3.0),
          sticky_threshold(2.0), inv_limit_cooldown_ms(2000), max_cancel_rate(10),
          stp_enabled(true), stp_allow_same_price(false), stp_min_price_gap(1.0) {}
};

//==============================================================================
// Statistics
//==============================================================================

struct UnifiedTrackerStats
{
    uint32_t orders_placed;
    uint32_t orders_filled;
    uint32_t orders_canceled;
    uint32_t cancel_requests;
    uint32_t duplicate_cancels;
    
    // Per-reason cancel counts
    uint32_t price_deviation_cancels;
    uint32_t stale_cancels;
    uint32_t timeout_cancels;
    uint32_t state_change_cancels;
    uint32_t inventory_limit_cancels;
    uint32_t risk_breach_cancels;
    uint32_t market_state_cancels;
    uint32_t closeout_cancels;
    uint32_t self_trade_cancels;
    
    double   avg_order_lifetime_ms;
    double   fill_rate;
    double   cancel_rate;
    uint64_t last_stats_log_time;
    
    // Self-trade stats
    uint32_t stp_checks;
    uint32_t stp_risks_detected;
    uint32_t stp_mm_cancels;
    
    UnifiedTrackerStats() : orders_placed(0), orders_filled(0), orders_canceled(0),
        cancel_requests(0), duplicate_cancels(0), price_deviation_cancels(0),
        stale_cancels(0), timeout_cancels(0), state_change_cancels(0),
        inventory_limit_cancels(0), risk_breach_cancels(0),
        market_state_cancels(0), closeout_cancels(0), self_trade_cancels(0),
        avg_order_lifetime_ms(0), fill_rate(0), cancel_rate(0),
        last_stats_log_time(0), stp_checks(0), stp_risks_detected(0),
        stp_mm_cancels(0) {}
    
    inline void recordCancel(CancelReason reason) {
        switch (reason) {
            case CancelReason::PRICE_DEVIATION: price_deviation_cancels++; break;
            case CancelReason::STALE: stale_cancels++; break;
            case CancelReason::TIMEOUT: timeout_cancels++; break;
            case CancelReason::STATE_CHANGE: state_change_cancels++; break;
            case CancelReason::INVENTORY_LIMIT: inventory_limit_cancels++; break;
            case CancelReason::RISK_BREACH: risk_breach_cancels++; break;
            case CancelReason::MARKET_STATE_CHANGE: market_state_cancels++; break;
            case CancelReason::CLOSEOUT: closeout_cancels++; break;
            case CancelReason::SELF_TRADE: self_trade_cancels++; break;
            default: break;
        }
    }
};

//==============================================================================
// Cancel Action
//==============================================================================

struct CancelAction
{
    uint32_t    order_id;
    CancelReason reason;
    double      deviation;
    
    CancelAction() : order_id(0), reason(CancelReason::NONE), deviation(0) {}
};

//==============================================================================
// Self-Trade Check Result
//==============================================================================

struct SelfTradeCheckResult
{
    bool has_risk;
    std::string risk_code;
    double conflict_price;
    double conflict_qty;
    std::vector<uint32_t> conflicting_order_ids;
    
    enum class Action : uint8_t
    {
        ALLOW,
        REJECT,
        CANCEL_MM_FIRST,
        ADJUST_PRICE
    } recommended_action;
    
    double adjusted_price;
    
    SelfTradeCheckResult()
        : has_risk(false), conflict_price(0), conflict_qty(0),
          recommended_action(Action::ALLOW), adjusted_price(0) {}
};

//==============================================================================
// Arbitrage Order Request (for self-trade check)
//==============================================================================

struct ArbitrageOrderRequest
{
    std::string code;
    bool is_buy;
    double price;
    double qty;
    bool is_market_order;
    
    ArbitrageOrderRequest()
        : price(0), qty(0), is_buy(true), is_market_order(false) {}
};

//==============================================================================
// Unified Order Tracker
//==============================================================================

class UnifiedOrderTracker
{
public:
    UnifiedOrderTracker() : _order_count(0), _total_cancels(0) {
        // P1优化: 预分配内存，减少运行时动态分配
        _orders.reserve(64);
        _free_slots.reserve(32);
        _cancel_timestamps.reserve(1000);
        _order_index.reserve(128);
        _order_place_times.reserve(128);
    }
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const UnifiedTrackerConfig& cfg) { 
        _cfg = cfg; 
        // P1优化: 根据配置预分配内存
        if (_cfg.max_orders > _orders.capacity()) {
            _orders.reserve(_cfg.max_orders);
            _free_slots.reserve(_cfg.max_orders / 2);
            _order_index.reserve(_cfg.max_orders);
            _order_place_times.reserve(_cfg.max_orders);
        }
    }
    const UnifiedTrackerConfig& getConfig() const { return _cfg; }
    
    //==========================================================================
    // Order Management - MM Orders
    //==========================================================================
    
    /// Track a market making order
    uint32_t trackMMOrder(uint32_t orderId, uint32_t levelIndex, const std::string& code,
                          double price, double qty, double placeMid, uint64_t placeTime, bool isBid)
    {
        return trackOrderInternal(orderId, levelIndex, code, price, qty, placeMid, placeTime, 
                                  isBid, true /*isMM*/, false /*isArb*/);
    }
    
    //==========================================================================
    // Order Management - Arbitrage Orders
    //==========================================================================
    
    /// Track an arbitrage order
    uint32_t trackArbOrder(uint32_t orderId, const std::string& code,
                           double price, double qty, double placeMid, uint64_t placeTime, bool isBuy)
    {
        return trackOrderInternal(orderId, 0 /*levelIndex*/, code, price, qty, placeMid, placeTime,
                                  isBuy, false /*isMM*/, true /*isArb*/);
    }
    
    //==========================================================================
    // Order Management - Common
    //==========================================================================
    
    void updateOrderQty(uint32_t orderId, double remainingQty)
    {
        UnifiedOrderInfo* order = getOrderByOrderId(orderId);
        if (!order) return;
        if (remainingQty <= 0) untrackOrder(orderId);
        else order->qty = remainingQty;
    }
    
    void untrackOrder(uint32_t orderId, uint64_t currentTime = 0);
    
    void markPendingCancel(uint32_t orderId, CancelReason reason) {
        UnifiedOrderInfo* order = getOrderByOrderId(orderId);
        if (order) order->setPendingCancel(reason);
    }
    
    void clearPendingCancel(uint32_t orderId) {
        UnifiedOrderInfo* order = getOrderByOrderId(orderId);
        if (order) order->clearPendingCancel();
    }
    
    //==========================================================================
    // Query Methods - O(1)
    //==========================================================================
    
    inline bool isTracked(uint32_t orderId) const {
        return _order_index.find(orderId) != _order_index.end();
    }
    
    inline UnifiedOrderInfo* getOrderByOrderId(uint32_t orderId) {
        auto it = _order_index.find(orderId);
        if (it != _order_index.end()) return &_orders[it->second];
        return nullptr;
    }
    
    inline const UnifiedOrderInfo* getOrderByOrderId(uint32_t orderId) const {
        auto it = _order_index.find(orderId);
        if (it != _order_index.end()) return &_orders[it->second];
        return nullptr;
    }
    
    inline UnifiedOrderInfo* getOrderByIndex(uint32_t index) {
        if (index < _orders.size()) return &_orders[index];
        return nullptr;
    }
    
    inline std::vector<UnifiedOrderInfo>& getOrders() { return _orders; }
    inline const std::vector<UnifiedOrderInfo>& getOrders() const { return _orders; }
    inline uint32_t getOrderCount() const { return _order_count; }
    
    //==========================================================================
    // Per-Contract Query
    //==========================================================================
    
    /// Get all active order IDs for a contract
    std::vector<uint32_t> getOrderIdsForContract(const std::string& code) const;
    
    /// Get active MM buy order IDs for a contract
    std::vector<uint32_t> getMMBuyOrderIds(const std::string& code) const;
    
    /// Get active MM sell order IDs for a contract
    std::vector<uint32_t> getMMSellOrderIds(const std::string& code) const;
    
    /// Get best MM buy price for a contract
    double getBestMMBuy(const std::string& code) const;
    
    /// Get best MM sell price for a contract
    double getBestMMSell(const std::string& code) const;
    
    /// Check if contract has active MM orders
    bool hasMMOrders(const std::string& code) const;
    
    /// Get total pending buy quantity for a contract (for position limit check)
    double getPendingBuyQty(const std::string& code) const;
    
    /// Get total pending sell quantity for a contract (for position limit check)
    double getPendingSellQty(const std::string& code) const;
    
    //==========================================================================
    // Price Deviation Check
    //==========================================================================
    
    inline bool checkPriceDeviation(uint32_t orderId, double currentMid, double tickSize) {
        UnifiedOrderInfo* order = getOrderByOrderId(orderId);
        if (!order || order->isPendingCancel()) return false;
        if (order->place_mid <= 0 || currentMid <= 0 || tickSize <= 0) return false;
        
        double deviation = std::abs(currentMid - order->place_mid) / tickSize;
        if (deviation > _cfg.price_deviation) {
            order->setPendingCancel(CancelReason::PRICE_DEVIATION);
            return true;
        }
        return false;
    }
    
    inline bool exceedsStickyThreshold(uint32_t orderId, double currentMid, double tickSize) const {
        auto it = _order_index.find(orderId);
        if (it == _order_index.end()) return true;
        const UnifiedOrderInfo& order = _orders[it->second];
        if (order.place_mid <= 0 || currentMid <= 0 || tickSize <= 0) return true;
        return std::abs(currentMid - order.place_mid) / tickSize > _cfg.sticky_threshold;
    }
    
    //==========================================================================
    // Auto-Cancel Checks
    // 注：STATE_CHANGE 和 PRICE_DEVIATION 已移除，由 FutuQuoter 粘性逻辑处理
    //==========================================================================
    
    std::vector<CancelAction> checkAutoCancel(
        const std::string& code, uint64_t currentTime, double currentMid, double tickSize,
        bool stateChanged, bool inventoryLimitHit, double current_risk_delta);
    
    //==========================================================================
    // Self-Trade Prevention
    //==========================================================================
    
    /// Check if an arbitrage order would cause self-trade
    SelfTradeCheckResult checkArbitrageOrder(const ArbitrageOrderRequest& request) const;
    
    /// Check for a specific contract and direction
    SelfTradeCheckResult checkSelfTrade(const std::string& code, bool is_buy, 
                                         double price, bool is_market_order = false) const;
    
    /// Get MM orders that would conflict with an arbitrage order
    std::vector<uint32_t> getConflictingMMOrders(const std::string& code, 
                                                  bool arb_is_buy, double arb_price) const;
    
    //==========================================================================
    // Statistics
    //==========================================================================
    
    uint32_t getTotalTracked() const { return _order_count; }
    uint32_t getTotalCancellations() const { return _total_cancels; }
    const UnifiedTrackerStats& getStats() const { return _stats; }
    
    void recordFilled() { _stats.orders_filled++; }
    void recordDuplicateCancel() { _stats.duplicate_cancels++; }
    
    bool shouldCancelDueToRate(uint64_t currentTime);
    
    void updateFillRateStats()
    {
        if (_stats.orders_placed > 0)
            _stats.fill_rate = static_cast<double>(_stats.orders_filled) / _stats.orders_placed;
    }
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void clear()
    {
        _orders.clear();
        _free_slots.clear();
        _order_index.clear();
        _order_place_times.clear();
        _cancel_timestamps.clear();
        _orders_by_code.clear();
        _mm_buy_by_code.clear();
        _mm_sell_by_code.clear();
        _best_buy_price.clear();
        _best_sell_price.clear();
        _order_count = 0;
        _total_cancels = 0;
        _stats = UnifiedTrackerStats();
    }

private:
    //==========================================================================
    // Internal Methods
    //==========================================================================
    
    uint32_t trackOrderInternal(uint32_t orderId, uint32_t levelIndex, const std::string& code,
                                double price, double qty, double placeMid, uint64_t placeTime,
                                bool isBid, bool isMM, bool isArb);
    
    void updateBestPrices(const std::string& code, double price, bool is_buy);
    
    //==========================================================================
    // Data Members
    //==========================================================================
    
    UnifiedTrackerConfig _cfg;
    
    // Main storage - continuous memory layout
    std::vector<UnifiedOrderInfo> _orders;
    std::vector<uint32_t> _free_slots;
    wtp::wt_hashmap<uint32_t, uint32_t> _order_index;  // orderId -> vector index
    uint32_t _order_count;
    uint32_t _total_cancels;
    
    // Place times for lifetime calculation
    wtp::wt_hashmap<uint32_t, uint64_t> _order_place_times;
    
    // Cancel timestamps for rate limiting
    std::vector<uint64_t> _cancel_timestamps;
    
    // Per-contract indices (lightweight - only order IDs)
    wtp::wt_hashmap<std::string, std::vector<uint32_t>> _orders_by_code;
    
    // Separate indices for MM buy/sell orders (for self-trade detection)
    wtp::wt_hashmap<std::string, std::vector<uint32_t>> _mm_buy_by_code;
    wtp::wt_hashmap<std::string, std::vector<uint32_t>> _mm_sell_by_code;
    
    // Best price cache
    wtp::wt_hashmap<std::string, double> _best_buy_price;
    wtp::wt_hashmap<std::string, double> _best_sell_price;
    
    // Statistics
    UnifiedTrackerStats _stats;
};

} // namespace futu
