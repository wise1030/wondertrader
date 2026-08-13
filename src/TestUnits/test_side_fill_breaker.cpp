/*!
 * \file test_side_fill_breaker.cpp
 * \brief SideFillBreaker (同侧连续成交熔断器) unit tests
 *
 * Verifies the per-contract side fill breaker state machine:
 *   - same-side consecutive fills trigger the pause at threshold
 *   - different contracts maintain independent counters (ao2609 vs ao2610)
 *   - opposite-side fill resets the same-side streak
 *   - window expiry resets the counter
 *   - pause auto-expires and does not accumulate during pause
 */
#include "../WtFutuCore/SideFillBreaker.h"
#include "gtest/gtest/gtest.h"

using namespace futu;

namespace
{

SideFillBreakerConfig makeCfg(uint32_t threshold = 3, uint32_t window_ms = 3000, uint32_t pause_ms = 5000)
{
    SideFillBreakerConfig cfg;
    cfg.max_consecutive_same_side = threshold;
    cfg.window_ms = window_ms;
    cfg.pause_ms = pause_ms;
    return cfg;
}

} // namespace

TEST(SideFillBreakerTest, TriggersAtThreshold)
{
    SideFillBreaker b(makeCfg());
    const uint64_t t0 = 1000000;
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0));
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0 + 300));
    EXPECT_TRUE(b.onFill("SHFE.ao2610", true, t0 + 600)); // 第 3 笔触发
    EXPECT_TRUE(b.isPaused("SHFE.ao2610", t0 + 700));
}

TEST(SideFillBreakerTest, PerContractIsolation)
{
    SideFillBreaker b(makeCfg());
    const uint64_t t0 = 1000000;
    // ao2609 的成交不影响 ao2610 计数（按合约独立维护）
    EXPECT_FALSE(b.onFill("SHFE.ao2609", true, t0));        // ao2609 buy 1
    EXPECT_FALSE(b.onFill("SHFE.ao2609", true, t0 + 100));  // ao2609 buy 2
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0 + 200));  // ao2610 buy 1
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0 + 300));  // ao2610 buy 2
    EXPECT_TRUE(b.onFill("SHFE.ao2610", true, t0 + 400));   // ao2610 buy 3 -> 触发 ao2610
    EXPECT_TRUE(b.isPaused("SHFE.ao2610", t0 + 500));
    EXPECT_FALSE(b.isPaused("SHFE.ao2609", t0 + 500)); // ao2609 未被暂停
    // ao2609 独立计数: 补到第 3 笔才触发自身熔断
    EXPECT_TRUE(b.onFill("SHFE.ao2609", true, t0 + 600));   // ao2609 buy 3 -> 触发 ao2609
    EXPECT_TRUE(b.isPaused("SHFE.ao2609", t0 + 700));
}

TEST(SideFillBreakerTest, OppositeSideResetsStreak)
{
    SideFillBreaker b(makeCfg());
    const uint64_t t0 = 1000000;
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0));        // buy 1
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0 + 100));  // buy 2
    EXPECT_FALSE(b.onFill("SHFE.ao2610", false, t0 + 200)); // sell 打断买单序列
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0 + 300));  // buy 1 (重新计数)
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0 + 400));  // buy 2
    EXPECT_TRUE(b.onFill("SHFE.ao2610", true, t0 + 500));   // buy 3 -> 触发
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0 + 600));  // 暂停期内不累计
}

TEST(SideFillBreakerTest, WindowExpiryResets)
{
    SideFillBreaker b(makeCfg(3, 1000, 5000));
    const uint64_t t0 = 1000000;
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0));
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0 + 100));
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0 + 2000)); // 窗口已过期 -> 重新计数
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0 + 2100));
    EXPECT_TRUE(b.onFill("SHFE.ao2610", true, t0 + 2200));
}

TEST(SideFillBreakerTest, PauseExpiresAndDoesNotAccumulate)
{
    SideFillBreaker b(makeCfg(3, 3000, 5000));
    const uint64_t t0 = 1000000;
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0));
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0 + 100));
    EXPECT_TRUE(b.onFill("SHFE.ao2610", true, t0 + 200)); // 触发
    EXPECT_TRUE(b.isPaused("SHFE.ao2610", t0 + 300));
    // 暂停期内成交不累计、不重复触发
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0 + 400));
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0 + 500));
    // 暂停到期自动恢复
    const uint64_t t_after = t0 + 200 + 5000 + 100;
    EXPECT_FALSE(b.isPaused("SHFE.ao2610", t_after));
    // 恢复后重新计数
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t_after));
    EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t_after + 100));
    EXPECT_TRUE(b.onFill("SHFE.ao2610", true, t_after + 200));
}

TEST(SideFillBreakerTest, DisabledWhenThresholdZero)
{
    SideFillBreaker b(makeCfg(0, 3000, 5000));
    const uint64_t t0 = 1000000;
    for (int i = 0; i < 10; ++i)
        EXPECT_FALSE(b.onFill("SHFE.ao2610", true, t0 + i * 100));
    EXPECT_FALSE(b.isPaused("SHFE.ao2610", t0 + 1000));
}
