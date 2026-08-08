/*!
 * \file FutuPortfolio.cpp
 * \brief Unified Portfolio Management Implementation
 *
 * Merged from: InventoryManager + FutuPortfolio
 * Performance optimized: O(1) contract lookup via hash map
 */
#include "FutuPortfolio.h"
#include "SpreadArbitrageManager.h" // B5: onOvershootDetected / hasActiveCloseIntent
#include "../Includes/WTSDataDef.hpp"
#include "../Share/TimeUtils.hpp"
#include "../WTSTools/WTSLogger.h"
#include <cmath>

namespace futu
{

FutuPortfolio::FutuPortfolio()
{
    RecursiveSpinGuard _g(_lock);
}

void FutuPortfolio::addContract(const std::string& code,
                                double multiplier,
                                double tickSize,
                                double hedgeRatio,
                                double maxPosition,
                                double contractMaxDelta,
                                double targetPosition)
{
    RecursiveSpinGuard _g(_lock);
    markAggregatesDirty(); // F4
    // O(1) check if contract already exists
    auto it = _code_to_state.find(code);
    if (it != _code_to_state.end()) {
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
    cs.contract_max_delta = contractMaxDelta; // 单合约 delta 软指标
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
    RecursiveSpinGuard _g(_lock);
    markAggregatesDirty(); // F4
    auto it = _code_to_state.find(code);
    if (it == _code_to_state.end())
        return;

    size_t idx = it->second;

    // If not the last element, swap with last and update its index
    if (idx != _contracts.size() - 1) {
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
    RecursiveSpinGuard _g(_lock);
    markAggregatesDirty(); // F4
    if (!tick)
        return;

    // O(1) lookup
    ContractState* cs = getContract(stdCode);
    if (!cs)
        return;

    // M5: 当日无成交 tick 的 price()==0, 直接覆盖会把策略层刚写入的 mid 打回 0
    //     -> exposure() 对 last_price<=0 返回 0 -> 毛暴露硬风控静默失效。
    //     仅正价才写, 否则保留 markToMarket/上一有效值。
    if (tick->price() > 0)
        cs->last_price = tick->price();
    cs->bid1 = tick->bidprice(0);
    cs->ask1 = tick->askprice(0);
    cs->last_update = TimeUtils::getLocalTimeNow();
}

void FutuPortfolio::markToMarket(const std::string& code, double lastPrice)
{
    RecursiveSpinGuard _g(_lock);
    markAggregatesDirty(); // F4
    ContractState* cs = getContract(code);
    if (!cs)
        return;

    cs->last_price = lastPrice;
    // v7.1: 分向簿有效时按方向分别计算浮盈 (与引擎口径一致,
    //       不受 MM+arb 交织对净额均价的污染影响)
    if (cs->long_qty > 0.01 || cs->short_qty > 0.01) {
        cs->unrealized_pnl =
            (cs->long_qty > 0.01 && cs->long_avg > 0 ? (lastPrice - cs->long_avg) * cs->long_qty * cs->multiplier : 0) +
            (cs->short_qty > 0.01 && cs->short_avg > 0 ? (cs->short_avg - lastPrice) * cs->short_qty * cs->multiplier
                                                       : 0);
    } else if (cs->position != 0 && cs->avg_cost > 0) {
        cs->unrealized_pnl = (lastPrice - cs->avg_cost) * cs->position * cs->multiplier;
    } else if (cs->position == 0) {
        // 平仓后清零浮盈, 避免残留浮盈与 realized_pnl 双重计数
        cs->unrealized_pnl = 0;
    }

    // Update daily_pnl whenever markToMarket is called
    updateDailyPnL(code);
}

void FutuPortfolio::addRealizedPnl(const std::string& code, double pnl)
{
    RecursiveSpinGuard _g(_lock);
    markAggregatesDirty(); // F4
    ContractState* cs = getContract(code);
    if (!cs)
        return;
    cs->realized_pnl += pnl;
    cs->daily_pnl = cs->unrealized_pnl + cs->realized_pnl;
}

void FutuPortfolio::setReferencePrice(const std::string& code, double refPrice)
{
    RecursiveSpinGuard _g(_lock);
    markAggregatesDirty(); // F4
    ContractState* cs = getContract(code);
    if (!cs || refPrice <= 0)
        return;
    cs->avg_cost = refPrice;
    // v7.1: 分向簿同步锚定 (隔夜持仓日初成本基准 = 昨收)
    if (cs->long_qty > 0.01)
        cs->long_avg = refPrice;
    if (cs->short_qty > 0.01)
        cs->short_avg = refPrice;
}

void FutuPortfolio::updateDailyPnL(const std::string& code)
{
    RecursiveSpinGuard _g(_lock);
    markAggregatesDirty(); // F4
    ContractState* cs = getContract(code);
    if (!cs)
        return;

    // daily_pnl = unrealized_pnl + realized_pnl
    cs->daily_pnl = cs->unrealized_pnl + cs->realized_pnl;
}

void FutuPortfolio::resetDailyPnl()
{
    RecursiveSpinGuard _g(_lock);
    markAggregatesDirty(); // F4
    for (auto& cs : _contracts) {
        cs.realized_pnl = 0;
        cs.unrealized_pnl = 0;
        cs.daily_pnl = 0;
        // 隔夜持仓重置成本基准为 0 — 触发 StrategyCoordinator 首 tick 的
        // pre_close 分支 (position!=0 && avg_cost==0) 以昨收重设 avg_cost.
        // 否则 markToMarket 用昨日 avg_cost 重算浮盈, 把昨日盈亏混入今日 daily_pnl,
        // 违背"日内 PnL 不跨日累计"的风控语义 (max_loss 误触).
        if (cs.position != 0) {
            cs.avg_cost = 0;
            cs.long_avg = 0; // v7.1: 分向簿同步重置, setReferencePrice 重锚
            cs.short_avg = 0;
        }
    }
}

void FutuPortfolio::onPositionUpdate(const char* stdCode, double newPos)
{
    RecursiveSpinGuard _g(_lock);
    markAggregatesDirty(); // F4
    // O(1) lookup
    ContractState* cs = getContract(stdCode);
    if (!cs)
        return;

    // 记录前一个position用于成交效果日志
    cs->prev_position = cs->position;
    cs->position = newPos;

    checkOvershootSignFlip(stdCode, cs->prev_position, newPos); // B5
}

void FutuPortfolio::updatePosition(const std::string& code, double position, double avgCost)
{
    RecursiveSpinGuard _g(_lock);
    markAggregatesDirty(); // F4
    ContractState* cs = getContract(code);
    if (!cs)
        return;

    double prev = cs->position; // B5: 捕获前值 (本函数不维护 prev_position 字段)
    cs->position = position;
    if (avgCost > 0)
        cs->avg_cost = avgCost;

    checkOvershootSignFlip(code.c_str(), prev, position); // B5
}

void FutuPortfolio::onTradeFill(const std::string& code, bool is_long_side, int offset, double vol, double price)
{
    RecursiveSpinGuard _g(_lock);
    markAggregatesDirty(); // F4
    ContractState* cs = getContract(code);
    if (!cs || vol <= 0)
        return;

    cs->prev_position = cs->position;

    double& side_qty = is_long_side ? cs->long_qty : cs->short_qty;
    double& side_avg = is_long_side ? cs->long_avg : cs->short_avg;

    if (offset == 0) // OPEN: 加仓, 加权均价
    {
        side_avg = (side_qty > 0.01) ? (side_avg * side_qty + price * vol) / (side_qty + vol) : price;
        side_qty += vol;
    } else // CLOSE(1)/CLOSETODAY(2): 对持有均价实现盈亏
    {
        double close_qty = std::min(vol, side_qty);
        if (side_avg > 0 && close_qty > 0.01) {
            double realized = is_long_side ? (price - side_avg) * close_qty * cs->multiplier
                                           : (side_avg - price) * close_qty * cs->multiplier;
            cs->realized_pnl += realized;
        }
        side_qty -= close_qty;
        if (side_qty < 0.01) {
            side_qty = 0;
            side_avg = 0;
        }

        // 平仓量超出持有量的部分 = 净仓模式下的反向开仓
        double excess = vol - close_qty;
        if (excess > 0.01) {
            double& opp_qty = is_long_side ? cs->short_qty : cs->long_qty;
            double& opp_avg = is_long_side ? cs->short_avg : cs->long_avg;
            opp_avg = (opp_qty > 0.01) ? (opp_avg * opp_qty + price * excess) / (opp_qty + excess) : price;
            opp_qty += excess;
        }
    }

    // 兼容字段: 净持仓 + 主导侧均价 (markToMarket 已改分向, 仅供遗留消费者)
    cs->position = cs->long_qty - cs->short_qty;
    cs->avg_cost = (cs->long_qty >= cs->short_qty) ? cs->long_avg : cs->short_avg;

    checkOvershootSignFlip(code.c_str(), cs->prev_position, cs->position); // B5
    updateDailyPnL(code);
}

void FutuPortfolio::resyncPosition(const std::string& code, double engine_net)
{
    RecursiveSpinGuard _g(_lock);
    markAggregatesDirty(); // F4
    ContractState* cs = getContract(code);
    if (!cs)
        return;

    cs->prev_position = cs->position;
    if (engine_net > 0.01) {
        cs->long_qty = engine_net;
        cs->short_qty = 0;
        cs->short_avg = 0;
        if (cs->long_avg <= 0)
            cs->long_avg = cs->last_price;
        cs->avg_cost = cs->long_avg;
    } else if (engine_net < -0.01) {
        cs->short_qty = -engine_net;
        cs->long_qty = 0;
        cs->long_avg = 0;
        if (cs->short_avg <= 0)
            cs->short_avg = cs->last_price;
        cs->avg_cost = cs->short_avg;
    } else {
        cs->long_qty = cs->short_qty = 0;
        cs->long_avg = cs->short_avg = 0;
        cs->avg_cost = 0;
    }
    cs->position = engine_net;
    checkOvershootSignFlip(code.c_str(), cs->prev_position, cs->position); // B5
    updateDailyPnL(code);
}

void FutuPortfolio::checkOvershootSignFlip(const char* code, double prev, double now)
{
    RecursiveSpinGuard _g(_lock);
    if (!_arb_manager)
        return;
    if (prev == 0.0 || prev * now >= 0.0)
        return; // 未翻转 (含从零建仓/回到零)
    if (_arb_manager->hasActiveCloseIntent(code)) {
        WTSLogger::error("Portfolio[{}] OVERSHOOT sign-flip: {:.1f} -> {:.1f} during arb close", code, prev, now);
        _arb_manager->onOvershootDetected(code);
    }
}

//==========================================================================
// Position Reduction

std::vector<const ContractState*> FutuPortfolio::getContractsNeedingReduction(double threshold) const
{
    RecursiveSpinGuard _g(_lock);
    std::vector<const ContractState*> result;
    for (const auto& c : _contracts) {
        if (c.needsPositionReduction(threshold)) {
            result.push_back(&c);
        }
    }
    return result;
}

void FutuPortfolio::smoothUpdateHedgeRatio(const std::string& code, double beta, long sample_count)
{
    RecursiveSpinGuard _g(_lock);
    ContractState* cs = getContract(code);
    if (!cs)
        return;
    if (!cs->hedge_ratio_initialized && cs->last_price > 0 && std::isfinite(beta) && beta > 0) {
        cs->hedge_ratio = beta;
        cs->hedge_ratio_initialized = true;
    } else {
        bool should_update = (sample_count >= 100);
        if (should_update && cs->hedge_ratio > 0) {
            double change_ratio = std::abs(beta - cs->hedge_ratio) / cs->hedge_ratio;
            if (change_ratio > 0.2) {
                beta = cs->hedge_ratio * (1.0 + (beta > cs->hedge_ratio ? 0.2 : -0.2));
            }
        }
        if (should_update && std::isfinite(beta) && beta > 0) {
            cs->hedge_ratio = beta;
        }
    }
    // 沿用旧语义: 不置聚合脏标 (原裸指针直写不置脏, 最晚下一 tick onTick 置脏收敛)
}

} // namespace futu
