/*!
 * \file CloseoutOrchestrator.h
 * \brief 收盘平仓编排器 (触发决策 + 执行驱动, 19.34 合并原 CloseoutTrigger)
 *
 * 设计目的:
 *   集中管理收盘平仓全链路:
 *     - 触发决策/状态机解释 (原 CloseoutTrigger::process 并入, 19.34:
 *       Trigger 无自身状态(状态权威=FutuRiskMonitor::CloseoutSub), 独立成类
 *       只剩碎片成本; 合并消掉 closeout_triggered 桥接传参)
 *     - 触发(延迟 N tick 撤单后启动执行器)
 *     - CloseoutExecutor 每 tick 驱动与状态同步(executor → RiskMonitor)
 *     - closeout 订单回报跟踪 (on_order)
 *     - session_end 强制收尾 / session_begin 复位
 *
 *   CloseoutExecutor 只负责执行(选价/批量/下单),
 *   FutuRiskMonitor 只负责状态机白名单校验,
 *   本类负责触发决策 + 两者的衔接编排。
 */
#pragma once

#include "ApplicationDeps.h"  // 19.33: 统一聚合 (成员 _deps 为按值切片)
#include <string>
#include <unordered_set>
#include <memory>
#include <cstdint>
#include "../Includes/FasterDefs.h"
#include "SpinLockGuard.h"
#include "../WTSTools/WTSLogger.h"

namespace wtp
{
class IUftStraCtx;
class WTSTickData;
} // namespace wtp

namespace futu
{

class CloseoutExecutor;
class FutuRiskMonitor;
class OrderRouter;
struct TickContext;  // 19.34: processTrigger 参数 (前向声明)

class CloseoutOrchestrator
{
public:
    /// 依赖注入 (init 时设置一次, 所有指针生命周期由策略类保证)

    // 19.33: 依赖统一走 ApplicationDeps 按值切片 (原嵌套 Deps 删除)
    void setDeps(const ApplicationDeps& deps)
    {
        _deps = deps;
        _deps.validate("CloseoutOrchestrator");
        // 19.34: 触发决策链消费 cfg (checkCloseout 时序/flatten 开关), 契约固化
        if (!_deps.cfg)
            WTSLogger::error("[DEPS] CloseoutOrchestrator: required dep 'cfg' is null");
    }

    //==========================================================================
    // 触发决策/状态机解释 (19.34 自 CloseoutTrigger 并入, 行为逐行保持)
    //==========================================================================

    /// Stage 0 触发判断: 返回 true = 本 tick 处于/进入 closeout (调用方跳报价,
    /// 活跃平仓态下仍跑硬风控)。结果缓存于 _trigger_active 供 onTick 消费。
    bool processTrigger(wtp::IUftStraCtx* ctx, TickContext& tc);

private:
    /// 触发状态机实现 (持锁; processTrigger 为锁包装 + 触发态缓存)
    bool processTriggerImpl(wtp::IUftStraCtx* ctx, TickContext& tc);

public:
    /// 每 tick 驱动: 延迟启动 → executor->run → 状态同步
    /// (触发态改经 _trigger_active 成员, 19.34 消掉桥接参数)
    /// 19.35: now_ms 由 on_tick 主入口单一转换后经参数下发 (不再自转换)
    void onTick(wtp::IUftStraCtx* ctx, wtp::WTSTickData* tick, uint64_t now_ms);

    /// 一次性启动 CloseoutExecutor (原 executeCloseoutHedge)
    void executeHedge(wtp::IUftStraCtx* ctx);

    /// on_order 回报中的 closeout 订单跟踪
    /// @param now_ms epoch 毫秒
    void onOrderEvent(
        wtp::IUftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isCanceled, double leftQty, uint64_t now_ms);

    /// session_end 强制收尾 (非终态 → markCloseoutFailed + 清守卫)
    void finalizeAtSessionEnd(uint64_t now_ms);

    /// session_begin 复位
    void resetSession();

private:
    static constexpr uint32_t CLOSEOUT_HEDGE_WAIT_TICKS = 2;

    ApplicationDeps _deps;  // 19.33: 按值切片 (消费点 _deps.xxx 零改动)

    // v7.6 阶段2: 递归自旋锁 — onTick(MdSpi) vs onOrderEvent(TdSpi)/
    //   finalizeAtSessionEnd/resetSession(RtTicker)
    mutable RecursiveSpinLock _lock;

    bool _closeout_hedge_executed = false;              ///< 本 session closeout 对冲是否已触发
    /// 19.34: 本 tick 触发态 (processTrigger 写入, onTick 同线程消费, 消桥接参数)
    bool _trigger_active = false;
    bool _closeout_hedge_pending = false;               ///< 是否正在等待延迟启动
    uint32_t _closeout_hedge_wait_ticks = 0;            ///< 已等待 tick 数
    std::unordered_set<uint32_t> _closeout_pending_ids; ///< closeout 对冲单 ID (兜底, 主识别走 OrderRouter)
    uint64_t _now_ms = 0; ///< v7.1: replay 时钟 (onTick 注入; mark* 与 RiskMonitor._current_time 同基准, 0=回退墙钟)
};

} // namespace futu
