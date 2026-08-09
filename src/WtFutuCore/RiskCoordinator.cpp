/*!
 * \file RiskCoordinator.cpp
 * \brief 风控协调器实现 (从 StrategyCoordinator 拆分, P1.3 Step 2a)
 *
 * 纯迁移: checkTakerReduce 行为与原 coordinator 实现完全一致。
 */
#include "RiskCoordinator.h"
#include "StrategyCoordinator.h"   // CoordinatorConfig / FutuRiskMonitor / RiskLiquidator (transitive)
#include "FutuPortfolio.h"          // ContractState / getAllContractsSnapshot
#include "OrderRouter.h"
#include "QuotePolicyChain.h"        // _quote_chain->riskWiden
#include "SelfTradeCalibrator.h"    // _self_trade_calibrator->decayCalibration            // Source / OrderSubmitResult / submitSell/Buy
#include <cmath>
#include "../Share/TimeUtils.hpp"
#include "../WTSTools/WTSLogger.h"

namespace futu {

bool RiskCoordinator::checkTakerReduce(wtp::IUftStraCtx* ctx, uint64_t exchange_time_ms)
{
    if (!ctx || !_deps.portfolio || !_deps.order_router)
        return false;
    if (_deps.cfg->taker_reduce_threshold <= 0.0)
        return false;

    // v7.1: 限频计时统一用 replay 时钟 (回测可复现); 未注入时回退墙钟
    uint64_t now_ms = exchange_time_ms > 0 ? exchange_time_ms : TimeUtils::getLocalTimeNow();
    bool triggered = false;

    // A2: Phase 1 - iterate without full-vector copy (was getAllContractsSnapshot:
    //   N x 208B ContractState copy under spinlock every tick). Collect candidates
    //   via forEachContractState (lock held, zero copy), then submit outside lock.
    struct ReduceCandidate {
        std::string code;
        double position;
        double max_position;
        double target;
        double qty;
        double price;
        bool is_long;
        double util;
    };
    std::vector<ReduceCandidate> candidates;

    _deps.portfolio->forEachContractState([&](const ContractState& c) {
        if (c.max_position <= 0 || std::abs(c.position) < 1.0)
            return;
        double util = std::abs(c.position) / c.max_position;
        if (util < _deps.cfg->taker_reduce_threshold)
            return;
        // 每合约限频
        auto it = _last_taker_reduce.find(c.code);
        if (it != _last_taker_reduce.end() && now_ms - it->second < _deps.cfg->taker_reduce_cooldown_ms)
            return;
        // 平掉超出 target x maxPos 的部分 (FAK 对手价, 不追价)
        double target = c.max_position * _deps.cfg->taker_reduce_target_util;
        double qty = std::floor(std::abs(c.position) - target);
        qty = clampReduceQty(qty, c.position);  // P0-2: 统一截断, 不开反向仓
        if (qty < 1.0)
            return;
        bool is_long = c.position > 0;
        double price = is_long ? c.bid1 : c.ask1; // 对手价
        if (price <= 0)
            return;
        candidates.push_back({c.code, c.position, c.max_position, target, qty, price, is_long, util});
    });

    // Phase 2 - submit orders (outside portfolio lock, same as original semantics)
    for (const auto& cand : candidates) {
        WTSLogger::warn("[TAKER_REDUCE] {} util={:.2f} >= {:.2f}: {} {:.0f}@{} (pos={:.0f}/{:.0f} -> target={:.0f})",
                        cand.code,
                        cand.util,
                        _deps.cfg->taker_reduce_threshold,
                        cand.is_long ? "SELL_CLOSE" : "BUY_CLOSE",
                        cand.qty,
                        cand.price,
                        cand.position,
                        cand.max_position,
                        cand.target);

        OrderSubmitResult rr = cand.is_long
            ? _deps.order_router->submitSell(ctx, cand.code.c_str(), cand.price, cand.qty, Source::CLOSEOUT, 1)
            : _deps.order_router->submitBuy(ctx, cand.code.c_str(), cand.price, cand.qty, Source::CLOSEOUT, 1);

        if (rr.rate_limited) {
            WTSLogger::warn("[TAKER_REDUCE] {} rate limited, will retry next cooldown", cand.code);
            continue;
        }
        if (rr.self_trade_blocked) {
            WTSLogger::warn("[TAKER_REDUCE] {} self-trade blocked (MM quotes on the way), will retry", cand.code);
            continue;
        }

        if (!rr.localids.empty()) {
            _last_taker_reduce[cand.code] = now_ms;
            if (_deps.risk_monitor)
                _deps.risk_monitor->recordOrder();
            triggered = true;
        } else {
            WTSLogger::error("[TAKER_REDUCE] {} order FAILED", cand.code);
        }
    }

    return triggered;
}

bool RiskCoordinator::checkRisk(wtp::IUftStraCtx* ctx, const TickContext& tc, bool in_cooloff)
{
    if (!_deps.risk_monitor || !_deps.portfolio)
        return true;

    // Check if previously halted (hard limit)
    if (_deps.risk_monitor->isTradingHalted()) {
        // T2: closeout 窗口内禁止自动恢复 (与 on_trade 路径的 "closeout halt must
        //     persist until on_session_begin" 语义对齐); 正常路径 processCloseout
        //     活跃态已提前 return, 此守卫只影响 T2 的 closeout 窗口复跑。
        if (_deps.risk_monitor->isCloseoutFlattening() || _deps.risk_monitor->isCloseoutTriggered()) {
            return false;
        }
        // 尝试自动恢复(REVERSIBLE halt): checkAndRecover 内部有节流(check_interval_ms)
        // + cooldown + canRecover 全套校验, IRREVERSIBLE 会被拒绝.
        if (_deps.risk_monitor->checkAndRecover(_deps.portfolio) && !_deps.risk_monitor->isTradingHalted()) {
            if (_deps.trading_state) {
                _deps.trading_state->resumeFromRisk();
                _deps.trading_state->unblockLong();
                _deps.trading_state->unblockShort();
            }
            if (_deps.arb_executor) {
                AsyncArbConfig arbCfg = _deps.arb_executor->getConfig();
                arbCfg.enabled.store(true);
                _deps.arb_executor->setConfig(arbCfg);
            }
            WTSLogger::info("StrategyCoordinator[{}]: Recovered from REVERSIBLE halt, resuming operations", tc.code);
            // fall through: 恢复成功后继续走正常风控检查
        } else {
            if (_deps.trading_state) {
                _deps.trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
            }
            // 限频日志：每5秒输出一次，避免刷屏
            {
                uint64_t now_ms = TimeUtils::getLocalTimeNow();
                if (now_ms - _last_halt_log_ms > 5000) {
                    WTSLogger::error("[RISK] Trading still halted (isTradingHalted=true), skipping risk check");
                    _last_halt_log_ms = now_ms;
                }
            }
            return false;
        }
    }

    if (_deps.risk_monitor->checkDeltaRate()) {
        // 使用TradingState方法
        // B3: delta-rate 停机只在此设置; 恢复走下方 violations.empty() 分支的统一
        // 恢复路径 (以 !checkDeltaRate() 为门, 等 RiskMonitor 15s 冷却清除标志后
        // 一次性完整恢复). 不要在此加 else 恢复分支 — 它会对违规类 RISK_HALTED
        // (PAUSE/BLOCK) 误触发, 抢在完整恢复前翻转 qphase, 导致 arb 永久禁用/
        // 方向 block 残留/spread_mult 不复位.
        if (_deps.trading_state) {
            _deps.trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
        }
        // 业务#3: delta 剧烈异动 = 价格正在快速移动, 黏性/追价机制全部停摆,
        // 旧价位双边义务单原样留在场上是最危险场景 (最长滞留 5s cooldown)。
        // 与 PAUSE/HALT 路径对齐: 停机同步撤单。
            if (_deps.cancel_all_quotes) _deps.cancel_all_quotes(ctx);
    }

    // R2.4: 策略性软响应 (delta util 0.8/0.9 → WIDEN_SPREAD, 不产生硬 violation).
    //   在 hard check 之前执行; soft action 不阻断 hard check (两者可叠加).
    //   设计: WIDEN_SPREAD 是策略行为 (调整报价), 不是硬风控 (BLOCK/PAUSE/HALT).
    //   v7.1 无状态化: 每 tick 由当前 util 重算, util 回落即回 1.0,
    //   消除旧 std::max 闩锁 (util 回落后 spread 仍被永久放大直到完整恢复).
    //   5A-2: 状态移入 RiskWidenPolicy。
    {
        double cur_util = _deps.portfolio ? _deps.portfolio->getPortfolioDeltaUtilization() : 0;
        double l1 = _deps.risk_monitor->getRateLimits().position_warning_l1;
        double l2 = _deps.risk_monitor->getRateLimits().position_warning_l2;
        bool halted = _deps.trading_state && _deps.trading_state->qphase == QuotingPhase::RISK_HALTED;
        _deps.quote_chain->riskWiden().tickSoft(cur_util, l1, l2, halted);
    }

    // P0-1.1: Active risk check every tick (复用缓冲, 零堆分配)
    _deps.risk_monitor->checkRiskLimits(_deps.portfolio, _violations_buf);
    auto& violations = _violations_buf;
    if (!violations.empty()) {
        RiskCategory category;
        RiskAction action = _deps.risk_monitor->determineActionWithCategory(violations, category);

        switch (action) {
        case RiskAction::HALT_TRADING:
            // 使用TradingState方法
            if (_deps.trading_state) {
                _deps.trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
            }
            _deps.risk_monitor->haltTrading(category, _deps.portfolio->getTotalPnL());

            // P0-2: halt 后动作补全 — 撤所有做市单
            if (_deps.cancel_all_quotes) _deps.cancel_all_quotes(ctx);
            // 撤所有非做市活跃单
            if (_deps.order_router) {
                _deps.order_router->cancelAllBySource(ctx, Source::CLOSEOUT);
                _deps.order_router->cancelAllBySource(ctx, Source::HEDGING);
                _deps.order_router->cancelAllBySource(ctx, Source::ARBITRAGE);
            }

            // IRREVERSIBLE → 全组合强平(对手价FAK) — P0-1: 统一 RiskLiquidator 原语;
            //   v7.7 业务#2: forceFlatAnchor(仅anchor×delta手数) → forceFlatAll(逐合约实际持仓)
            if (category == RiskCategory::IRREVERSIBLE && _deps.portfolio && _deps.order_router) {
                // ② closeout 窗口内禁 forceFlatAll: closeout(anchor-delta) 是指定平仓者,
                //    forceFlatAll(逐合约) 与之同向卖会超卖(跨源盲区: _inflight_qty 守卫只数 CLOSEOUT).
                //    让 closeout 处理; forceFlatAll 留给非 closeout 的 HALT 场景.
                if (_deps.risk_monitor->isCloseoutFlattening() || _deps.risk_monitor->isCloseoutTriggered()) {
                    WTSLogger::warn("[RISK] HALT IRREVERSIBLE during closeout: forceFlatAll deferred to closeout (avoid cross-source dual-sell)");
                } else {
                    _liquidator.setDeps({_deps.order_router, _deps.portfolio});
                    _liquidator.forceFlatAll(ctx, "HALT IRREVERSIBLE FORCE FLAT");
                }
            }

            if (_deps.arb_executor) {
                AsyncArbConfig arbCfg = _deps.arb_executor->getConfig();
                arbCfg.enabled.store(false);
                _deps.arb_executor->setConfig(arbCfg);
                WTSLogger::error("StrategyCoordinator[{}]: Arbitrage executor disabled due to HALT_TRADING", tc.code);
            }
            break;

        case RiskAction::PAUSE_QUOTING:
            // v7.3: 不可达死分支已删除 — determineActionWithCategory 不再返回 PAUSE_QUOTING
            //   (原判定 long_breach&&short_breach 数学上不可能同时成立)。
            //   仓位控制全部由软连续控制链承担 (skew/force/takerReduce), 硬停只有 HALT。
            break;

        case RiskAction::BLOCK_SIDE_LONG:
            // R2.7: 进入 RISK_HALTED, 走统一恢复路径 (此前 blockLong 后 qphase 仍 NORMAL,
            // 恢复分支要求 qphase==RISK_HALTED → block 永久残留, 仅 channel_ready 可清)
            if (_deps.trading_state) {
                _deps.trading_state->blockLong();
                _deps.trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
            }
            if (_deps.risk_monitor)
                _deps.risk_monitor->pauseQuoting();
            // 业务#4: EXPOSURE breach 恰是最该停 arb 的场景 (毛暴露超限, arb 继续开仓
            // 会进一步放大暴露), 与 HALT/PAUSE 对齐停 executor; 恢复走统一路径复活。
            if (_deps.arb_executor) {
                AsyncArbConfig arbCfg = _deps.arb_executor->getConfig();
                arbCfg.enabled.store(false);
                _deps.arb_executor->setConfig(arbCfg);
                WTSLogger::warn("StrategyCoordinator[{}]: Arbitrage executor disabled due to BLOCK_SIDE_LONG", tc.code);
            }
            WTSLogger::warn("[RISK] BLOCK_SIDE_LONG: halted until recovery");
            break;

        case RiskAction::BLOCK_SIDE_SHORT:
            // R2.7: 同 BLOCK_SIDE_LONG
            if (_deps.trading_state) {
                _deps.trading_state->blockShort();
                _deps.trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
            }
            if (_deps.risk_monitor)
                _deps.risk_monitor->pauseQuoting();
            // 业务#4: 同 BLOCK_SIDE_LONG
            if (_deps.arb_executor) {
                AsyncArbConfig arbCfg = _deps.arb_executor->getConfig();
                arbCfg.enabled.store(false);
                _deps.arb_executor->setConfig(arbCfg);
                WTSLogger::warn("StrategyCoordinator[{}]: Arbitrage executor disabled due to BLOCK_SIDE_SHORT",
                                tc.code);
            }
            WTSLogger::warn("[RISK] BLOCK_SIDE_SHORT: halted until recovery");
            break;

        case RiskAction::WIDEN_SPREAD:
            // R2.5: 分级倍数 — L1(util≥0.8)→×1.5, L2(util≥0.9)→×2.0.
            // (soft check 已在 hard check 之前处理了主要的 WIDEN; 此处处理 WARNING 级别升级路径,
            //  即 determineActionWithCategory 末尾 breachCount>=widen_threshold 的返回)
            // 5A-2: 状态移入 RiskWidenPolicy。
            {
                double cur_util = _deps.portfolio ? _deps.portfolio->getPortfolioDeltaUtilization() : 0;
                double l2 = _deps.risk_monitor->getRateLimits().position_warning_l2;
                _deps.quote_chain->riskWiden().onHardWiden(cur_util, l2);
            }
            break;

            // R2.5/D5: REDUCE_SIZE 已删除 — 做市有最低报价数量要求, 不能 reduce qty;
            //   统一用 WIDEN_SPREAD 分级倍数替代 (加宽 spread 降低成交率, 近似 qty 缩减)

        case RiskAction::FLATTEN_POSITION: {
            // v7.3: 不可达死分支已删除 — breachCount 恒 <=1 (仅 EXPOSURE 产 BREACH,
            //   每 tick 至多一条), flatten_threshold=2 永不可达。
            //   强平职能由 HALT_TRADING 的 IRREVERSIBLE FORCE FLAT 承担。
            break;
        }

        default:
            break;
        }
    } else {
        // Auto-recovery check (仅针对 RISK_HALTED — MARKET/TOXICITY/ERROR 暂停
        // 有各自的恢复路径, 旧代码 !isActive() 会把 MARKET 暂停误翻 NORMAL 造成状态闪烁)
        // B3: delta-rate 停机期间 (!checkDeltaRate() 为 false) 禁止恢复 —
        // 等 RiskMonitor 冷却清除 _delta_rate_breached 后才允许走统一恢复.
        if (_deps.trading_state && _deps.trading_state->qphase == QuotingPhase::RISK_HALTED) {
            // v7.8: 区分 delta-rate-only halt 与 hard violation halt
            // delta-rate breach 是速率问题(瞬时变化太快), 恢复不应被 delta_util 绝对水平阻止,
            // 否则形成死锁: 高delta阻止恢复 -> 无法报价 -> 无法减仓 -> delta无法降低
            bool _v78_hard_violation = _deps.risk_monitor->isTradingHalted() || _deps.risk_monitor->isQuotingPaused();
            bool _v78_delta_cleared = !_deps.risk_monitor->checkDeltaRate();
            if (_v78_delta_cleared && (!_v78_hard_violation || _deps.risk_monitor->canRecover(_deps.portfolio))) {
                // P1-1: resumeFromRisk() unconditionally sets qphase=NORMAL
                // (replaces old 3-call recovery that cleared individual bool flags)
                _deps.trading_state->resumeFromRisk();
                _deps.trading_state->unblockLong();
                _deps.trading_state->unblockShort();

                // P-11 fix: 同步RiskMonitor的atomic状态，保持单一source of truth
                _deps.risk_monitor->resumeQuoting();
                _deps.risk_monitor->unblockLong();
                _deps.risk_monitor->unblockShort();

                // R2: 重置软风控倍数 (WIDEN_SPREAD 分级设置的, 恢复时归 1.0)
                _deps.quote_chain->riskWiden().reset();

                if (_deps.arb_executor) {
                    AsyncArbConfig arbCfg = _deps.arb_executor->getConfig();
                    arbCfg.enabled.store(true);
                    _deps.arb_executor->setConfig(arbCfg);
                }
                WTSLogger::info("StrategyCoordinator[{}]: Risk normalized, resuming operations{}",
                                tc.code,
                                _v78_hard_violation ? "" : " (delta-rate halt recovery)");
            }
        }
    }

    // Check toxicity cooldown
    // 空指针保护 + P0-12: 使用TradingState方法
    // P1-6/U1: enter 用 setQuotingPhase, exit 用 tryResumeFrom(TOXICITY)
    // 避免冷却期结束时在 HALT/ERROR/MARKET 期间被误翻 NORMAL.
    if (in_cooloff) {
        if (_deps.trading_state)
            _deps.trading_state->setQuotingPhase(QuotingPhase::TOXICITY);
        if (_deps.self_trade_calibrator) {
            _deps.self_trade_calibrator->decayCalibration(tc.code, tc.timestamp, _deps.cfg->modules.toxicity_cooloff_ms);
        }
    } else {
        if (_deps.trading_state)
            _deps.trading_state->tryResumeFrom(QuotingPhase::TOXICITY);
    }

    // 空指针保护
    return !_deps.trading_state || _deps.trading_state->qphase != QuotingPhase::RISK_HALTED;
}

} // namespace futu
