/*!
 * \file CloseoutTrigger.h
 * \brief 收盘平仓触发器/状态机 (从 StrategyCoordinator::processCloseout 拆出, P1.3 Step 1)
 *
 * 职责: 决定"何时触发收盘平仓" + 驱动 CloseoutSub 子态流转
 *   (IDLE/TRIGGERED/DRAINING/ASSESSING/EXECUTING/RETRYING/COMPLETED/FAILED)。
 * 与 CloseoutOrchestrator (执行驱动, 已分离 C3) 互补: 本类只管触发/状态, 不管执行。
 * process() 返回 true = 本 tick 触发或处于 closeout; processTick 据此跳报价,
 * 并把结果喂给 CloseoutOrchestrator.onTick(ctx, tick, closeout_triggered)。
 *
 * 依赖: risk_monitor(子态/触发)、trading_state(enterCloseout)、portfolio(可用性闸)、
 *   cfg(closeout 时序参数)、cancel_all_quotes 回调(原 coordinator _quoters 循环 cancelAll,
 *   改回调以保持本类对 FutuQuoter 的单向依赖)。
 */
#pragma once
#include <functional>
namespace wtp { class IUftStraCtx; }
namespace futu {
class FutuRiskMonitor;
class TradingState;
class FutuPortfolio;
struct CoordinatorConfig;
struct TickContext;

class CloseoutTrigger
{
public:
    struct Deps
    {
        FutuRiskMonitor* risk_monitor = nullptr;
        TradingState* trading_state = nullptr;
        FutuPortfolio* portfolio = nullptr;
        const CoordinatorConfig* cfg = nullptr;
        /// 取消所有合约报价 (原 coordinator _quoters 循环 cancelAll)
        std::function<void(wtp::IUftStraCtx*)> cancel_all_quotes;
    };

    void setDeps(const Deps& deps) { _deps = deps; }

    /// 返回 true = 本 tick 触发/处于 closeout
    bool process(wtp::IUftStraCtx* ctx, TickContext& tc);

private:
    Deps _deps;
};
} // namespace futu
