/*!
 * \file CloseoutTrigger.cpp
 * \brief 收盘平仓触发器/状态机实现 (从 StrategyCoordinator::processCloseout 迁移, P1.3 Step 1)
 *
 * 纯迁移: 行为与原 processCloseout 完全一致, 仅 _quoters 循环 cancelAll 改为 cancel_all_quotes 回调。
 */
#include "CloseoutTrigger.h"
#include "StrategyCoordinator.h"   // TickContext / CoordinatorConfig / CloseoutSub (via FutuRiskMonitor.h)
#include "../WTSTools/WTSLogger.h"

namespace futu {

bool CloseoutTrigger::process(wtp::IUftStraCtx* ctx, TickContext& tc)
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

} // namespace futu
