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
                                 double maxPosition, double contractMaxDelta, double targetPosition)
{
    // O(1) check if contract already exists
    auto it = _code_to_state.find(code);
    if (it != _code_to_state.end())
    {
        // Update existing contract
        ContractState& cs = _contracts[it->second];
        cs.multiplier = multiplier;
        cs.tick_size = tickSize;
        cs.hedge_ratio = hedgeRatio;
        cs.max_position = maxPosition;
        cs.contract_max_delta = contractMaxDelta;  // 单合约 delta 软指标
        cs.target_position = targetPosition;
        invalidateCache();  // 必须刷新缓存
        return;
    }

    // Add new contract
    ContractState cs;
    cs.code = code;
    cs.multiplier = multiplier;
    cs.tick_size = tickSize;
    cs.hedge_ratio = hedgeRatio;
    cs.max_position = maxPosition;
    cs.contract_max_delta = contractMaxDelta;  // 单合约 delta 软指标
    cs.target_position = targetPosition;
    _contracts.push_back(std::move(cs));

    // Update lookup map - store index (not pointer) to avoid dangling pointer on vector resize
    _code_to_state[code] = _contracts.size() - 1;

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

    size_t idx = it->second;
    
    // If not the last element, swap with last and update its index
    if (idx != _contracts.size() - 1)
    {
        // Move last element to the removed position
        _contracts[idx] = std::move(_contracts.back());
        // Update the moved element's index in the map
        _code_to_state[_contracts[idx].code] = idx;
    }
    _contracts.pop_back();
    
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
    
    // 价格更新后需要刷新 delta 缓存
    invalidateCache();
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
// Hedging
//==========================================================================

bool FutuPortfolio::needsHedging() const
{
    // 使用 portfolio_max_delta 作为对冲触发阈值（软指标）
    return _params.portfolio_max_delta > 0 && std::abs(getTotalDelta()) > _params.portfolio_max_delta;
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
    double hedgeQty = -deltaToHedge / anchor->hedge_ratio;

    // Round to nearest integer
    action.qty = std::round(hedgeQty);    
    // Mark as urgent if significantly over portfolio_max_delta (软指标)
    action.is_urgent = _params.portfolio_max_delta > 0 && std::abs(currentDelta) > _params.portfolio_max_delta * 1.5;
    
    return action;
}

//==========================================================================
// Position Reduction
//==========================================================================

std::vector<const ContractState*> FutuPortfolio::getContractsNeedingReduction(double threshold) const
{
    std::vector<const ContractState*> result;
    for (const auto& c : _contracts)
    {
        if (c.needsPositionReduction(threshold))
        {
            result.push_back(&c);
        }
    }
    return result;
}

} // namespace futu
