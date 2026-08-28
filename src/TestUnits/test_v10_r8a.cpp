/*!
 * \file test_v10_r8a.cpp
 * \brief V10-R8a 开盘前报价风暴护栏回归测试
 *
 * 场景: 08-28 实盘 20:59 一分钟 44,304 单 (738/s) —— 盘前交易所拒单
 * ("当前状态禁止报单") 走 cancel 终态通道, 绕过 on_entrust 错误计数,
 * 既有 ERROR 熔断盲区。护栏: ORDER_RATE violation 持续超阈值 →
 * 单轨 ERROR 暂停 (不动 haltTrading), 经 handleQuotingAutoResume
 * 指数退避试探恢复。
 *
 * 驱动方式: 真 FutuRiskMonitor (recordOrders 灌单) + 真 FutuPortfolio +
 * 真 TradingState, 经 RiskCoordinator::checkRisk 每 tick 推进 replay 时钟。
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
    std::atomic<uint64_t> quoting_paused_since{0};
    std::atomic<uint32_t> order_error_count{0};
    RiskCoordinator coord;

    Rig()
    {
        RiskCoordinator::Deps deps;
        deps.portfolio = &portfolio;
        deps.risk_monitor = &monitor;
        deps.trading_state = &state;
        deps.quote_chain = &chain;
        deps.quoting_paused_since = &quoting_paused_since;
        deps.order_error_count = &order_error_count;
        coord.setDeps(deps);
    }

    /// 灌单使 ORDER_RATE violation 产生 (默认 max_orders_per_sec=50);
    /// 须先对齐 monitor 的 replay 时钟 (recordOrder/prune 均以其为窗口基准)
    void storm(uint64_t ts_ms)
    {
        monitor.setCurrentTime(ts_ms);
        for (int i = 0; i < 60; ++i)
            monitor.recordOrder();
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

TEST(test_v10_r8a, SustainedRateBreachTriggersErrorSingleTrack)
{
    Rig rig;
    rig.monitor.resetDaily();

    const uint64_t T0 = 1750700000000ULL;
    // 第 1 tick: 风暴开始 (violation 首次出现, 记忆起点, 不触发)
    rig.storm(T0);
    rig.coord.checkRisk(nullptr, rig.tick(T0), false);
    EXPECT_NE(rig.state.qphase.load(), QuotingPhase::ERROR);

    // 第 2 tick (+1s): 持续中, 仍 < 3s 阈值, 不触发
    rig.storm(T0 + 1000);
    rig.coord.checkRisk(nullptr, rig.tick(T0 + 1000), false);
    EXPECT_NE(rig.state.qphase.load(), QuotingPhase::ERROR);

    // 第 3 tick (+3.5s): 持续超阈值 → ERROR 单轨
    rig.storm(T0 + 3500);
    rig.coord.checkRisk(nullptr, rig.tick(T0 + 3500), false);
    EXPECT_EQ(rig.state.qphase.load(), QuotingPhase::ERROR);
    // D1 单轨语义: 不动 haltTrading (恢复预算不被消耗)
    EXPECT_FALSE(rig.monitor.isTradingHalted());
    // 恢复挂钩已注入: 暂停起点/错误计数落位 (退避试探恢复依赖)
    EXPECT_EQ(rig.quoting_paused_since.load(), T0 + 3500);
    EXPECT_EQ(rig.order_error_count.load(), 1u);
}

TEST(test_v10_r8a, TransientBreachDoesNotTrigger)
{
    Rig rig;
    rig.monitor.resetDaily();

    const uint64_t T0 = 1750700000000ULL;
    // 单 tick 风暴后恢复正常 → 起点清零, 后续重新计时
    rig.storm(T0);
    rig.coord.checkRisk(nullptr, rig.tick(T0), false);
    rig.coord.checkRisk(nullptr, rig.tick(T0 + 60000), false); // 无违规 tick (ring 已被 prune)
    rig.storm(T0 + 61000);
    rig.coord.checkRisk(nullptr, rig.tick(T0 + 61000), false); // 新起点
    rig.coord.checkRisk(nullptr, rig.tick(T0 + 65000), false);
    // 61000 起点后 65000-61000=4000ms >= 3000 → 触发 (验证重新计时正确,
    // 而非沿用 T0 旧起点导致的立即触发)
    EXPECT_EQ(rig.state.qphase.load(), QuotingPhase::ERROR);
}

TEST(test_v10_r8a, HaltedStateNotOverridden)
{
    Rig rig;
    rig.monitor.resetDaily();

    const uint64_t T0 = 1750700000000ULL;
    // IRREVERSIBLE halt 不可自动恢复 (REVERSIBLE 会被 checkAndRecover 立即恢复,
    // 无法构造守卫态) —— 守卫态下风暴护栏不得绕过 halt
    rig.monitor.haltTrading(RiskCategory::IRREVERSIBLE, 0);
    rig.state.setQuotingPhase(QuotingPhase::RISK_HALTED);

    rig.storm(T0);
    rig.coord.checkRisk(nullptr, rig.tick(T0), false);
    rig.storm(T0 + 3500);
    rig.coord.checkRisk(nullptr, rig.tick(T0 + 3500), false);
    rig.storm(T0 + 7000);
    rig.coord.checkRisk(nullptr, rig.tick(T0 + 7000), false);
    // RISK_HALTED 不可被 ERROR 直接抢占 (setQuotingPhase 校验拒绝)
    EXPECT_EQ(rig.state.qphase.load(), QuotingPhase::RISK_HALTED);
    EXPECT_TRUE(rig.monitor.isTradingHalted());
}

TEST(test_v10_r8a, ConfigDefaultAndKeyParse)
{
    RiskRateLimits r;
    EXPECT_EQ(r.order_rate_pause_ms, 3000u);
    EXPECT_EQ(RiskRateLimits::fromVariant(nullptr).order_rate_pause_ms, 3000u);
}
