/*!
 * \file SelfTradePrevention.cpp
 * \brief Self-Trade Prevention Implementation (UnifiedOrderTracker Wrapper)
 * 
 * All order tracking is delegated to UnifiedOrderTracker.
 * This class provides legacy API compatibility.
 */

#include "SelfTradePrevention.h"
#include "../WTSTools/WTSLogger.h"

namespace futu {

//==========================================================================
// Order Tracking (delegated to UnifiedOrderTracker)
//==========================================================================

void SelfTradePrevention::trackMMOrder(const std::string& code, uint32_t order_id, 
                                        double price, double qty, bool is_buy, uint64_t timestamp)
{
    if (!_tracker)
    {
        WTSLogger::error("SelfTradePrevention::trackMMOrder: UnifiedOrderTracker not set");
        return;
    }
    
    // Use level_index = 0 for legacy API (no level concept)
    _tracker->trackMMOrder(order_id, 0, code, price, qty, 0.0, timestamp, is_buy);
}

void SelfTradePrevention::untrackOrder(uint32_t order_id)
{
    if (_tracker)
        _tracker->untrackOrder(order_id);
}

void SelfTradePrevention::updateOrderQty(uint32_t order_id, double new_qty)
{
    if (_tracker)
        _tracker->updateOrderQty(order_id, new_qty);
}

void SelfTradePrevention::clear()
{
    // Don't clear the tracker - it's shared with other components
    // Only clear our own state if any
}

//==========================================================================
// Self-Trade Detection
//==========================================================================

SelfTradeCheckResult SelfTradePrevention::checkArbitrageOrder(const ArbitrageOrderRequest& request) const
{
    return checkOrder(request.code, request.is_buy, request.price, request.is_market_order);
}

SelfTradeCheckResult SelfTradePrevention::checkOrder(const std::string& code, bool is_buy, 
                                                      double price, bool is_market_order) const
{
    SelfTradeCheckResult result;
    
    if (!_config.enabled || !_tracker)
        return result;
    
    // Use UnifiedOrderTracker's self-trade check
    auto utResult = _tracker->checkSelfTrade(code, is_buy, price, is_market_order);
    
    // Convert to legacy result
    result.has_risk = utResult.has_risk;
    result.risk_code = utResult.risk_code;
    result.conflict_price = utResult.conflict_price;
    result.conflict_qty = utResult.conflict_qty;
    result.conflicting_order_ids = utResult.conflicting_order_ids;
    
    // Convert action
    switch (utResult.recommended_action)
    {
        case futu::SelfTradeCheckResult::Action::ALLOW:
            result.recommended_action = SelfTradeCheckResult::Action::ALLOW;
            break;
        case futu::SelfTradeCheckResult::Action::CANCEL_MM_FIRST:
            result.recommended_action = SelfTradeCheckResult::Action::CANCEL_MM_FIRST;
            break;
        case futu::SelfTradeCheckResult::Action::REJECT:
            result.recommended_action = SelfTradeCheckResult::Action::REJECT;
            break;
        case futu::SelfTradeCheckResult::Action::ADJUST_PRICE:
            result.recommended_action = SelfTradeCheckResult::Action::ADJUST_PRICE;
            break;
    }
    
    result.adjusted_price = utResult.adjusted_price;
    
    return result;
}

//==========================================================================
// Query
//==========================================================================

std::vector<ActiveOrder> SelfTradePrevention::getMMBuyOrders(const std::string& code) const
{
    std::vector<ActiveOrder> result;
    
    if (!_tracker) return result;
    
    auto orderIds = _tracker->getMMBuyOrderIds(code);
    for (uint32_t id : orderIds)
    {
        const UnifiedOrderInfo* info = _tracker->getOrderByOrderId(id);
        if (info)
        {
            ActiveOrder order;
            order.code = info->code;
            order.order_id = info->order_id;
            order.price = info->price;
            order.qty = info->qty;
            order.is_buy = info->isBid();
            order.timestamp = info->place_time;
            result.push_back(order);
        }
    }
    
    return result;
}

std::vector<ActiveOrder> SelfTradePrevention::getMMSellOrders(const std::string& code) const
{
    std::vector<ActiveOrder> result;
    
    if (!_tracker) return result;
    
    auto orderIds = _tracker->getMMSellOrderIds(code);
    for (uint32_t id : orderIds)
    {
        const UnifiedOrderInfo* info = _tracker->getOrderByOrderId(id);
        if (info)
        {
            ActiveOrder order;
            order.code = info->code;
            order.order_id = info->order_id;
            order.price = info->price;
            order.qty = info->qty;
            order.is_buy = info->isBid();
            order.timestamp = info->place_time;
            result.push_back(order);
        }
    }
    
    return result;
}

double SelfTradePrevention::getBestMMBuy(const std::string& code) const
{
    if (_tracker)
        return _tracker->getBestMMBuy(code);
    return 0;
}

double SelfTradePrevention::getBestMMSell(const std::string& code) const
{
    if (_tracker)
        return _tracker->getBestMMSell(code);
    return 0;
}

bool SelfTradePrevention::hasMMOrders(const std::string& code) const
{
    if (_tracker)
        return _tracker->hasMMOrders(code);
    return false;
}

//==========================================================================
// Statistics
//==========================================================================

size_t SelfTradePrevention::totalOrders() const
{
    if (_tracker)
        return _tracker->getOrderCount();
    return 0;
}

size_t SelfTradePrevention::ordersForContract(const std::string& code) const
{
    if (_tracker)
        return _tracker->getOrderIdsForContract(code).size();
    return 0;
}

} // namespace futu