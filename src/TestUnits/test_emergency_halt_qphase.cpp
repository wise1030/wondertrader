// §20.4 (2026-10-21): EMERGENCY halt 必须同步提升 TradingState.qphase
//
// 背景: 2026-10-21 09:01 实盘实证 —— arb 线程 handleRiskAlert → haltTrading(IRREVERSIBLE)
// 只置 RiskMonitor._trading_halted，从不提升 qphase；报价门禁读 TradingState.qphase，
// 导致 MM 报价照跑、checkRisk 因 halted 整体跳过（持仓/日亏硬闸全失效）。
// 修复契约: EMERGENCY 处置后 isTradingHalted()==true 且 canQuote()==false。

#include <gtest/gtest.h>

#include "../WtFutuCore/UftFutuMmStrategy.h"

using namespace futu;

// 修复主契约: EMERGENCY → 全局停机 + 报价门禁关闭
TEST(EmergencyHaltQphase, EmergencyPromotesQphase)
{
    UftFutuMmStrategy stra("test_ao");
    ASSERT_FALSE(stra.testIsTradingHalted());
    ASSERT_TRUE(stra.testCanQuote()); // 初始 qphase=NORMAL, 可报价

    stra.testInjectEmergencyAlert(159925.0, 100000.0);

    EXPECT_TRUE(stra.testIsTradingHalted());
    EXPECT_FALSE(stra.testCanQuote()); // §20.4 修复点: qphase 提升为 RISK_HALTED
}

// 幂等/重复注入: 第二次 EMERGENCY 不得崩溃、不得错误恢复
TEST(EmergencyHaltQphase, EmergencyIdempotent)
{
    UftFutuMmStrategy stra("test_ec");
    stra.testInjectEmergencyAlert(159925.0, 100000.0);
    ASSERT_TRUE(stra.testIsTradingHalted());
    ASSERT_FALSE(stra.testCanQuote());

    stra.testInjectEmergencyAlert(200000.0, 100000.0); // 重复注入

    EXPECT_TRUE(stra.testIsTradingHalted());
    EXPECT_FALSE(stra.testCanQuote());
}

// 独立实例隔离: 一个策略停机不影响另一个实例的判定（无共享静态状态回归）
TEST(EmergencyHaltQphase, InstancesAreIsolated)
{
    UftFutuMmStrategy halted("test_halted");
    UftFutuMmStrategy healthy("test_healthy");
    halted.testInjectEmergencyAlert(159925.0, 100000.0);

    EXPECT_TRUE(halted.testIsTradingHalted());
    EXPECT_FALSE(halted.testCanQuote());
    EXPECT_FALSE(healthy.testIsTradingHalted());
    EXPECT_TRUE(healthy.testCanQuote());
}
