/*!
 * \file AutoCancelPolicy.h
 * \brief Automatic Order Cancellation Policy for Market Making
 * 
 * Manages automatic cancellation of resting orders based on:
 *   - Time since placement (stale order detection)
 *   - Price deviation from current market
 *   - Market state changes
 *   - Inventory/risk triggers
 * 
 * All operations are O(1) using hash maps for order tracking.
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN
class IUftStraCtx;
class WTSTickData;
NS_WTP_END

namespace futu {

/// Cancellation policy parameters
struct CancelParams
{
    uint32_t    max_age_ms;         ///< Maximum order age in milliseconds
    double      price_deviation;    ///< Max price deviation from market (in ticks)
    uint32_t    check_interval_ms;  ///< Check interval in milliseconds
    bool        cancel_on_state_change; ///< Cancel when market state changes
    bool        cancel_on_inventory_limit; ///< Cancel when inventory limit hit
    
    // Sticky 策略参数 - 减少 STATE_CHANGE 时的频繁撤单
    double      sticky_threshold;   ///< Price stickiness threshold in ticks (订单价格粘性阈值)
                                    ///< 当 STATE_CHANGE 时，如果新价格与原订单价格偏离不超过此阈值，
                                    ///< 则不触发撤单，避免频繁撤单重报
    
    // INVENTORY_LIMIT 冷却机制 - 避免短时间内重复撤单
    uint32_t    inventory_limit_cooldown_ms; ///< Cooldown period for INVENTORY_LIMIT cancellations (ms)
                                            ///< 同一订单在冷却期内不会被 INVENTORY_LIMIT 理由再次撤单
    
    CancelParams()
        : max_age_ms(5000)
        , price_deviation(3.0)
        , check_interval_ms(100)
        , cancel_on_state_change(true)
        , cancel_on_inventory_limit(true)
        , sticky_threshold(1.0)  // 默认1个tick的粘性阈值
        , inventory_limit_cooldown_ms(2000)  // 默认2秒冷却期
    {}
};

/// Tracked order information
struct TrackedOrder
{
    uint32_t    order_id;           ///< Local order ID
    std::string code;               ///< Contract code
    double      price;              ///< Order price
    double      qty;                ///< Order quantity
    bool        is_buy;             ///< Buy or sell
    uint64_t    place_time;         ///< Placement timestamp (ms)
    uint64_t    last_check;         ///< Last check timestamp
    double      place_mid;          ///< Mid price at placement
    uint8_t     flags;              ///< Additional flags
    uint64_t    last_inventory_cancel_check; ///< Last INVENTORY_LIMIT cancel check timestamp (ms)
    
    TrackedOrder()
        : order_id(0), price(0), qty(0), is_buy(false)
        , place_time(0), last_check(0), place_mid(0), flags(0)
        , last_inventory_cancel_check(0)
    {}
};

/// Cancellation reason
enum class CancelReason
{
    NONE,               ///< No cancellation
    STALE,              ///< Order too old
    PRICE_DEVIATION,    ///< Price moved away from market
    STATE_CHANGE,       ///< Market state changed
    INVENTORY_LIMIT,    ///< Inventory limit reached
    MANUAL,             ///< Manual cancellation
    REPLACE             ///< Cancel for replacement
};

/// Cancellation action
struct CancelAction
{
    uint32_t    order_id;           ///< Order to cancel
    CancelReason reason;            ///< Reason for cancellation
    double      current_mid;        ///< Current mid price
    double      deviation;          ///< Price deviation
    
    CancelAction()
        : order_id(0), reason(CancelReason::NONE)
        , current_mid(0), deviation(0)
    {}
};

/// Auto Cancel Policy Manager
class AutoCancelPolicy
{
public:
    AutoCancelPolicy();
    ~AutoCancelPolicy() {}
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setParams(const CancelParams& params) { _params = params; }
    const CancelParams& getParams() const { return _params; }
    
    //==========================================================================
    // Order Tracking
    //==========================================================================
    
    /// Track a new order
    void trackOrder(uint32_t orderId, const std::string& code, 
                    double price, double qty, bool isBuy,
                    uint64_t placeTime, double placeMid);
    
    /// Stop tracking an order (called on fill/cancel)
    void untrackOrder(uint32_t orderId);
    
    /// Check if order is being tracked
    bool isTracked(uint32_t orderId) const;
    
    /// Get tracked order info
    const TrackedOrder* getOrder(uint32_t orderId) const;
    
    //==========================================================================
    // Cancellation Checks
    //==========================================================================
    
    /// Check all orders for cancellation conditions
    /// Returns list of orders to cancel
    std::vector<CancelAction> checkOrders(
        uint64_t currentTime,
        const std::string& code,
        double currentMid,
        double tickSize,
        bool stateChanged = false,
        bool inventoryLimitHit = false
    );
    
    /// Check a single order
    CancelAction checkOrder(
        uint32_t orderId,
        uint64_t currentTime,
        double currentMid,
        double tickSize
    ) const;
    
    //==========================================================================
    // Execution
    //==========================================================================
    
    /// Execute cancellation actions via context
    void executeCancellations(wtp::IUftStraCtx* ctx, 
                              const std::vector<CancelAction>& actions);
    
    //==========================================================================
    // Statistics
    //==========================================================================
    
    uint32_t getTotalTracked() const { return _orders.size(); }
    uint32_t getTotalCancellations() const { return _total_cancels; }
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void clear();
    
private:
    CancelParams _params;
    
    /// O(1) order lookup
    wtp::wt_hashmap<uint32_t, TrackedOrder> _orders;
    
    // Statistics
    uint32_t _total_cancels;
};

} // namespace futu
