/*!
 * \file RiskCoordinator.h
 * \brief 风控协调器 (从 StrategyCoordinator 拆分, P1.3 Step 2a/2b)
 *
 * 职责: 承载从 coordinator 错位归属迁出的风控逻辑 (复核§3.2)。
 * Step 2a: checkTakerReduce (taker 减仓)。
 * Step 2b: checkRisk (风控中枢: HALT/PAUSE/TOXICITY 相位切换 + forceFlat + arb 禁用)。
 */
#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <functional>
#include "RiskLiquidator.h"   // _liquidator 值成员 + clampReduceQty
#include "FutuRiskMonitor.h"  // FutuRiskMonitor / RiskViolation / CloseoutSub / RiskAction
namespace wtp { class IUftStraCtx; }
namespace futu {
class TradingState;
class AsyncArbitrageExecutor;
class QuotePolicyChain;
class SelfTradeCalibrator;
class FutuPortfolio;
class OrderRouter;
struct CoordinatorConfig;
struct TickContext;

class RiskCoordinator
{
public:
    struct Deps
    {
        FutuPortfolio* portfolio = nullptr;
        OrderRouter* order_router = nullptr;
        FutuRiskMonitor* risk_monitor = nullptr;
        TradingState* trading_state = nullptr;
        AsyncArbitrageExecutor* arb_executor = nullptr;        // nullable (null-guard)
        QuotePolicyChain* quote_chain = nullptr;               // shared w/ coordinator processQuoting
        SelfTradeCalibrator* self_trade_calibrator = nullptr;
        const CoordinatorConfig* cfg = nullptr;
        std::function<void(wtp::IUftStraCtx*)> cancel_all_quotes; // HALT 撤所有做市单
    };
    void setDeps(const Deps& deps) { _deps = deps; }

    /// taker 减仓 (Step 2a). exchange_time_ms = replay 时钟 (0=回退墙钟).
    bool checkTakerReduce(wtp::IUftStraCtx* ctx, uint64_t exchange_time_ms);

    /// 风控中枢检查 (Step 2b). in_cooloff = 调用点预计算的 toxicity cooloff 状态.
    bool checkRisk(wtp::IUftStraCtx* ctx, const TickContext& tc, bool in_cooloff);

private:
    Deps _deps;
    std::unordered_map<std::string, uint64_t> _last_taker_reduce;   // 2a: taker 限频
    RiskLiquidator _liquidator;                  // 2b: 统一强平原语 (从 coordinator 迁入)
    uint64_t _last_halt_log_ms = 0;              // 2b: halted 日志限频 (从 coordinator 迁入)
    std::vector<RiskViolation> _violations_buf;  // 2b: 风控违规复用缓冲 (从 coordinator 迁入)
};
} // namespace futu
