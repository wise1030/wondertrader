/*!
 * \file CloseoutOrchestrator.cpp
 * \brief 收盘平仓编排器实现 (自 UftFutuMmStrategy 搬移, 逻辑零修改)
 */
#include "CloseoutOrchestrator.h"
#include "StrategyCoordinator.h"   // 19.34: TickContext / CoordinatorConfig / CloseoutSub
#include "CloseoutExecutor.h"
#include "FutuRiskMonitor.h"
#include "FutuPortfolio.h"
#include "TradingState.h"
#include "FutuQuoter.h"
#include "OrderRouter.h"
#include "../Includes/IUftStraCtx.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/TimeUtils.hpp"
#include "../WTSTools/WTSLogger.h"

namespace futu
{


//==============================================================================
// 19.34: 触发决策/状态机解释 — 自 CloseoutTrigger 并入 (CloseoutTrigger.cpp 全文,
// 行为逐行保持; _deps 字段名一致 [同为 ApplicationDeps 成员], 仅类名/日志前缀不变)
//==============================================================================
bool CloseoutOrchestrator::processTrigger(wtp::IUftStraCtx* ctx, TickContext& tc)
{
    bool active;
    {
        // 与 onTick 同一把递归锁: processTrigger(MdSpi Stage0) vs onOrderEvent(TdSpi)
        RecursiveSpinGuard _g(_lock);
        active = processTriggerImpl(ctx, tc);
    }
    _trigger_active = active;  // Md 线程单写单读 (Stage0 → onTick 同线程顺序)
    return active;
}

bool CloseoutOrchestrator::processTriggerImpl(wtp::IUftStraCtx* ctx, TickContext& tc)
{
    if (!_deps.risk_monitor) {
        return false;
    }

    // 至少有一个触发点启用才继续
    if (_deps.cfg->closeout_minutes_before <= 0 && (_deps.cfg->night_close_time == 0 || _deps.cfg->night_minutes_before <= 0)) {
        return false;
    }

    CloseoutSub state = _deps.risk_monitor->getCloseoutSub();
    uint32_t closeTime = _deps.cfg->close_time;

    switch (state) {
    case CloseoutSub::IDLE: {
        bool triggered = _deps.risk_monitor->checkCloseout(tc.time_hms, closeTime);
        if (triggered) {
            if (_deps.cancel_all_quotes) _deps.cancel_all_quotes(ctx);
            if (_deps.flush_bilateral_stats) {
                uint32_t cur_hhmm = (tc.time_hms >= 10000) ? tc.time_hms / 100 : tc.time_hms;
                uint32_t secs = ctx->stra_get_secs() / 1000;
                _deps.flush_bilateral_stats(ctx, cur_hhmm, secs);
            }

            if (_deps.cfg->closeout_flatten_position && _deps.portfolio) {
                _deps.risk_monitor->markCloseoutDraining(tc.timestamp);
            } else {
                _deps.risk_monitor->markCloseoutCompleted(tc.timestamp);
            }
            return true;
        }
        return false;
    }

    case CloseoutSub::FAILED: {
        uint64_t now_ms = tc.timestamp;
        // 非交易时段不 retry: 收盘/休市期间发出的减仓单必被柜台拒绝,
        // 空耗 retry 配额 (2026-08-17: 15:00 收盘后 15:16 仍在无效 retry)
        if (!tc.is_trading_session)
            return true;
        if (_deps.risk_monitor->checkCloseoutRetry(now_ms)) {
            if (_deps.cancel_all_quotes) _deps.cancel_all_quotes(ctx);
            if (_deps.portfolio) {
                _deps.risk_monitor->markCloseoutDraining(now_ms);
            }
        }
        return true;
    }

    case CloseoutSub::TRIGGERED:
    case CloseoutSub::DRAINING:
    case CloseoutSub::ASSESSING:
    case CloseoutSub::EXECUTING:
    case CloseoutSub::RETRYING:
        return true;

    case CloseoutSub::COMPLETED: {
        //======================================================================
        // 区分夜盘/白盘 closeout 完成
        //
        // 夜盘平仓完成后，立即重置状态+恢复做市。
        // 白盘 closeout 完成后，不再重置（终态，直到日内交易结束）。
        //
        // 旧逻辑: 等 currentHour>=6 才 reset → 凌晨完成时 hour=0 不满足
        //         → 整个白盘都不做市！
        // 新逻辑: 夜盘 closeout 完成立即 reset + resume，白盘 closeout
        //         在 minutes_before=15 时才重新触发 (14:45)，期间正常做市。
        //======================================================================
        const auto& closeoutInfo = _deps.risk_monitor->getCloseoutSubInfo();

        if (_deps.cfg->night_close_time > 0 && closeoutInfo.is_night_closeout) {
            // 夜盘 closeout 完成 → 立即 reset，让白盘可以正常做市
            _deps.risk_monitor->resetCloseout();
            // Bug C: 不调 exitToQuoting - phase 保持 CLOSEOUT, 报价暂停, 等夜盘收盘恢复.
            // (旧代码此处 exitToQuoting -> 收盘前 90s 报价器恢复重建仓, 把 anchor 对冲打掉.
            //  on_session_begin 仅进程启动触发, 常驻系统日盘开盘无 session_begin, 故恢复点
            //  放 processTick 夜盘收盘检测, 见 MM pipeline 前)
            WTSLogger::info("[CLOSEOUT] Night session closeout completed, holding quoting paused until night close {}",
                            _deps.cfg->night_close_time);
            // 重新检查白盘 closeout (09:00 不会触发，14:45 才触发)
            bool triggered = _deps.risk_monitor->checkCloseout(tc.time_hms, closeTime);
            if (triggered) {
                if (_deps.cancel_all_quotes) _deps.cancel_all_quotes(ctx);
                if (_deps.cfg->closeout_flatten_position && _deps.portfolio) {
                    _deps.risk_monitor->markCloseoutDraining(tc.timestamp);
                } else {
                    _deps.risk_monitor->markCloseoutCompleted(tc.timestamp);
                }
                return true;
            }
            return false;
        }

        // 白盘 closeout 完成 → 终态，不做市直到日内交易结束
        // 只在首次进入 COMPLETED 时打日志+halt，避免每 tick 循环
        if (_deps.trading_state) {
            _deps.trading_state->enterCloseout();
        }
        if (_deps.cancel_all_quotes) _deps.cancel_all_quotes(ctx);
        return true;
    }

    default:
        return false;
    }
}

void CloseoutOrchestrator::onTick(wtp::IUftStraCtx* ctx, wtp::WTSTickData* tick, uint64_t now_ms)
{
    RecursiveSpinGuard _g(_lock);
    // 19.34: 触发态经 _trigger_active 成员 (processTrigger 同线程先写), 消桥接参数
    const bool closeout_triggered = _trigger_active;
    // v7.1: replay 时钟注入 (mark* 与 RiskMonitor._current_time 同为 replay 基准)
    {
        _now_ms = now_ms;  // 19.35: 经参数下发, 不再重复转换
    }

    // === Closeout hedge trigger ===
    if (closeout_triggered && _deps.flatten_position && _deps.risk_monitor->isCloseoutFlattening() &&
        !_closeout_hedge_executed) {
        _deps.trading_state->enterCloseout();
        if (_deps.quoters) {
            for (auto& [code, quoter] : *_deps.quoters) {
                if (quoter)
                    quoter->cancelAll(ctx);
            }
        }
        _closeout_hedge_pending = true;
        _closeout_hedge_wait_ticks = 0;
        _closeout_hedge_executed = true;
        WTSLogger::warn("UftFutuMmStrategy[{}] CLOSEOUT: halted + cancelAll, hedge deferred {} ticks",
                        _deps.strategy_id,
                        CLOSEOUT_HEDGE_WAIT_TICKS);
    }

    // Deferred CloseoutExecutor start
    if (_closeout_hedge_pending) {
        _closeout_hedge_wait_ticks++;
        if (_closeout_hedge_wait_ticks >= CLOSEOUT_HEDGE_WAIT_TICKS) {
            WTSLogger::warn("UftFutuMmStrategy[{}] CLOSEOUT: starting CloseoutExecutor after {} ticks",
                            _deps.strategy_id,
                            _closeout_hedge_wait_ticks);
            executeHedge(ctx);
            _closeout_hedge_pending = false;
            _closeout_hedge_wait_ticks = 0;
        }
    }

    // Run CloseoutExecutor every tick if active
    if (_deps.executor && _deps.executor->isActive()) {
        // T2: HALT 期间 executor 不得继续发单 — 原逻辑只看在途计数,
        //   HALT 撤销 CLOSEOUT 在途单后 inflight=0 -> 下一轮继续发。
        //   转 FAILED 终态, 由风控恢复/retry 机制接管 (有界 max_retries)。
        //   注意: closeout 流程自身只 pauseQuoting 不 haltTrading, 此门不误伤。
        // v7.9: 仅 IRREVERSIBLE halt 阻断。REVERSIBLE halt (报单错误等)
        //   不应拦截减仓 — closeout 是风险收敛, 与 halt 目的不悖;
        //   且 REVERSIBLE halt 只撤 MM quoter 单, 不动 OrderRouter 的
        //   closeout 在途单, T2 重发循环问题在此类 halt 下不存在。
        //   (2026-08-17 ao 实盘: 报单错误 halt 锁死收盘减仓, 裸仓过夜)
        if (_deps.risk_monitor->isTradingHalted() &&
            _deps.risk_monitor->getHaltCategory() == RiskCategory::IRREVERSIBLE) {
            if (_deps.risk_monitor->getCloseoutSub() != CloseoutSub::FAILED) {
                WTSLogger::error("[CLOSEOUT] Trading halted (IRREVERSIBLE) during flattening, abort executor -> FAILED");
                _deps.risk_monitor->markCloseoutFailed(_now_ms > 0 ? _now_ms : TimeUtils::getLocalTimeNow());
            }
            return;
        }

        ContractState anchor_buf;
        const ContractState* anchorState =
            _deps.portfolio->getContractSnapshot(*_deps.anchor_code, anchor_buf) ? &anchor_buf : nullptr;
        MarketSnapshot snap;
        snap.bid1 = anchorState ? anchorState->bid1 : 0;
        snap.ask1 = anchorState ? anchorState->ask1 : 0;
        snap.bid1_qty = tick->bidqty(0);
        snap.ask1_qty = tick->askqty(0);
        snap.price_tick = anchorState ? anchorState->tick_size : 0;
        snap.upper_limit = tick->upperlimit();
        snap.lower_limit = tick->lowerlimit();
        {
            uint32_t at = tick->actiontime();
            uint32_t hh = at / 10000000;
            uint32_t mm = (at / 100000) % 100;
            uint32_t ss = (at / 1000) % 100;
            uint32_t mmm = at % 1000;
            snap.timestamp_ms = static_cast<uint64_t>(hh) * 3600000ULL + static_cast<uint64_t>(mm) * 60000ULL +
                                static_cast<uint64_t>(ss) * 1000ULL + mmm;
        }
        _deps.executor->run(ctx, snap);

        if (_deps.executor->isCompleted()) {
            if (_deps.risk_monitor->getCloseoutSub() != CloseoutSub::COMPLETED) {
                _deps.risk_monitor->markCloseoutCompleted(_now_ms > 0 ? _now_ms : TimeUtils::getLocalTimeNow());
            }
        } else if (_deps.executor->isFailed()) {
            _deps.risk_monitor->markCloseoutFailed(_now_ms > 0 ? _now_ms : TimeUtils::getLocalTimeNow());
        }
    }

    // Reset hedge flags on terminal/transient closeout states
    auto cs = _deps.risk_monitor->getCloseoutSub();
    if (cs == CloseoutSub::IDLE || cs == CloseoutSub::FAILED || cs == CloseoutSub::RETRYING) {
        _closeout_hedge_executed = false;
        _closeout_hedge_pending = false;
        _closeout_hedge_wait_ticks = 0;
        if (_deps.executor && !_deps.executor->isCompleted())
            _deps.executor->reset();
    }
}

void CloseoutOrchestrator::executeHedge(wtp::IUftStraCtx* ctx)
{
    RecursiveSpinGuard _g(_lock);
    //============================================================
    // CloseoutExecutor 启动入口
    // 实际执行逻辑在 CloseoutExecutor::run() 中，由 onTick 每 tick 调用。
    // 本函数只负责一次性的 start()：计算 close_time_ms，启动执行器。
    //============================================================

    if (!_deps.executor) {
        WTSLogger::error("UftFutuMmStrategy[{}] CloseoutExecutor is null!", _deps.strategy_id);
        return;
    }

    // 已经在运行（retry 重入场景），不重复 start
    if (!_deps.executor->isIdle())
        return;

    // P1-2: closeout 决策前从策略引擎同步持仓
    for (const auto& c : _deps.portfolio->getAllContractsSnapshot()) {
        double actual = ctx->stra_get_local_position(c.code.c_str());
        if (std::abs(c.position - actual) > 0.01) {
            WTSLogger::info("UftFutuMmStrategy[{}] Portfolio sync before closeout: {} {:.0f}->{:.0f}",
                            _deps.strategy_id,
                            c.code,
                            c.position,
                            actual);
            _deps.portfolio->onPositionUpdate(c.code.c_str(), actual);
        }
    }

    double totalDelta = _deps.portfolio->getNetDelta();

    if (std::abs(totalDelta) < 0.01) {
        WTSLogger::info("UftFutuMmStrategy[{}] Closeout: No position to hedge (Delta=0)", _deps.strategy_id);
        if (_deps.risk_monitor->getCloseoutSub() != CloseoutSub::COMPLETED) {
            _deps.risk_monitor->markCloseoutCompleted(_now_ms > 0 ? _now_ms : TimeUtils::getLocalTimeNow());
        }
        return;
    }

    // 获取锚定合约信息
    ContractState anchor_buf;
    const ContractState* anchorState =
        _deps.portfolio->getContractSnapshot(*_deps.anchor_code, anchor_buf) ? &anchor_buf : nullptr;
    if (!anchorState) {
        WTSLogger::error("UftFutuMmStrategy[{}] Closeout failed: anchor contract {} not found",
                         _deps.strategy_id,
                         *_deps.anchor_code);
        return;
    }

    double hedgeRatio = anchorState->hedge_ratio;
    if (hedgeRatio <= 0) {
        WTSLogger::error("UftFutuMmStrategy[{}] Closeout failed: invalid hedgeRatio={}", _deps.strategy_id, hedgeRatio);
        return;
    }

    // 计算 close_time_ms (ms-from-midnight).
    // Bug A: 夜盘 closeout 用夜盘收盘时间(HHMM)作 deadline, 非恒用日盘 150000.
    // 旧代码恒用 _deps.close_time(150000) -> 夜盘 time_left=14h -> urgency=0 ->
    // PASSIVE tier -> 平仓单不积极. 现按 is_night_closeout 取夜盘收盘时间.
    uint32_t close_hhmmss = _deps.close_time; // 默认日盘 HHMMSS, e.g. 150000
    if (_deps.risk_monitor && _deps.risk_monitor->getCloseoutSubInfo().is_night_closeout &&
        _deps.risk_monitor->getNightCloseTime() > 0) {
        uint32_t nct = _deps.risk_monitor->getNightCloseTime(); // HHMM, e.g. 100
        close_hhmmss = (nct / 100) * 10000 + (nct % 100) * 100; // HHMM -> HHMMSS
    }
    uint32_t hh = close_hhmmss / 10000;
    uint32_t mm = (close_hhmmss / 100) % 100;
    uint32_t ss = close_hhmmss % 100;
    uint64_t close_time_ms = static_cast<uint64_t>(hh) * 3600000ULL + static_cast<uint64_t>(mm) * 60000ULL +
                             static_cast<uint64_t>(ss) * 1000ULL;

    // 启动 CloseoutExecutor
    WTSLogger::warn("UftFutuMmStrategy[{}] CLOSEOUT: starting CloseoutExecutor "
                    "(code={}, delta={:.2f}, hedge_ratio={:.2f}, close_ms={})",
                    _deps.strategy_id,
                    *_deps.anchor_code,
                    totalDelta,
                    hedgeRatio,
                    close_time_ms);

    _deps.executor->start(ctx, _deps.anchor_code->c_str(), close_time_ms, hedgeRatio);

    // 标记 FLATTENING (executor 已启动，等待渐进成交)
    // 时间戳统一 epoch ms (搬移时修复: 旧代码在此用了压缩时间戳)
    _deps.risk_monitor->markCloseoutDraining(_now_ms > 0 ? _now_ms : TimeUtils::getLocalTimeNow());
}

void CloseoutOrchestrator::onOrderEvent(
    wtp::IUftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isCanceled, double leftQty, uint64_t now_ms)
{
    RecursiveSpinGuard _g(_lock);
    // 判定 closeout 单: OrderRouter 来源标记(主) + pending_ids 兜底
    bool is_closeout_order = (_closeout_pending_ids.find(localid) != _closeout_pending_ids.end()) ||
                             (_deps.order_router && _deps.order_router->isOrderFromSource(localid, Source::CLOSEOUT));

    if (!_deps.risk_monitor || !_deps.risk_monitor->isCloseoutFlattening() || !is_closeout_order)
        return;

    if (isCanceled) {
        // M9: 区分"真拒单"与"FAK 部分成交剩余撤单/风控主动撤单":
        //   - FAK 部分成交后交易所撤剩余 (leftQty<totalQty): 正常分批流程,
        //     交给 executor 轮次逻辑继续, 误判 FAILED 会无意义消耗 max_retries(3),
        //     耗尽后 CLOSEOUT_FAILED 永久挂起、仓位过夜。
        //   - HALT 路径 cancelAllBySource(CLOSEOUT) 的主动撤单同理不判 FAILED。
        double total_qty = 0;
        if (_deps.order_router) {
            for (const auto& o : _deps.order_router->getActiveOrders(Source::CLOSEOUT)) {
                if (o.localid == localid) {
                    total_qty = o.qty;
                    break;
                }
            }
        }
        bool zero_fill_reject = (total_qty <= 0) || (leftQty >= total_qty - 1e-9);
        bool self_canceled = _deps.risk_monitor->isTradingHalted();

        _closeout_pending_ids.erase(localid);
        if (zero_fill_reject && !self_canceled) {
            WTSLogger::warn(
                "[CLOSEOUT] Order rejected (zero fill) during flattening: code={} localid={}, marking FAILED for retry",
                stdCode,
                localid);
            _deps.risk_monitor->markCloseoutFailed(now_ms);
        } else {
            WTSLogger::info("[CLOSEOUT] Order canceled with partial fill or self-canceled: code={} localid={} "
                            "left={}/{}, continue executor rounds",
                            stdCode,
                            localid,
                            leftQty,
                            total_qty);
        }
    } else if (leftQty == 0) {
        // Order fully filled — check if position is now flat
        _closeout_pending_ids.erase(localid);
        double totalDelta = _deps.portfolio->getTotalDelta();
        if (std::abs(totalDelta) < 0.01) {
            WTSLogger::info("[CLOSEOUT] All positions flattened, marking COMPLETED");
            _deps.risk_monitor->markCloseoutCompleted(now_ms);
        }
        // else: still have positions, wait for more fills or next tick
    }
}

void CloseoutOrchestrator::finalizeAtSessionEnd(uint64_t now_ms)
{
    RecursiveSpinGuard _g(_lock);
    // session_end closeout 状态强制收尾
    // 根因:closeout FLATTENING → COMPLETED 仅在 on_order 回调 + getTotalDelta()<0.01
    // 这一条路径上转移。若 hedge 单未全成、或成交后 Delta 因取整/口径残留(如 1 手)
    // 不到阈值,state 卡 FLATTENING。session 结束是硬边界:此后不可能再有 tick/order
    // 推动状态,必须当场强制收尾,避免下一日 resetCloseout 被状态机拒绝(canTransitionTo
    // 仅允许 COMPLETED→IDLE)导致 state 永久死锁。
    auto cs_at_end = _deps.risk_monitor->getCloseoutSub();
    if (cs_at_end != CloseoutSub::IDLE && cs_at_end != CloseoutSub::COMPLETED) {
        WTSLogger::warn("UftFutuMmStrategy[{}] session end with non-terminal closeout state={}, force-finalizing",
                        _deps.strategy_id,
                        static_cast<int>(cs_at_end));
        // 走 markCloseoutFailed:FLATTENING/RETRYING 都允许转 FAILED;TRIGGERED 不允许,
        // 但 TRIGGERED 在 session_end 出现属异常,仍走 force reset 兜底
        _deps.risk_monitor->markCloseoutFailed(now_ms);
        // 同步清守卫(防止 stale ids 跨 session 污染 on_order 路径)
        _closeout_pending_ids.clear();
        _closeout_hedge_pending = false;
        _closeout_hedge_wait_ticks = 0;
        _closeout_hedge_executed = false;
    }
}

void CloseoutOrchestrator::resetSession()
{
    RecursiveSpinGuard _g(_lock);
    _closeout_hedge_executed = false;
    _closeout_hedge_pending = false;
    _closeout_hedge_wait_ticks = 0;
    _closeout_pending_ids.clear();
    if (_deps.executor)
        _deps.executor->reset();
}

} // namespace futu
