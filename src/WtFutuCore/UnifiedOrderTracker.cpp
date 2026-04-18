/*!
 * \file UnifiedOrderTracker.cpp
 * \brief Unified Order Tracker Implementation
 */

#include "UnifiedOrderTracker.h"
#include "../WTSTools/WTSLogger.h"
#include <algorithm>
#include <limits>

namespace futu {

//==============================================================================
// Internal: Track Order
//==============================================================================

uint32_t UnifiedOrderTracker::trackOrderInternal(
    uint32_t orderId, uint32_t levelIndex, const std::string& code,
    double price, double qty, double placeMid, uint64_t placeTime,
    bool isBid, bool isMM, bool isArb)
{
    // Get or allocate slot
    uint32_t index = !_free_slots.empty() ? _free_slots.back() : static_cast<uint32_t>(_orders.size());
    if (!_free_slots.empty()) _free_slots.pop_back();
    else _orders.emplace_back();
    
    // Fill order info
    UnifiedOrderInfo& order = _orders[index];
    order.order_id = orderId;
    order.level_index = levelIndex;
    strncpy(order.code, code.c_str(), MAX_CODE_LEN - 1);
    order.code[MAX_CODE_LEN - 1] = '\0';
    order.price = price;
    order.qty = qty;
    order.place_mid = placeMid;
    order.place_time = placeTime;
    order.last_check = placeTime;
    order.last_inv_cancel_check = 0;
    
    // Set flags
    order.flags = OrderFlags::IS_ACTIVE;
    if (isBid) order.flags = order.flags | OrderFlags::IS_BID;
    if (isMM) order.flags = order.flags | OrderFlags::IS_MM_ORDER;
    if (isArb) order.flags = order.flags | OrderFlags::IS_ARB_ORDER;
    order.cancel_reason = CancelReason::NONE;
    
    // Update indices
    _order_index[orderId] = index;
    _order_place_times[orderId] = placeTime;
    
    // Update per-contract indices
    _orders_by_code[code].push_back(orderId);
    
    if (isMM)
    {
        if (isBid)
            _mm_buy_by_code[code].push_back(orderId);
        else
            _mm_sell_by_code[code].push_back(orderId);
        
        // Update best price cache
        updateBestPrices(code, price, isBid);
    }
    
    _order_count++;
    _stats.orders_placed++;
    
    return index;
}

//==============================================================================
// Untrack Order
//==============================================================================

void UnifiedOrderTracker::untrackOrder(uint32_t orderId, uint64_t currentTime)
{
    auto it = _order_index.find(orderId);
    if (it == _order_index.end()) return;
    
    uint32_t index = it->second;
    UnifiedOrderInfo& order = _orders[index];
    std::string code = order.code;
    bool isBid = order.isBid();
    bool isMM = order.isMMOrder();
    double price = order.price;
    
    // Update statistics
    if (order.isPendingCancel())
    {
        _stats.orders_canceled++;
        _stats.recordCancel(order.cancel_reason);
        
        _cancel_timestamps.push_back(currentTime > 0 ? currentTime : 
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    else
    {
        _stats.orders_filled++;
    }
    
    // Update average lifetime
    auto timeIt = _order_place_times.find(orderId);
    if (timeIt != _order_place_times.end())
    {
        uint64_t placeTime = timeIt->second;
        uint64_t now = currentTime > 0 ? currentTime : placeTime;
        double lifetime = static_cast<double>(now - placeTime);
        if (_stats.orders_placed > 0)
        {
            _stats.avg_order_lifetime_ms = 
                (_stats.avg_order_lifetime_ms * (_stats.orders_placed - 1) + lifetime) / _stats.orders_placed;
        }
        _order_place_times.erase(timeIt);
    }
    
    // Remove from per-contract indices
    {
        auto codeIt = _orders_by_code.find(code);
        if (codeIt != _orders_by_code.end())
        {
            auto& ids = codeIt->second;
            ids.erase(std::remove(ids.begin(), ids.end(), orderId), ids.end());
            if (ids.empty()) _orders_by_code.erase(codeIt);
        }
    }
    
    if (isMM)
    {
        if (isBid)
        {
            auto it2 = _mm_buy_by_code.find(code);
            if (it2 != _mm_buy_by_code.end())
            {
                auto& ids = it2->second;
                ids.erase(std::remove(ids.begin(), ids.end(), orderId), ids.end());
                if (ids.empty()) _mm_buy_by_code.erase(it2);
            }
        }
        else
        {
            auto it2 = _mm_sell_by_code.find(code);
            if (it2 != _mm_sell_by_code.end())
            {
                auto& ids = it2->second;
                ids.erase(std::remove(ids.begin(), ids.end(), orderId), ids.end());
                if (ids.empty()) _mm_sell_by_code.erase(it2);
            }
        }
        
        // Recalculate best prices
        if (isBid && !_mm_buy_by_code[code].empty())
        {
            double best = 0;
            for (uint32_t id : _mm_buy_by_code[code])
            {
                const UnifiedOrderInfo* o = getOrderByOrderId(id);
                if (o && o->isActive() && o->price > best) best = o->price;
            }
            _best_buy_price[code] = best;
        }
        else if (!isBid && !_mm_sell_by_code[code].empty())
        {
            double best = std::numeric_limits<double>::max();
            for (uint32_t id : _mm_sell_by_code[code])
            {
                const UnifiedOrderInfo* o = getOrderByOrderId(id);
                if (o && o->isActive() && o->price < best) best = o->price;
            }
            _best_sell_price[code] = (best == std::numeric_limits<double>::max()) ? 0 : best;
        }
    }
    
    // Clear order and free slot
    _orders[index] = UnifiedOrderInfo();
    _free_slots.push_back(index);
    _order_index.erase(it);
    _order_count--;
}

//==============================================================================
// Update Best Prices
//==============================================================================

void UnifiedOrderTracker::updateBestPrices(const std::string& code, double price, bool is_buy)
{
    if (is_buy)
    {
        auto it = _best_buy_price.find(code);
        if (it == _best_buy_price.end() || price > it->second)
            _best_buy_price[code] = price;
    }
    else
    {
        auto it = _best_sell_price.find(code);
        if (it == _best_sell_price.end() || price < it->second || it->second == 0)
            _best_sell_price[code] = price;
    }
}

//==============================================================================
// Per-Contract Query
//==============================================================================

std::vector<uint32_t> UnifiedOrderTracker::getOrderIdsForContract(const std::string& code) const
{
    std::vector<uint32_t> result;
    auto it = _orders_by_code.find(code);
    if (it != _orders_by_code.end())
    {
        for (uint32_t id : it->second)
        {
            const UnifiedOrderInfo* order = getOrderByOrderId(id);
            if (order && order->isActive())
                result.push_back(id);
        }
    }
    return result;
}

std::vector<uint32_t> UnifiedOrderTracker::getMMBuyOrderIds(const std::string& code) const
{
    std::vector<uint32_t> result;
    auto it = _mm_buy_by_code.find(code);
    if (it != _mm_buy_by_code.end())
    {
        for (uint32_t id : it->second)
        {
            const UnifiedOrderInfo* order = getOrderByOrderId(id);
            if (order && order->isActive())
                result.push_back(id);
        }
    }
    return result;
}

std::vector<uint32_t> UnifiedOrderTracker::getMMSellOrderIds(const std::string& code) const
{
    std::vector<uint32_t> result;
    auto it = _mm_sell_by_code.find(code);
    if (it != _mm_sell_by_code.end())
    {
        for (uint32_t id : it->second)
        {
            const UnifiedOrderInfo* order = getOrderByOrderId(id);
            if (order && order->isActive())
                result.push_back(id);
        }
    }
    return result;
}

double UnifiedOrderTracker::getBestMMBuy(const std::string& code) const
{
    auto it = _best_buy_price.find(code);
    if (it != _best_buy_price.end()) return it->second;
    return 0;
}

double UnifiedOrderTracker::getBestMMSell(const std::string& code) const
{
    auto it = _best_sell_price.find(code);
    if (it != _best_sell_price.end()) return it->second;
    return 0;
}

bool UnifiedOrderTracker::hasMMOrders(const std::string& code) const
{
    auto buyIt = _mm_buy_by_code.find(code);
    if (buyIt != _mm_buy_by_code.end() && !buyIt->second.empty()) return true;
    
    auto sellIt = _mm_sell_by_code.find(code);
    if (sellIt != _mm_sell_by_code.end() && !sellIt->second.empty()) return true;
    
    return false;
}

double UnifiedOrderTracker::getPendingBuyQty(const std::string& code) const
{
    double total_qty = 0;
    auto it = _mm_buy_by_code.find(code);
    if (it != _mm_buy_by_code.end())
    {
        for (uint32_t order_id : it->second)
        {
            auto idx_it = _order_index.find(order_id);
            if (idx_it != _order_index.end())
            {
                const UnifiedOrderInfo& order = _orders[idx_it->second];
                if (order.isActive() && !order.isPendingCancel())
                {
                    total_qty += order.qty;
                }
            }
        }
    }
    return total_qty;
}

double UnifiedOrderTracker::getPendingSellQty(const std::string& code) const
{
    double total_qty = 0;
    auto it = _mm_sell_by_code.find(code);
    if (it != _mm_sell_by_code.end())
    {
        for (uint32_t order_id : it->second)
        {
            auto idx_it = _order_index.find(order_id);
            if (idx_it != _order_index.end())
            {
                const UnifiedOrderInfo& order = _orders[idx_it->second];
                if (order.isActive() && !order.isPendingCancel())
                {
                    total_qty += order.qty;
                }
            }
        }
    }
    return total_qty;
}

//==============================================================================
// Auto-Cancel Check
//==============================================================================

std::vector<CancelAction> UnifiedOrderTracker::checkAutoCancel(
    const std::string& code, uint64_t currentTime, double currentMid, double tickSize,
    bool stateChanged, bool inventoryLimitHit, double current_risk_delta)
{
    std::vector<CancelAction> actions;
    
    // Create snapshot of active order indices
    std::vector<size_t> active_indices;
    active_indices.reserve(_orders.size());
    
    for (size_t i = 0; i < _orders.size(); ++i)
    {
        const UnifiedOrderInfo& order = _orders[i];
        if (order.isActive() && !order.isPendingCancel())
        {
            if (code.empty() || std::string(order.code) == code)
                active_indices.push_back(i);
        }
    }
    
    // Process each order
    for (size_t idx : active_indices)
    {
        UnifiedOrderInfo& order = _orders[idx];
        
        if (!order.isActive() || order.isPendingCancel()) continue;
        
        order.last_check = currentTime;
        CancelAction action;
        action.order_id = order.order_id;
        
        // Priority 1: Inventory limit
        if (inventoryLimitHit && _cfg.inv_limit_cooldown_ms > 0)
        {
            // Only cancel orders that push the portfolio further into the limit breach direction
            // current_risk_delta > 0 -> excessively long -> cancel BUYS (isBid())
            // current_risk_delta < 0 -> excessively short -> cancel SELLS (!isBid())
            bool is_breach_direction = false;
            if (current_risk_delta >= 0 && order.isBid()) is_breach_direction = true;
            else if (current_risk_delta < 0 && !order.isBid()) is_breach_direction = true;
            
            if (is_breach_direction)
            {
                bool withinCooldown = false;
                if (order.last_inv_cancel_check > 0) {
                    uint64_t diff = currentTime - order.last_inv_cancel_check;
                    if (diff < _cfg.inv_limit_cooldown_ms) withinCooldown = true;
                }
                if (!withinCooldown) {
                    order.last_inv_cancel_check = currentTime;
                    action.reason = CancelReason::INVENTORY_LIMIT;
                    actions.push_back(action);
                    order.setPendingCancel(CancelReason::INVENTORY_LIMIT);
                    continue;
                }
            }
        }
        
        // ============================================================
        // Priority 2: STALE - 订单过期
        // 价格偏离检查已移至 FutuQuoter.refreshQuotes 的粘性报价逻辑
        // FutuQuoter 比较 newPrice vs placePrice，更准确感知 skew/spread 变化
        // 
        // 优化：如果价格仍在有效范围内（偏离小于 2 * sticky_threshold），
        //       不立即撤单，给订单更多成交机会
        // ============================================================
        uint64_t age = currentTime - order.place_time;
        if (age > _cfg.max_age_ms) {
            // 价格有效性检查：如果价格仍在合理范围内，延迟撤单
            double price_deviation = std::abs(order.price - currentMid) / tickSize;
            double extended_threshold = _cfg.sticky_threshold * 2.0;  // 扩展阈值
            
            if (price_deviation <= extended_threshold) {
                // 价格仍在有效范围内，延长订单寿命（给 50% 额外时间）
                uint64_t extended_age = _cfg.max_age_ms + (_cfg.max_age_ms / 2);
                if (age <= extended_age) {
                    // 在扩展时间内，不撤单
                    continue;
                }
            }
            
            action.reason = CancelReason::STALE;
            actions.push_back(action);
            order.setPendingCancel(CancelReason::STALE);
            continue;
        }
    }
    
    _total_cancels += static_cast<uint32_t>(actions.size());
    return actions;
}

//==============================================================================
// Self-Trade Prevention
//==============================================================================

SelfTradeCheckResult UnifiedOrderTracker::checkArbitrageOrder(const ArbitrageOrderRequest& request) const
{
    return checkSelfTrade(request.code, request.is_buy, request.price, request.is_market_order);
}

SelfTradeCheckResult UnifiedOrderTracker::checkSelfTrade(
    const std::string& code, bool is_buy, double price, bool is_market_order) const
{
    SelfTradeCheckResult result;
    
    if (!_cfg.stp_enabled) return result;
    
    // Arbitrage buy would conflict with MM sell orders at or below arbitrage price
    // Arbitrage sell would conflict with MM buy orders at or above arbitrage price
    
    const auto& mmOrders = is_buy ? _mm_sell_by_code : _mm_buy_by_code;
    auto it = mmOrders.find(code);
    if (it == mmOrders.end() || it->second.empty()) return result;
    
    for (uint32_t orderId : it->second)
    {
        const UnifiedOrderInfo* order = getOrderByOrderId(orderId);
        if (!order || !order->isActive()) continue;
        
        bool conflict = false;
        if (is_market_order)
        {
            // Market orders always conflict
            conflict = true;
        }
        else if (is_buy)
        {
            // Buying: conflicts with MM sell orders at or below our price
            conflict = (order->price <= price);
        }
        else
        {
            // Selling: conflicts with MM buy orders at or above our price
            conflict = (order->price >= price);
        }
        
        if (conflict)
        {
            result.has_risk = true;
            result.risk_code = code;
            result.conflict_price = order->price;
            result.conflict_qty += order->qty;
            result.conflicting_order_ids.push_back(orderId);
        }
    }
    
    if (result.has_risk)
    {
        // Recommend canceling MM orders first
        result.recommended_action = SelfTradeCheckResult::Action::CANCEL_MM_FIRST;
    }
    
    return result;
}

std::vector<uint32_t> UnifiedOrderTracker::getConflictingMMOrders(
    const std::string& code, bool arb_is_buy, double arb_price) const
{
    std::vector<uint32_t> result;
    
    const auto& mmOrders = arb_is_buy ? _mm_sell_by_code : _mm_buy_by_code;
    auto it = mmOrders.find(code);
    if (it == mmOrders.end()) return result;
    
    for (uint32_t orderId : it->second)
    {
        const UnifiedOrderInfo* order = getOrderByOrderId(orderId);
        if (!order || !order->isActive()) continue;
        
        bool conflict = false;
        if (arb_is_buy)
            conflict = (order->price <= arb_price);
        else
            conflict = (order->price >= arb_price);
        
        if (conflict)
            result.push_back(orderId);
    }
    
    return result;
}

//==============================================================================
// Cancel Rate Check
//==============================================================================

bool UnifiedOrderTracker::shouldCancelDueToRate(uint64_t currentTime)
{
    if (_cfg.max_cancel_rate > 0)
    {
        auto it = _cancel_timestamps.begin();
        while (it != _cancel_timestamps.end())
        {
            if (currentTime - *it > 1000) it = _cancel_timestamps.erase(it);
            else ++it;
        }
        _stats.cancel_rate = static_cast<double>(_cancel_timestamps.size());
        return _stats.cancel_rate > _cfg.max_cancel_rate;
    }
    return false;
}

} // namespace futu
