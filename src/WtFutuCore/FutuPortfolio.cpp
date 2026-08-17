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
    // UnifiedNetBook：unrealized 由引擎 profit 权威更新，markToMarket 不再重算成本簿。
    updateDailyPnL(code);
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
        cs.day_open_unrealized = 0;
        cs.day_unrealized_anchor_valid = false;
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
    // UnifiedNetBook：成交后的净仓与 PnL 由 processTradeFill 从引擎 profit 镜像，
    // 本函数只保留 prev_position 供成交效果日志使用。
    (void)is_long_side;
    (void)offset;
    (void)vol;
    (void)price;
}

void FutuPortfolio::resyncPosition(const std::string& code, double engine_net)
{
    RecursiveSpinGuard _g(_lock);
    markAggregatesDirty(); // F4
    ContractState* cs = getContract(code);
    if (!cs)
        return;

    cs->prev_position = cs->position;
    cs->position = engine_net;
    checkOvershootSignFlip(code.c_str(), cs->prev_position, cs->position); // B5
    updateDailyPnL(code);
    // 引擎再同步意味着本地成本簿出现过偏差，保守标记 stale。
    cs->shadow_stale = true;
    WTSLogger::warn("Portfolio[{}] shadow cost basis stale after engine resync (engine_net={:.1f})",
                    code,
                    engine_net);
}

void FutuPortfolio::setShadowFromEngine(const std::string& code,
                                        double engine_net,
                                        double engine_realized,
                                        double engine_unrealized)
{
    RecursiveSpinGuard _g(_lock);
    ContractState* cs = getContract(code);
    if (!cs)
        return;

    cs->shadow_net = engine_net;
    cs->shadow_realized_pnl = engine_realized;

    if (!cs->day_unrealized_anchor_valid) {
        cs->day_open_unrealized = engine_unrealized;
        cs->day_unrealized_anchor_valid = true;
    }
    double day_unrealized = engine_unrealized - cs->day_open_unrealized;
    cs->shadow_unrealized_pnl = day_unrealized;

    // UnifiedNetBook 为唯一权威：canonical 字段直接镜像引擎 net/profit。
    double prev = cs->position;
    cs->position = engine_net;
    cs->realized_pnl = engine_realized;
    cs->unrealized_pnl = day_unrealized;
    checkOvershootSignFlip(code.c_str(), prev, engine_net);
    updateDailyPnL(code);
}

void FutuPortfolio::markShadowStale(const std::string& code)
{
    RecursiveSpinGuard _g(_lock);
    ContractState* cs = getContract(code);
    if (cs)
        cs->shadow_stale = true;
}

void FutuPortfolio::clearShadowStale(const std::string& code)
{
    RecursiveSpinGuard _g(_lock);
    ContractState* cs = getContract(code);
    if (cs)
        cs->shadow_stale = false;
}

bool FutuPortfolio::isShadowStale(const std::string& code) const
{
    RecursiveSpinGuard _g(_lock);
    const ContractState* cs = getContract(code);
    return cs ? cs->shadow_stale : false;
}

bool FutuPortfolio::hasStaleCostBasis() const
{
    RecursiveSpinGuard _g(_lock);
    for (const auto& c : _contracts) {
        if (c.shadow_stale)
            return true;
    }
    return false;
}

double FutuPortfolio::getShadowNet(const std::string& code) const
{
    RecursiveSpinGuard _g(_lock);
    const ContractState* cs = getContract(code);
    return cs ? cs->shadow_net : 0.0;
}

double FutuPortfolio::getShadowRealizedPnl(const std::string& code) const
{
    RecursiveSpinGuard _g(_lock);
    const ContractState* cs = getContract(code);
    return cs ? cs->shadow_realized_pnl : 0.0;
}

double FutuPortfolio::getShadowUnrealizedPnl(const std::string& code) const
{
    RecursiveSpinGuard _g(_lock);
    const ContractState* cs = getContract(code);
    return cs ? cs->shadow_unrealized_pnl : 0.0;
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
