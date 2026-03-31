/*!
 * \file FutuPortfolio.cpp
 * \brief Unified Portfolio Management Implementation
 * 
 * Merged from: InventoryManager + FutuPortfolio
 * Performance optimized: O(1) contract lookup via hash map
 */
#include "FutuPortfolio.h"
#include "../Includes/WTSDataDef.hpp"

namespace futu {

FutuPortfolio::FutuPortfolio()
    : _cached_delta(0)
    , _delta_dirty(true)
{
}

void FutuPortfolio::addContract(const std::string& code, double multiplier, double tickSize, double hedgeRatio,
                                 double maxPosition, double maxDelta)
{
    // O(1) check if contract already exists
    auto it = _code_to_state.find(code);
    if (it != _code_to_state.end())
    {
        // Update existing contract
        ContractState* cs = it->second;
        cs->multiplier = multiplier;
        cs->tick_size = tickSize;
        cs->hedge_ratio = hedgeRatio;
        cs->max_position = maxPosition;
        cs->max_delta = maxDelta;
        return;
    }

    // Add new contract
    ContractState cs;
    cs.code = code;
    cs.multiplier = multiplier;
    cs.tick_size = tickSize;
    cs.hedge_ratio = hedgeRatio;
    cs.max_position = maxPosition;
    cs.max_delta = maxDelta;
    _contracts.push_back(std::move(cs));

    // Update lookup map - point to the last element
    _code_to_state[code] = &_contracts.back();

    // First contract becomes anchor by default
    if (_anchor_code.empty())
        _anchor_code = code;
    
    invalidateCache();
}

void FutuPortfolio::removeContract(const std::string& code)
{
    auto it = _code_to_state.find(code);
    if (it == _code_to_state.end())
        return;

    ContractState* toRemove = it->second;
    
    // Find position in vector
    auto vecIt = std::find_if(_contracts.begin(), _contracts.end(), 
        [toRemove](const ContractState& cs) { return &cs == toRemove; });
    
    if (vecIt != _contracts.end())
    {
        // If not the last element, swap with last and update its pointer
        if (vecIt != _contracts.end() - 1)
        {
            ContractState* lastState = &_contracts.back();
            *vecIt = std::move(_contracts.back());
            // Update the moved element's pointer in the map
            _code_to_state[vecIt->code] = &(*vecIt);
        }
        _contracts.pop_back();
    }
    
    _code_to_state.erase(it);
    invalidateCache();
}

void FutuPortfolio::onTick(const char* stdCode, wtp::WTSTickData* tick)
{
    if (!tick) return;

    // O(1) lookup
    ContractState* cs = getContract(stdCode);
    if (!cs) return;

    cs->last_price = tick->price();
    cs->bid1 = tick->bidprice(0);
    cs->ask1 = tick->askprice(0);
    cs->last_update = 0; // Could use tick timestamp
}

void FutuPortfolio::markToMarket(const std::string& code, double lastPrice)
{
    ContractState* cs = getContract(code);
    if (!cs) return;
    
    cs->last_price = lastPrice;
    if (cs->position != 0 && cs->avg_cost > 0)
    {
        cs->unrealized_pnl = (lastPrice - cs->avg_cost) * cs->position * cs->multiplier;
    }
}

void FutuPortfolio::onPositionUpdate(const char* stdCode, double newPos)
{
    // O(1) lookup
    ContractState* cs = getContract(stdCode);
    if (!cs) return;

    cs->position = newPos;
    invalidateCache();
}

void FutuPortfolio::updatePosition(const std::string& code, double position, double avgCost)
{
    ContractState* cs = getContract(code);
    if (!cs) return;
    
    cs->position = position;
    if (avgCost > 0)
        cs->avg_cost = avgCost;
    
    invalidateCache();
}

//==========================================================================
// Quote Adjustment
//==========================================================================

double FutuPortfolio::computeQuoteSkew(const std::string& code) const
{
    // @deprecated This method is kept for backward compatibility.
    // Use SpreadOptimizer::computePortfolioSkew() for GLFT-based inventory skew
    // that considers cross-contract correlations.
    //
    // Legacy calculation:
    // Skew = -inventory_deviation * skew_factor
    // Negative inventory (short) -> positive skew -> encourage buying (bid up)
    // Positive inventory (long) -> negative skew -> encourage selling (ask down)
    double deviation = getInventoryDeviation();
    double skew = -deviation * _params.skew_factor;
    
    // Clamp to max skew
    if (skew > _params.max_skew)
        skew = _params.max_skew;
    else if (skew < -_params.max_skew)
        skew = -_params.max_skew;
    
    return skew;
}

double FutuPortfolio::computeEnhancedSkew(const std::string& code) const
{
    // 增强 skew 计算 - 使用非线性调整
    // 目标：更激进地减小库存偏离
    
    double deviation = getInventoryDeviation();  // 正数 = 多头超仓，负数 = 空头超仓
    double utilization = getDeltaUtilization();   // 当前利用率 (0-1+)
    
    // 基础 skew（线性）
    double baseSkew = -deviation * _params.skew_factor;
    
    // 非线性增强：当利用率超过阈值时，使用指数增强
    double enhancedSkew = baseSkew;
    
    if (utilization > _params.aggressive_skew_threshold)
    {
        // 计算增强因子：利用率越高，skew 越激进
        // 使用幂函数：enhanced = base * (1 + (util - threshold)^sensitivity)
        double excessUtil = utilization - _params.aggressive_skew_threshold;
        double enhancementFactor = 1.0 + std::pow(excessUtil, _params.skew_sensitivity);
        enhancedSkew = baseSkew * enhancementFactor;
    }
    
    // 在高利用率时，额外添加方向性 skew
    // 多头超仓时（deviation > 0），增加负 skew（向下倾斜卖价）
    // 空头超仓时（deviation < 0），增加正 skew（向上倾斜买价）
    if (utilization > _params.one_sided_threshold * 0.8)
    {
        double directionSkew = 0;
        double directionFactor = (utilization - _params.one_sided_threshold * 0.8) / 
                                 (1.0 - _params.one_sided_threshold * 0.8);
        directionFactor = std::min(directionFactor, 1.0);
        
        if (deviation > 0)
        {
            // 多头超仓，向下倾斜
            directionSkew = -_params.max_skew * directionFactor * 0.5;
        }
        else if (deviation < 0)
        {
            // 空头超仓，向上倾斜
            directionSkew = _params.max_skew * directionFactor * 0.5;
        }
        
        enhancedSkew += directionSkew;
    }
    
    // Clamp to max skew
    if (enhancedSkew > _params.max_skew)
        enhancedSkew = _params.max_skew;
    else if (enhancedSkew < -_params.max_skew)
        enhancedSkew = -_params.max_skew;
    
    return enhancedSkew;
}

bool FutuPortfolio::shouldQuoteSide(bool is_buy_side) const
{
    double utilization = getDeltaUtilization();
    
    // 如果利用率低于单向报价阈值，双边都报
    if (utilization < _params.one_sided_threshold)
        return true;
    
    double deviation = getInventoryDeviation();
    
    // 多头超仓（deviation > 0）
    // - 应该报卖单来减仓
    // - 应该限制买单
    if (deviation > 0)
    {
        return !is_buy_side;  // 只报卖单
    }
    // 空头超仓（deviation < 0）
    // - 应该报买单来减仓
    // - 应该限制卖单
    else if (deviation < 0)
    {
        return is_buy_side;   // 只报买单
    }
    
    // 库存平衡，双边都报
    return true;
}

int FutuPortfolio::getOneSidedQuoteSuggestion() const
{
    double utilization = getDeltaUtilization();
    
    // 如果利用率低于单向报价阈值，双边报价
    if (utilization < _params.one_sided_threshold)
        return 0;
    
    double deviation = getInventoryDeviation();
    
    if (deviation > 0)
    {
        // 多头超仓，只报卖单（减多头）
        return -1;
    }
    else if (deviation < 0)
    {
        // 空头超仓，只报买单（减空头）
        return 1;
    }
    
    return 0;  // 双边报价
}

//==========================================================================
// Hedging
//==========================================================================

bool FutuPortfolio::needsHedging() const
{
    return std::abs(getTotalDelta()) > _params.delta_limit;
}

HedgeAction FutuPortfolio::computeHedge(double target_delta) const
{
    HedgeAction action;
    action.code = _anchor_code;
    action.qty = 0;
    action.price = 0;
    action.is_urgent = false;
    
    if (_anchor_code.empty())
        return action;

    // O(1) lookup
    const ContractState* anchor = getContract(_anchor_code);
    if (!anchor || anchor->multiplier == 0)
        return action;

    double currentDelta = getTotalDelta();
    
    // Calculate hedge quantity to reduce delta toward target
    double deltaToHedge = (currentDelta - _params.target_inventory) * _params.hedge_ratio;
    
    // Convert delta to contracts
    double anchorPrice = anchor->last_price > 0 ? anchor->last_price : 1.0;
    double hedgeQty = -deltaToHedge / (anchor->multiplier * anchor->hedge_ratio * anchorPrice);
    
    // Round to nearest integer
    action.qty = std::round(hedgeQty);
    
    // Mark as urgent if significantly over limit
    action.is_urgent = std::abs(currentDelta) > _params.delta_limit * 1.5;
    
    return action;
}

} // namespace futu
