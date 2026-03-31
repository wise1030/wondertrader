/*!
 * \file AutoCancelPolicy.cpp
 * \brief Automatic Order Cancellation Policy Implementation
 */
#include "AutoCancelPolicy.h"
#include "../Includes/IUftStraCtx.h"
#include "../WTSTools/WTSLogger.h"

namespace futu {

AutoCancelPolicy::AutoCancelPolicy()
    : _total_cancels(0)
{
}

void AutoCancelPolicy::trackOrder(uint32_t orderId, const std::string& code,
                                   double price, double qty, bool isBuy,
                                   uint64_t placeTime, double placeMid)
{
    TrackedOrder order;
    order.order_id = orderId;
    order.code = code;
    order.price = price;
    order.qty = qty;
    order.is_buy = isBuy;
    order.place_time = placeTime;
    order.last_check = placeTime;
    order.place_mid = placeMid;
    order.flags = 0;
    order.last_inventory_cancel_check = 0;  // 初始化冷却时间
    
    _orders[orderId] = order;
}

void AutoCancelPolicy::untrackOrder(uint32_t orderId)
{
    _orders.erase(orderId);
}

bool AutoCancelPolicy::isTracked(uint32_t orderId) const
{
    return _orders.find(orderId) != _orders.end();
}

const TrackedOrder* AutoCancelPolicy::getOrder(uint32_t orderId) const
{
    auto it = _orders.find(orderId);
    return (it != _orders.end()) ? &it->second : nullptr;
}

CancelAction AutoCancelPolicy::checkOrder(uint32_t orderId,
                                           uint64_t currentTime,
                                           double currentMid,
                                           double tickSize) const
{
    CancelAction action;
    action.order_id = 0;
    action.reason = CancelReason::NONE;
    
    auto it = _orders.find(orderId);
    if (it == _orders.end())
        return action;
    
    const TrackedOrder& order = it->second;
    action.order_id = orderId;
    action.current_mid = currentMid;
    
    // Check age
    uint64_t age = currentTime - order.place_time;
    if (age > _params.max_age_ms)
    {
        action.reason = CancelReason::STALE;
        return action;
    }
    
    // Check price deviation
    if (order.place_mid > 0 && currentMid > 0 && tickSize > 0)
    {
        double deviation = std::abs(currentMid - order.place_mid) / tickSize;
        action.deviation = deviation;
        
        if (deviation > _params.price_deviation)
        {
            action.reason = CancelReason::PRICE_DEVIATION;
            return action;
        }
    }
    
    return action;
}

std::vector<CancelAction> AutoCancelPolicy::checkOrders(
    uint64_t currentTime,
    const std::string& code,
    double currentMid,
    double tickSize,
    bool stateChanged,
    bool inventoryLimitHit)
{
    std::vector<CancelAction> actions;
    
    for (auto& pair : _orders)
    {
        TrackedOrder& order = pair.second;
        
        // Filter by code if specified
        if (!code.empty() && order.code != code)
            continue;
        
        order.last_check = currentTime;
        
        CancelAction action;
        action.order_id = order.order_id;
        action.current_mid = currentMid;
        
        // Priority 1: Inventory limit (with cooldown mechanism)
        if (inventoryLimitHit && _params.cancel_on_inventory_limit)
        {
            // 冷却机制检查：如果距离上次 INVENTORY_LIMIT 撤单检查在冷却期内，跳过
            if (_params.inventory_limit_cooldown_ms > 0 && 
                order.last_inventory_cancel_check > 0)
            {
                uint64_t time_since_last_check = currentTime - order.last_inventory_cancel_check;
                if (time_since_last_check < _params.inventory_limit_cooldown_ms)
                {
                    // 在冷却期内，跳过 INVENTORY_LIMIT 撤单，继续检查其他条件
                    goto check_other_conditions;
                }
            }
            
            // 记录本次检查时间
            order.last_inventory_cancel_check = currentTime;
            
            action.reason = CancelReason::INVENTORY_LIMIT;
            actions.push_back(action);
            continue;
        }
        
    check_other_conditions:
        // Priority 2: State change (with sticky check)
        if (stateChanged && _params.cancel_on_state_change)
        {
            // Sticky 策略：如果订单价格与当前市场价格的偏离在容忍范围内，不撤单
            // 这避免了频繁的撤单-重报循环
            double priceDeviation = 0;
            if (order.place_mid > 0 && currentMid > 0 && tickSize > 0)
            {
                priceDeviation = std::abs(currentMid - order.place_mid) / tickSize;
            }
            
            // 只有当价格偏离超过 sticky_threshold 时才撤单
            if (priceDeviation > _params.sticky_threshold)
            {
                action.reason = CancelReason::STATE_CHANGE;
                action.deviation = priceDeviation;
                actions.push_back(action);
                continue; // Already decided to cancel, skip further checks
            }
            // 否则保留订单，让它继续接受后面的超时（Age）和价格偏离检查
        }
        
        // Priority 3: Age
        uint64_t age = currentTime - order.place_time;
        if (age > _params.max_age_ms)
        {
            action.reason = CancelReason::STALE;
            actions.push_back(action);
            continue;
        }
        
        // Priority 4: Price deviation
        if (order.place_mid > 0 && currentMid > 0 && tickSize > 0)
        {
            double deviation = std::abs(currentMid - order.place_mid) / tickSize;
            action.deviation = deviation;
            
            if (deviation > _params.price_deviation)
            {
                action.reason = CancelReason::PRICE_DEVIATION;
                actions.push_back(action);
            }
        }
    }
    
    return actions;
}

void AutoCancelPolicy::executeCancellations(
    wtp::IUftStraCtx* ctx,
    const std::vector<CancelAction>& actions)
{
    if (!ctx || actions.empty())
        return;
    
    for (const auto& action : actions)
    {
        if (ctx->stra_cancel(action.order_id))
        {
            _total_cancels++;
            
            // Log cancellation
            const char* reasonStr = "UNKNOWN";
            switch (action.reason)
            {
                case CancelReason::STALE: reasonStr = "STALE"; break;
                case CancelReason::PRICE_DEVIATION: reasonStr = "PRICE_DEVIATION"; break;
                case CancelReason::STATE_CHANGE: reasonStr = "STATE_CHANGE"; break;
                case CancelReason::INVENTORY_LIMIT: reasonStr = "INVENTORY_LIMIT"; break;
                case CancelReason::MANUAL: reasonStr = "MANUAL"; break;
                case CancelReason::REPLACE: reasonStr = "REPLACE"; break;
                default: break;
            }
            
            ctx->stra_log_debug(fmt::format("Auto-cancel order {} reason: {}", 
                action.order_id, reasonStr).c_str());
        }
    }
}

void AutoCancelPolicy::clear()
{
    _orders.clear();
}

} // namespace futu
