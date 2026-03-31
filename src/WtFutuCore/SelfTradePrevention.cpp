/*!
 * \file SelfTradePrevention.cpp
 * \brief Self-Trade Prevention Implementation
 */
#include "SelfTradePrevention.h"
#include <algorithm>
#include <cmath>

namespace futu {

//==============================================================================
// Construction
//==============================================================================

SelfTradePrevention::SelfTradePrevention()
    : _config()
{
}

//==============================================================================
// Order Tracking
//==============================================================================

void SelfTradePrevention::trackMMOrder(const std::string& code, uint32_t order_id,
                                        double price, double qty, bool is_buy, uint64_t timestamp)
{
    if (!_config.enabled || order_id == 0 || qty <= 0)
        return;
    
    // Create order record
    ActiveOrder order(code, order_id, price, qty, is_buy, timestamp);
    
    // Store by order ID
    _orders_by_id[order_id] = order;
    
    // Store by code
    if (is_buy)
    {
        _buy_orders_by_code[code][order_id] = order;
        
        // Update best buy price
        auto it = _best_buy_price.find(code);
        if (it == _best_buy_price.end() || price > it->second)
        {
            _best_buy_price[code] = price;
        }
    }
    else
    {
        _sell_orders_by_code[code][order_id] = order;
        
        // Update best sell price
        auto it = _best_sell_price.find(code);
        if (it == _best_sell_price.end() || price < it->second)
        {
            _best_sell_price[code] = price;
        }
    }
}

void SelfTradePrevention::untrackOrder(uint32_t order_id)
{
    auto it = _orders_by_id.find(order_id);
    if (it == _orders_by_id.end())
        return;
    
    const ActiveOrder& order = it->second;
    const std::string& code = order.code;
    bool is_buy = order.is_buy;
    double price = order.price;
    
    // Remove from code maps
    if (is_buy)
    {
        auto code_it = _buy_orders_by_code.find(code);
        if (code_it != _buy_orders_by_code.end())
        {
            code_it->second.erase(order_id);
            if (code_it->second.empty())
            {
                _buy_orders_by_code.erase(code_it);
                _best_buy_price.erase(code);
            }
            else if (price == _best_buy_price[code])
            {
                // Recalculate best buy
                double best = 0;
                for (const auto& [id, ord] : code_it->second)
                {
                    if (ord.price > best)
                        best = ord.price;
                }
                _best_buy_price[code] = best;
            }
        }
    }
    else
    {
        auto code_it = _sell_orders_by_code.find(code);
        if (code_it != _sell_orders_by_code.end())
        {
            code_it->second.erase(order_id);
            if (code_it->second.empty())
            {
                _sell_orders_by_code.erase(code_it);
                _best_sell_price.erase(code);
            }
            else if (price == _best_sell_price[code])
            {
                // Recalculate best sell
                double best = std::numeric_limits<double>::max();
                for (const auto& [id, ord] : code_it->second)
                {
                    if (ord.price < best)
                        best = ord.price;
                }
                _best_sell_price[code] = best;
            }
        }
    }
    
    // Remove from ID map
    _orders_by_id.erase(it);
}

void SelfTradePrevention::updateOrderQty(uint32_t order_id, double new_qty)
{
    auto it = _orders_by_id.find(order_id);
    if (it == _orders_by_id.end())
        return;
    
    if (new_qty <= 0)
    {
        untrackOrder(order_id);
        return;
    }
    
    it->second.qty = new_qty;
    
    // Update in code maps too
    const ActiveOrder& order = it->second;
    if (order.is_buy)
    {
        auto code_it = _buy_orders_by_code.find(order.code);
        if (code_it != _buy_orders_by_code.end())
        {
            auto order_it = code_it->second.find(order_id);
            if (order_it != code_it->second.end())
            {
                order_it->second.qty = new_qty;
            }
        }
    }
    else
    {
        auto code_it = _sell_orders_by_code.find(order.code);
        if (code_it != _sell_orders_by_code.end())
        {
            auto order_it = code_it->second.find(order_id);
            if (order_it != code_it->second.end())
            {
                order_it->second.qty = new_qty;
            }
        }
    }
}

void SelfTradePrevention::clear()
{
    _orders_by_id.clear();
    _buy_orders_by_code.clear();
    _sell_orders_by_code.clear();
    _best_buy_price.clear();
    _best_sell_price.clear();
}

//==============================================================================
// Self-Trade Detection
//==============================================================================

SelfTradeCheckResult SelfTradePrevention::checkArbitrageOrder(const ArbitrageOrderRequest& request) const
{
    return checkOrder(request.code, request.is_buy, request.price, request.is_market_order);
}

SelfTradeCheckResult SelfTradePrevention::checkOrder(const std::string& code, bool is_buy,
                                                      double price, bool is_market_order) const
{
    SelfTradeCheckResult result;
    
    if (!_config.enabled)
    {
        result.has_risk = false;
        result.recommended_action = SelfTradeCheckResult::Action::ALLOW;
        return result;
    }
    
    // Self-trade risk occurs when:
    // 1. Arbitrage wants to BUY, and MM has SELL orders at or below arbitrage buy price
    // 2. Arbitrage wants to SELL, and MM has BUY orders at or above arbitrage sell price
    
    if (is_buy)
    {
        // Check if there are MM sell orders that would cross
        auto sell_it = _sell_orders_by_code.find(code);
        if (sell_it == _sell_orders_by_code.end())
        {
            // No MM sell orders, no risk
            result.has_risk = false;
            result.recommended_action = SelfTradeCheckResult::Action::ALLOW;
            return result;
        }
        
        // Find conflicting orders
        double min_gap = _config.min_price_gap;
        
        for (const auto& [id, order] : sell_it->second)
        {
            // For market orders, any MM sell order is a conflict
            // For limit orders, conflict if sell_price <= buy_price + gap
            bool conflicts = false;
            
            if (is_market_order)
            {
                conflicts = true;
            }
            else if (!_config.allow_same_price && order.price <= price + min_gap)
            {
                conflicts = true;
            }
            else if (_config.allow_same_price && order.price < price)
            {
                conflicts = true;
            }
            
            if (conflicts)
            {
                result.has_risk = true;
                result.conflicting_order_ids.push_back(id);
                result.conflict_price = order.price;
                result.conflict_qty += order.qty;
            }
        }
        
        if (result.has_risk)
        {
            result.risk_code = code;
            
            switch (_config.strategy)
            {
                case StpConfig::Strategy::REJECT_ARB:
                    result.recommended_action = SelfTradeCheckResult::Action::REJECT;
                    break;
                    
                case StpConfig::Strategy::CANCEL_MM:
                    result.recommended_action = SelfTradeCheckResult::Action::CANCEL_FIRST;
                    break;
                    
                case StpConfig::Strategy::ADJUST_ARB_PRICE:
                    // Adjust buy price to be below the lowest MM sell price
                    if (!is_market_order)
                    {
                        double best_sell = getBestMMSell(code);
                        result.adjusted_price = best_sell - _config.price_adjust_ticks;
                        result.recommended_action = SelfTradeCheckResult::Action::ADJUST_PRICE;
                    }
                    else
                    {
                        // Can't adjust market order price
                        result.recommended_action = SelfTradeCheckResult::Action::CANCEL_FIRST;
                    }
                    break;
            }
        }
    }
    else
    {
        // Arbitrage wants to SELL
        // Check if there are MM buy orders that would cross
        auto buy_it = _buy_orders_by_code.find(code);
        if (buy_it == _buy_orders_by_code.end())
        {
            // No MM buy orders, no risk
            result.has_risk = false;
            result.recommended_action = SelfTradeCheckResult::Action::ALLOW;
            return result;
        }
        
        // Find conflicting orders
        double min_gap = _config.min_price_gap;
        
        for (const auto& [id, order] : buy_it->second)
        {
            bool conflicts = false;
            
            if (is_market_order)
            {
                conflicts = true;
            }
            else if (!_config.allow_same_price && order.price >= price - min_gap)
            {
                conflicts = true;
            }
            else if (_config.allow_same_price && order.price > price)
            {
                conflicts = true;
            }
            
            if (conflicts)
            {
                result.has_risk = true;
                result.conflicting_order_ids.push_back(id);
                result.conflict_price = order.price;
                result.conflict_qty += order.qty;
            }
        }
        
        if (result.has_risk)
        {
            result.risk_code = code;
            
            switch (_config.strategy)
            {
                case StpConfig::Strategy::REJECT_ARB:
                    result.recommended_action = SelfTradeCheckResult::Action::REJECT;
                    break;
                    
                case StpConfig::Strategy::CANCEL_MM:
                    result.recommended_action = SelfTradeCheckResult::Action::CANCEL_FIRST;
                    break;
                    
                case StpConfig::Strategy::ADJUST_ARB_PRICE:
                    if (!is_market_order)
                    {
                        double best_buy = getBestMMBuy(code);
                        result.adjusted_price = best_buy + _config.price_adjust_ticks;
                        result.recommended_action = SelfTradeCheckResult::Action::ADJUST_PRICE;
                    }
                    else
                    {
                        result.recommended_action = SelfTradeCheckResult::Action::CANCEL_FIRST;
                    }
                    break;
            }
        }
    }
    
    return result;
}

//==============================================================================
// Query
//==============================================================================

std::vector<ActiveOrder> SelfTradePrevention::getMMBuyOrders(const std::string& code) const
{
    std::vector<ActiveOrder> result;
    
    auto it = _buy_orders_by_code.find(code);
    if (it == _buy_orders_by_code.end())
        return result;
    
    result.reserve(it->second.size());
    for (const auto& [id, order] : it->second)
    {
        result.push_back(order);
    }
    
    // Sort by price descending (best first)
    std::sort(result.begin(), result.end(), 
              [](const ActiveOrder& a, const ActiveOrder& b) { return a.price > b.price; });
    
    return result;
}

std::vector<ActiveOrder> SelfTradePrevention::getMMSellOrders(const std::string& code) const
{
    std::vector<ActiveOrder> result;
    
    auto it = _sell_orders_by_code.find(code);
    if (it == _sell_orders_by_code.end())
        return result;
    
    result.reserve(it->second.size());
    for (const auto& [id, order] : it->second)
    {
        result.push_back(order);
    }
    
    // Sort by price ascending (best first)
    std::sort(result.begin(), result.end(),
              [](const ActiveOrder& a, const ActiveOrder& b) { return a.price < b.price; });
    
    return result;
}

double SelfTradePrevention::getBestMMBuy(const std::string& code) const
{
    auto it = _best_buy_price.find(code);
    return (it != _best_buy_price.end()) ? it->second : 0.0;
}

double SelfTradePrevention::getBestMMSell(const std::string& code) const
{
    auto it = _best_sell_price.find(code);
    return (it != _best_sell_price.end()) ? it->second : 0.0;
}

bool SelfTradePrevention::hasMMOrders(const std::string& code) const
{
    return (_buy_orders_by_code.find(code) != _buy_orders_by_code.end()) ||
           (_sell_orders_by_code.find(code) != _sell_orders_by_code.end());
}

size_t SelfTradePrevention::ordersForContract(const std::string& code) const
{
    size_t count = 0;
    
    auto buy_it = _buy_orders_by_code.find(code);
    if (buy_it != _buy_orders_by_code.end())
        count += buy_it->second.size();
    
    auto sell_it = _sell_orders_by_code.find(code);
    if (sell_it != _sell_orders_by_code.end())
        count += sell_it->second.size();
    
    return count;
}

//==============================================================================
// Internal Methods
//==============================================================================

double SelfTradePrevention::computeAdjustedPrice(double original_price, bool is_buy) const
{
    // Compute price adjusted to avoid crossing
    // For buy: adjust downward
    // For sell: adjust upward
    
    double adjustment = _config.price_adjust_ticks;
    
    if (is_buy)
    {
        return original_price - adjustment;
    }
    else
    {
        return original_price + adjustment;
    }
}

} // namespace futu
