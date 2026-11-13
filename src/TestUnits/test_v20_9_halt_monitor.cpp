/*!
 * \file test_v20_9_halt_monitor.cpp
 * \brief §20.9 批 2: halt 期硬闸监视子集回归测试 (审计 T1 风控盲区收窄)
 *
 * 背景: 原 checkRisk 在 isTradingHalted() 的 else 分支 (恢复被拒) 直接 return,
 * halt 期间持仓硬顶/日亏盯市完全不评估 —— 审计报告 T1 (④号②位风险)。
 * 批 2 在该分支插入 monitor-only 升级子集: 恶化到 IRREVERSIBLE 类别即升级
 * (升级链与正常 HALT 路径一致)。恢复求值与报价闸门语义不变 (19.16 原则③)。
 *
 * 驱动方式同 test_v10_r8a: 真 FutuRiskMonitor + 真 FutuPortfolio + 真 TradingState
 * + 真 QuotePolicyChain, 经 RiskCoordinator::checkRisk 推进。
 */
#include "../WtFutuCore/RiskCoordinator.h"
#include "../WtFutuCore/StrategyCoordinator.h" // TickContext 完整定义
#include "../WtFutuCore/FutuRiskMonitor.h"
#include "../WtFutuCore/FutuPortfolio.h"
#include "../WtFutuCore/TradingState.h"
#include "../WtFutuCore/QuotePolicyChain.h"
#include "gtest/gtest.h"

using namespace futu;

namespace
{

struct Rig
{
    FutuRiskMonitor monitor;
    FutuPortfolio portfolio;
    TradingState state;
    QuotePolicyChain chain;
    RiskCoordinator coord;

    Rig()
    {
        ApplicationDeps deps;
        deps.portfolio = &portfolio;
        deps.risk_monitor = &monitor;
        deps.trading_state = &state;
        deps.quote_chain = &chain;
        coord.setDeps(deps);

        // 极小日亏线便于触发: 浮亏超过 10 即 DAILY_LOSS CRITICAL → IRREVERSIBLE
        PortfolioParams p;
        p.max_loss = 10.0;
        portfolio.setParams(p);
    }

    TickContext tick(uint64_t ts_ms)
    {
        TickContext tc;
        tc.code = "SHFE.ao2609";
        tc.timestamp = ts_ms;
        tc.mid = 100.0;
        tc.bid_px = 99.9;
        tc.ask_px = 100.1;
        tc.tick_size = 0.2;
        return tc;
    }
};

} // namespace

// REVERSIBLE halt 期内日亏恶化 → 监视子集升级为 IRREVERSIBLE
TEST(FutuHaltPeriodMonitor, WorsenedDailyLossEscalatesToIrreversible)
{
    Rig rig;
    const uint64_t T0 = 1750700000000ULL;
    rig.monitor.setCurrentTime(T0);

    // 模拟可逆停机 (delta-rate 类): REVERSIBLE halt
    rig.monitor.haltTrading(RiskCategory::REVERSIBLE, 0.0);
    ASSERT_TRUE(rig.monitor.isTradingHalted());
    ASSERT_EQ(rig.monitor.getHaltCategory(), RiskCategory::REVERSIBLE);

    // halt 期内日亏恶化: 引擎 realized = -500 (远超 max_loss=10)。
    // 先 addContract 建合约簿记, 再 setShadowFromEngine 注入引擎口径盈亏
    // (shadow/updatePosition 对未知合约均静默跳过 —— 调试实证: 漏建合约则注入落空)
    rig.portfolio.addContract("SHFE.ao2609", 1.0, 0.2);
    rig.portfolio.setShadowFromEngine("SHFE.ao2609", 1.0, -500.0, 0.0);

    // checkRisk: cooldown 内恢复被拒 → else 分支 → 监视子集 → 升级
    rig.coord.checkRisk(nullptr, rig.tick(T0), false);
    EXPECT_EQ(rig.monitor.getHaltCategory(), RiskCategory::IRREVERSIBLE)
        << "halt 期日亏恶化必须经监视子集升级为 IRREVERSIBLE";
}

// REVERSIBLE halt 期内风险平稳 → 不升级 (维持可逆, 恢复语义不变)
TEST(FutuHaltPeriodMonitor, StableRiskDoesNotEscalate)
{
    Rig rig;
    const uint64_t T0 = 1750700000000ULL;
    rig.monitor.setCurrentTime(T0);

    rig.monitor.haltTrading(RiskCategory::REVERSIBLE, 0.0);
    ASSERT_EQ(rig.monitor.getHaltCategory(), RiskCategory::REVERSIBLE);

    // 无持仓无亏损, 风险平稳
    rig.coord.checkRisk(nullptr, rig.tick(T0), false);
    EXPECT_EQ(rig.monitor.getHaltCategory(), RiskCategory::REVERSIBLE)
        << "风险平稳时不得升级";
    EXPECT_TRUE(rig.monitor.isTradingHalted()) << "halt 状态本身不得被监视子集解除";
}

// IRREVERSIBLE halt 期内不再重复升级 (幂等, 避免 forceFlat 重复触发)
TEST(FutuHaltPeriodMonitor, AlreadyIrreversibleDoesNotReEscalate)
{
    Rig rig;
    const uint64_t T0 = 1750700000000ULL;
    rig.monitor.setCurrentTime(T0);

    rig.monitor.haltTrading(RiskCategory::IRREVERSIBLE, -500.0);
    ASSERT_EQ(rig.monitor.getHaltCategory(), RiskCategory::IRREVERSIBLE);

    rig.portfolio.addContract("SHFE.ao2609", 1.0, 0.2);
    rig.portfolio.setShadowFromEngine("SHFE.ao2609", 1.0, -500.0, 0.0);

    rig.coord.checkRisk(nullptr, rig.tick(T0), false);
    EXPECT_EQ(rig.monitor.getHaltCategory(), RiskCategory::IRREVERSIBLE);
}
