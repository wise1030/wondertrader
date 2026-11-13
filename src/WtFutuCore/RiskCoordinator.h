/*!
 * \file RiskCoordinator.h
 * \brief 风控协调器 (从 StrategyCoordinator 拆分, P1.3 Step 2a/2b)
 *
 * 职责: 承载从 coordinator 错位归属迁出的风控逻辑 (复核§3.2)。
 * Step 2a: checkTakerReduce (taker 减仓)。
 * Step 2b: checkRisk (风控中枢: HALT/PAUSE 相位切换 + forceFlat + arb 禁用;
 *          V10-R10 起 TOXICITY 相位翻转收编至 StrategyCoordinator)。
 */
#pragma once
#include "../WTSTools/WTSLogger.h"
#include "ApplicationDeps.h"  // 19.33: 统一聚合 (成员 _deps 为按值切片)
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <atomic>
#include <functional>
#include "RiskLiquidator.h"   // _liquidator 值成员 + clampReduceQty
#include "FutuRiskMonitor.h"  // FutuRiskMonitor / RiskViolation / CloseoutSub / RiskAction
namespace wtp { class IUftStraCtx; }
namespace futu {
struct TickContext;

class RiskCoordinator
{
public:
    /// F-core P1-6 阶段 1(2026-09-19): ORDER_RATE 持续违规窗口是否活跃 (影子对比用;
    ///   Md 单写者, 读侧一次 load, 无锁)
    bool isRateBreachActive() const { return _rate_breach_since_ms != 0; }

    // 19.33: 依赖统一走 ApplicationDeps 按值切片 (原嵌套 Deps 删除)
    void setDeps(const ApplicationDeps& deps)
    {
        _deps = deps;
        _deps.validate("RiskCoordinator");
        // 19.33-cleanup: cfg 消费者契约固化 (checkRisk 配置读取)
        if (!_deps.cfg)
            WTSLogger::error("[DEPS] RiskCoordinator: required dep 'cfg' is null");
        // 19.33: 内部 liquidator 随本 setDeps 一次性接线 (消费 order_router/portfolio 切片;
        // 原 checkRisk HALT 分支的按次 setDeps 已收编删除)
        _liquidator.setDeps(deps);
    }

    /// taker 减仓 (Step 2a). exchange_time_ms = replay 时钟 (0=回退墙钟).
    bool checkTakerReduce(wtp::IUftStraCtx* ctx, uint64_t exchange_time_ms);

    /// 风控中枢检查 (Step 2b). in_cooloff = 调用点预计算的毒性暂停态 (含 resume-hold;
    /// V10-R10 起 TOXICITY 相位翻转已收编至 StrategyCoordinator, 本处仅消费该态做
    /// 自成交校准衰减)。
    bool checkRisk(wtp::IUftStraCtx* ctx, const TickContext& tc, bool in_cooloff);

private:
    /// §20.9 批 2(2026-11-12): halt 期硬闸监视子集 —— 恢复被拒/仍 halt 的 else 分支内
    ///   调用。持仓硬顶/日亏盯市在 halt 期继续评估, 恶化到 IRREVERSIBLE 类别即升级
    ///   (升级链与正常 HALT 路径一致)。报价闸门与恢复求值照旧跳过 (19.16 原则③不变);
    ///   closeout 窗口由入口提前 return 覆盖 (平仓期持仓有意非零, 不监视)。
    void _monitorHardGatesWhileHalted(wtp::IUftStraCtx* ctx, const TickContext& tc);

private:
    ApplicationDeps _deps;  // 19.33: 按值切片 (消费点 _deps.xxx 零改动)
    std::unordered_map<std::string, uint64_t> _last_taker_reduce;   // 2a: taker 限频
    RiskLiquidator _liquidator;                  // 2b: 统一强平原语 (从 coordinator 迁入)
    uint64_t _last_halt_log_ms = 0;              // 2b: halted 日志限频 (从 coordinator 迁入)
    std::vector<RiskViolation> _violations_buf;  // 2b: 风控违规复用缓冲 (从 coordinator 迁入)
    uint64_t _rate_breach_since_ms = 0;          // V10-R8-a: ORDER_RATE 持续违规起点 (0=无违规; Md 单写者)
};
} // namespace futu
