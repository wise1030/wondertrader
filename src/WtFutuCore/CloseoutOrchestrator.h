/*!
 * \file CloseoutOrchestrator.h
 * \brief 收盘平仓编排器 (从 UftFutuMmStrategy 拆分的 closeout 驱动职责)
 *
 * 设计目的:
 *   集中管理收盘平仓的驱动逻辑:
 *     - 触发(延迟 N tick 撤单后启动执行器)
 *     - CloseoutExecutor 每 tick 驱动与状态同步(executor → RiskMonitor)
 *     - closeout 订单回报跟踪 (on_order)
 *     - session_end 强制收尾 / session_begin 复位
 *
 *   CloseoutExecutor 只负责执行(选价/批量/下单),
 *   FutuRiskMonitor 只负责状态机白名单校验,
 *   本类负责两者的衔接编排。
 */
#pragma once

#include <string>
#include <unordered_set>
#include <memory>
#include <cstdint>
#include "../Includes/FasterDefs.h"
#include "SpinLockGuard.h"

namespace wtp {
class IUftStraCtx;
class WTSTickData;
}

namespace futu {

class CloseoutExecutor;
class FutuRiskMonitor;
class FutuPortfolio;
class TradingState;
class FutuQuoter;
class OrderRouter;

class CloseoutOrchestrator
{
public:
    /// 依赖注入 (init 时设置一次, 所有指针生命周期由策略类保证)
    struct Deps {
        CloseoutExecutor* executor = nullptr;
        FutuRiskMonitor* risk_monitor = nullptr;
        FutuPortfolio* portfolio = nullptr;
        TradingState* trading_state = nullptr;
        OrderRouter* order_router = nullptr;
        wtp::wt_hashmap<std::string, std::unique_ptr<FutuQuoter>>* quoters = nullptr;
        const std::string* anchor_code = nullptr;
        uint32_t close_time = 150000;   ///< HHMMSS
        bool flatten_position = true;
        const char* strategy_id = "";
    };

    void setDeps(const Deps& deps) { _deps = deps; }

    /// 每 tick 驱动: 触发判断 → 延迟启动 → executor->run → 状态同步
    /// @param closeout_triggered 流水线 processCloseout 本 tick 是否触发了 closeout
    void onTick(wtp::IUftStraCtx* ctx, wtp::WTSTickData* tick, bool closeout_triggered);

    /// 一次性启动 CloseoutExecutor (原 executeCloseoutHedge)
    void executeHedge(wtp::IUftStraCtx* ctx);

    /// on_order 回报中的 closeout 订单跟踪
    /// @param now_ms epoch 毫秒
    void onOrderEvent(wtp::IUftStraCtx* ctx, uint32_t localid, const char* stdCode,
                      bool isCanceled, double leftQty, uint64_t now_ms);

    /// session_end 强制收尾 (非终态 → markCloseoutFailed + 清守卫)
    void finalizeAtSessionEnd(uint64_t now_ms);

    /// session_begin 复位
    void resetSession();

private:
    static constexpr uint32_t CLOSEOUT_HEDGE_WAIT_TICKS = 2;

    Deps _deps;

    // v7.6 阶段2: 递归自旋锁 — onTick(MdSpi) vs onOrderEvent(TdSpi)/
    //   finalizeAtSessionEnd/resetSession(RtTicker)
    mutable RecursiveSpinLock _lock;

    bool _closeout_hedge_executed = false;   ///< 本 session closeout 对冲是否已触发
    bool _closeout_hedge_pending = false;    ///< 是否正在等待延迟启动
    uint32_t _closeout_hedge_wait_ticks = 0; ///< 已等待 tick 数
    std::unordered_set<uint32_t> _closeout_pending_ids;  ///< closeout 对冲单 ID (兜底, 主识别走 OrderRouter)
    uint64_t _now_ms = 0;  ///< v7.1: replay 时钟 (onTick 注入; mark* 与 RiskMonitor._current_time 同基准, 0=回退墙钟)
};

} // namespace futu
