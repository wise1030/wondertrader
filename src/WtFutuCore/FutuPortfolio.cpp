/*!
 * \file FutuPortfolio.cpp
 * \brief Unified Portfolio Management Implementation
 * 
 * Merged from: InventoryManager + FutuPortfolio
 * Performance optimized: O(1) contract lookup via hash map
 */
#include "FutuPortfolio.h"
#include "../Includes/WTSDataDef.hpp"
#include "../WTSTools/WTSLogger.h"
#include <cmath>

namespace futu {

FutuPortfolio::FutuPortfolio()
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
        cs.contract_max_delta = contractMaxDelta;
        cs.target_position = targetPosition;
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
    
    // Update daily_pnl whenever markToMarket is called
    updateDailyPnL(code);
}

void FutuPortfolio::addRealizedPnl(const std::string& code, double pnl)
{
    ContractState* cs = getContract(code);
    if (!cs) return;
    cs->realized_pnl += pnl;
    cs->daily_pnl = cs->unrealized_pnl + cs->realized_pnl;
}

void FutuPortfolio::setReferencePrice(const std::string& code, double refPrice)
{
    ContractState* cs = getContract(code);
    if (!cs || refPrice <= 0) return;
    cs->avg_cost = refPrice;
}

void FutuPortfolio::updateDailyPnL(const std::string& code)
{
    ContractState* cs = getContract(code);
    if (!cs) return;
    
    // daily_pnl = unrealized_pnl + realized_pnl
    cs->daily_pnl = cs->unrealized_pnl + cs->realized_pnl;
}

void FutuPortfolio::onPositionUpdate(const char* stdCode, double newPos)
{
    // O(1) lookup
    ContractState* cs = getContract(stdCode);
    if (!cs) return;

    // 记录前一个position用于成交效果日志
    cs->prev_position = cs->position;
    cs->position = newPos;
}

void FutuPortfolio::updatePosition(const std::string& code, double position, double avgCost)
{
    ContractState* cs = getContract(code);
    if (!cs) return;
    
    cs->position = position;
    if (avgCost > 0)
        cs->avg_cost = avgCost;
    
}

//==========================================================================
// Hedging
//==========================================================================

bool FutuPortfolio::needsHedging() const
{
    // 使用 hedge_delta_threshold * portfolio_max_delta 作为对冲触发阈值
    // hedge_delta_threshold 是利用率比例 (默认0.8)，即达到80% max_delta时触发对冲
    double trigger_delta = _params.portfolio_max_delta * _params.hedge_delta_threshold;
    return _params.portfolio_max_delta > 0 && trigger_delta > 0 && std::abs(getTotalDelta()) > trigger_delta;
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
    if (!anchor || anchor->multiplier == 0 || anchor->hedge_ratio == 0)
        return action;

    double currentDelta = getTotalDelta();
    
    // Calculate hedge quantity to reduce delta toward target
    double deltaToHedge = (currentDelta - target_delta) * _params.hedge_ratio;

    // Convert delta to contracts
    double hedgeQty = -deltaToHedge / anchor->hedge_ratio;

    // Round to nearest integer
    action.qty = std::round(hedgeQty);

    // P1-3: anchor 持仓上限 guard — 防止 hedge 导致 anchor 反向超限
    if (anchor->max_position > 0)
    {
        double projected = anchor->position + action.qty;
        double max_pos = anchor->max_position;
        if (projected > max_pos)
        {
            action.qty = max_pos - anchor->position;
            action.is_urgent = true;
            WTSLogger::warn("computeHedge: clamped to anchor max_position (pos={:.0f}, raw_qty={:.0f}, clamped_qty={:.0f})",
                anchor->position, std::round(hedgeQty), action.qty);
        }
        else if (projected < -max_pos)
        {
            action.qty = -max_pos - anchor->position;
            action.is_urgent = true;
            WTSLogger::warn("computeHedge: clamped to anchor -max_position (pos={:.0f}, raw_qty={:.0f}, clamped_qty={:.0f})",
                anchor->position, std::round(hedgeQty), action.qty);
        }
    }

    // Mark as urgent if significantly over portfolio_max_delta (软指标)
    action.is_urgent = action.is_urgent || (_params.portfolio_max_delta > 0 && std::abs(currentDelta) > _params.portfolio_max_delta * 1.5);
    
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
