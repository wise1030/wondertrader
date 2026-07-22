/*!
 * \file CloseoutOrchestrator.cpp
 * \brief 收盘平仓编排器实现 (自 UftFutuMmStrategy 搬移, 逻辑零修改)
 */
#include "CloseoutOrchestrator.h"
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

namespace futu {

void CloseoutOrchestrator::onTick(wtp::IUftStraCtx* ctx, wtp::WTSTickData* tick, bool closeout_triggered)
{
    // === Closeout hedge trigger ===
    if (closeout_triggered && _deps.flatten_position
        && _deps.risk_monitor->isCloseoutFlattening() && !_closeout_hedge_executed)
    {
        _deps.trading_state->enterCloseout();
        if (_deps.quoters) {
            for (auto& [code, quoter] : *_deps.quoters) {
                if (quoter) quoter->cancelAll(ctx);
            }
        }
        _closeout_hedge_pending = true;
        _closeout_hedge_wait_ticks = 0;
        _closeout_hedge_executed = true;
        WTSLogger::warn("UftFutuMmStrategy[{}] CLOSEOUT: halted + cancelAll, hedge deferred {} ticks",
                        _deps.strategy_id, CLOSEOUT_HEDGE_WAIT_TICKS);
    }

    // Deferred CloseoutExecutor start
    if (_closeout_hedge_pending)
    {
        _closeout_hedge_wait_ticks++;
        if (_closeout_hedge_wait_ticks >= CLOSEOUT_HEDGE_WAIT_TICKS)
        {
            WTSLogger::warn("UftFutuMmStrategy[{}] CLOSEOUT: starting CloseoutExecutor after {} ticks",
                            _deps.strategy_id, _closeout_hedge_wait_ticks);
            executeHedge(ctx);
            _closeout_hedge_pending = false;
            _closeout_hedge_wait_ticks = 0;
        }
    }

    // Run CloseoutExecutor every tick if active
    if (_deps.executor && _deps.executor->isActive())
    {
        const ContractState* anchorState = _deps.portfolio->getContract(*_deps.anchor_code);
        MarketSnapshot snap;
        snap.bid1       = anchorState ? anchorState->bid1 : 0;
        snap.ask1       = anchorState ? anchorState->ask1 : 0;
        snap.bid1_qty   = tick->bidqty(0);
        snap.ask1_qty   = tick->askqty(0);
        snap.price_tick = anchorState ? anchorState->tick_size : 0;
        snap.upper_limit = tick->upperlimit();
        snap.lower_limit = tick->lowerlimit();
        {
            uint32_t at = tick->actiontime();
            uint32_t hh = at / 10000000;
            uint32_t mm = (at / 100000) % 100;
            uint32_t ss = (at / 1000) % 100;
            uint32_t mmm = at % 1000;
            snap.timestamp_ms = static_cast<uint64_t>(hh) * 3600000ULL
                              + static_cast<uint64_t>(mm) * 60000ULL
                              + static_cast<uint64_t>(ss) * 1000ULL
                              + mmm;
        }
        _deps.executor->run(ctx, snap);

        if (_deps.executor->isCompleted())
        {
            if (_deps.risk_monitor->getCloseoutSub() != CloseoutSub::COMPLETED)
            {
                _deps.risk_monitor->markCloseoutCompleted(TimeUtils::getLocalTimeNow());
            }
        }
        else if (_deps.executor->isFailed())
        {
            _deps.risk_monitor->markCloseoutFailed(TimeUtils::getLocalTimeNow());
        }
    }

    // Reset hedge flags on terminal/transient closeout states
    auto cs = _deps.risk_monitor->getCloseoutSub();
    if (cs == CloseoutSub::IDLE || cs == CloseoutSub::FAILED
        || cs == CloseoutSub::RETRYING)
    {
        _closeout_hedge_executed = false;
        _closeout_hedge_pending = false;
        _closeout_hedge_wait_ticks = 0;
        if (_deps.executor && !_deps.executor->isCompleted())
            _deps.executor->reset();
    }
}

void CloseoutOrchestrator::executeHedge(wtp::IUftStraCtx* ctx)
{
//============================================================
// CloseoutExecutor 启动入口
// 实际执行逻辑在 CloseoutExecutor::run() 中，由 onTick 每 tick 调用。
// 本函数只负责一次性的 start()：计算 close_time_ms，启动执行器。
//============================================================

if (!_deps.executor)
{
    WTSLogger::error("UftFutuMmStrategy[{}] CloseoutExecutor is null!", _deps.strategy_id);
    return;
}

// 已经在运行（retry 重入场景），不重复 start
if (!_deps.executor->isIdle())
return;

// P1-2: closeout 决策前从策略引擎同步持仓
for (const auto& c : _deps.portfolio->getAllContracts())
{
    double actual = ctx->stra_get_local_position(c.code.c_str());
    if (std::abs(c.position - actual) > 0.01)
    {
        WTSLogger::info("UftFutuMmStrategy[{}] Portfolio sync before closeout: {} {:.0f}->{:.0f}",
                        _deps.strategy_id, c.code, c.position, actual);
        _deps.portfolio->onPositionUpdate(c.code.c_str(), actual);
    }
}

double totalDelta = _deps.portfolio->getNetDelta();

if (std::abs(totalDelta) < 0.01)
{
    WTSLogger::info("UftFutuMmStrategy[{}] Closeout: No position to hedge (Delta=0)", _deps.strategy_id);
    if (_deps.risk_monitor->getCloseoutSub() != CloseoutSub::COMPLETED)
    {
        _deps.risk_monitor->markCloseoutCompleted(TimeUtils::getLocalTimeNow());
    }
    return;
}

// 获取锚定合约信息
const ContractState* anchorState = _deps.portfolio->getContract(*_deps.anchor_code);
if (!anchorState)
{
    WTSLogger::error("UftFutuMmStrategy[{}] Closeout failed: anchor contract {} not found",
                     _deps.strategy_id, *_deps.anchor_code);
    return;
}

double hedgeRatio = anchorState->hedge_ratio;
if (hedgeRatio <= 0)
{
    WTSLogger::error("UftFutuMmStrategy[{}] Closeout failed: invalid hedgeRatio={}",
                     _deps.strategy_id, hedgeRatio);
    return;
}

// 计算 close_time_ms (ms-from-midnight from HHMMSS config)
uint32_t close_hhmmss = _deps.close_time;  // e.g. 150000
uint32_t hh = close_hhmmss / 10000;
uint32_t mm = (close_hhmmss / 100) % 100;
uint32_t ss = close_hhmmss % 100;
uint64_t close_time_ms = static_cast<uint64_t>(hh) * 3600000ULL
                       + static_cast<uint64_t>(mm) * 60000ULL
                       + static_cast<uint64_t>(ss) * 1000ULL;

// 启动 CloseoutExecutor
WTSLogger::warn("UftFutuMmStrategy[{}] CLOSEOUT: starting CloseoutExecutor "
                "(code={}, delta={:.2f}, hedge_ratio={:.2f}, close_ms={})",
                _deps.strategy_id, *_deps.anchor_code, totalDelta, hedgeRatio, close_time_ms);

_deps.executor->start(ctx, _deps.anchor_code->c_str(),
                       close_time_ms, hedgeRatio);

// 标记 FLATTENING (executor 已启动，等待渐进成交)
// 时间戳统一 epoch ms (搬移时修复: 旧代码在此用了压缩时间戳)
_deps.risk_monitor->markCloseoutDraining(TimeUtils::getLocalTimeNow());
}

void CloseoutOrchestrator::onOrderEvent(wtp::IUftStraCtx* ctx, uint32_t localid, const char* stdCode,
                                         bool isCanceled, double leftQty, uint64_t now_ms)
{
    // 判定 closeout 单: OrderRouter 来源标记(主) + pending_ids 兜底
    bool is_closeout_order = (_closeout_pending_ids.find(localid) != _closeout_pending_ids.end())
        || (_deps.order_router && _deps.order_router->isOrderFromSource(localid, Source::CLOSEOUT));

    if (!_deps.risk_monitor || !_deps.risk_monitor->isCloseoutFlattening() || !is_closeout_order)
        return;

    if (isCanceled)
    {
        // Closeout order was rejected/canceled — mark FAILED to trigger retry
        WTSLogger::warn("[CLOSEOUT] Order canceled/rejected during flattening: code={} localid={}, marking FAILED for retry",
            stdCode, localid);
        _closeout_pending_ids.erase(localid);
        _deps.risk_monitor->markCloseoutFailed(now_ms);
    }
    else if (leftQty == 0)
    {
        // Order fully filled — check if position is now flat
        _closeout_pending_ids.erase(localid);
        double totalDelta = _deps.portfolio->getTotalDelta();
        if (std::abs(totalDelta) < 0.01)
        {
            WTSLogger::info("[CLOSEOUT] All positions flattened, marking COMPLETED");
            _deps.risk_monitor->markCloseoutCompleted(now_ms);
        }
        // else: still have positions, wait for more fills or next tick
    }
}

void CloseoutOrchestrator::finalizeAtSessionEnd(uint64_t now_ms)
{
    // session_end closeout 状态强制收尾
    // 根因:closeout FLATTENING → COMPLETED 仅在 on_order 回调 + getTotalDelta()<0.01
    // 这一条路径上转移。若 hedge 单未全成、或成交后 Delta 因取整/口径残留(如 1 手)
    // 不到阈值,state 卡 FLATTENING。session 结束是硬边界:此后不可能再有 tick/order
    // 推动状态,必须当场强制收尾,避免下一日 resetCloseout 被状态机拒绝(canTransitionTo
    // 仅允许 COMPLETED→IDLE)导致 state 永久死锁。
    auto cs_at_end = _deps.risk_monitor->getCloseoutSub();
    if (cs_at_end != CloseoutSub::IDLE && cs_at_end != CloseoutSub::COMPLETED)
    {
        WTSLogger::warn("UftFutuMmStrategy[{}] session end with non-terminal closeout state={}, force-finalizing",
            _deps.strategy_id, static_cast<int>(cs_at_end));
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
    _closeout_hedge_executed = false;
    _closeout_hedge_pending = false;
    _closeout_hedge_wait_ticks = 0;
    _closeout_pending_ids.clear();
    if (_deps.executor)
        _deps.executor->reset();
}

} // namespace futu
