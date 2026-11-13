/*!
 * \file test_v20_8_latch_batch.cpp
 * \brief §20.8 批 B 闩锁健壮性 unit tests (2026-11-02)
 *
 * 覆盖:
 *   B1 ZombieLatchPolicy — 新段重置原点 / 同段续期保单调 / lift 清态 /
 *      capped·escalated 每段一次 (10-22 EXCEEDED 计时失真回归)
 *   B2 UnifiedOrderTracker 撤单重试指数退避 — 1s→2s→4s, 总预算 7s
 *      (10-22 09:02 开盘洪峰固定间隔误升 ZOMBIE 回归)
 *   B3 getZombieOrderIds — 长闩锁升级诊断的单号清单
 */
#include "../WtFutuCore/ZombieLatchPolicy.h"
#include "../WtFutuCore/UnifiedOrderTracker.h"
#include "gtest/gtest.h"

using namespace futu;

namespace
{
constexpr const char* CODE = "INE.ec.ec2611";
}

//==============================================================================
// B1: ZombieLatchPolicy
//==============================================================================

// 新闩锁段: 原点重置, held 从 0 起算 (§20.8-B1 核心回归 — 10-22 实证)
TEST(V20_8_LatchPolicy, FreshEpisodeHeldStartsFromZero)
{
    ZombieLatchPolicy p;
    p.arm(CODE, false, 100000); // 新段
    EXPECT_EQ(p.heldMs(CODE, 100000), 0u);
    EXPECT_EQ(p.heldMs(CODE, 100000 + 3000), 3000u);
}

// 同段续期 (was_halted=true): 保持单调原点, 总时长连续累计 (B1 语义, 2026-09-16)
TEST(V20_8_LatchPolicy, ContinuousEpisodeKeepsMonotonicOrigin)
{
    ZombieLatchPolicy p;
    p.arm(CODE, false, 100000);
    EXPECT_EQ(p.heldMs(CODE, 106000), 6000u);
    p.arm(CODE, true, 106000); // 同段续期 (升级去重后再次 zombie)
    EXPECT_EQ(p.heldMs(CODE, 112000), 12000u); // 不从 6000 重新起算
}

// 解除后重新置位 = 新段: 原点重置 (防继承陈旧起点瞬时误触发 EXCEEDED)
TEST(V20_8_LatchPolicy, LiftResetsOriginAndOneShotFlags)
{
    ZombieLatchPolicy p;
    p.arm(CODE, false, 100000);
    EXPECT_EQ(p.heldMs(CODE, 200000), 100000u);
    p.lift(CODE);
    EXPECT_EQ(p.heldMs(CODE, 300000), 0u); // 无记录回退 0

    p.arm(CODE, false, 300000); // 新段
    EXPECT_EQ(p.heldMs(CODE, 301000), 1000u); // 从 0 起算, 非 201000
}

// capped / escalated: 每闩锁段一次, lift 后重新武装
TEST(V20_8_LatchPolicy, CappedAndEscalatedOncePerEpisode)
{
    ZombieLatchPolicy p;
    p.arm(CODE, false, 100000);
    EXPECT_TRUE(p.markCapped(CODE));
    EXPECT_FALSE(p.markCapped(CODE));
    EXPECT_TRUE(p.markEscalated(CODE));
    EXPECT_FALSE(p.markEscalated(CODE));

    p.lift(CODE);
    p.arm(CODE, false, 200000); // 新段
    EXPECT_TRUE(p.markCapped(CODE));    // 重新武装
    EXPECT_TRUE(p.markEscalated(CODE)); // 重新武装
}

//==============================================================================
// B2: 撤单重试指数退避
//==============================================================================

// base=1000ms: 重试间隔 1s→2s→4s, 耗尽后再过 8s 置 zombie (总预算 7s)
TEST(V20_8_LatchPolicy, CancelRetryExponentialBackoff)
{
    UnifiedOrderTracker t;
    UnifiedTrackerConfig cfg;
    cfg.pending_cancel_timeout_ms = 1000; // 生产 cancelRetryIntervalMs
    cfg.cancel_max_retries = 3;
    t.setConfig(cfg);

    t.trackMMOrder(7, 0, CODE, 2410.0, 10, 2410.0, 1000, true);
    ASSERT_TRUE(t.tryMarkPendingCancel(7, CancelReason::MANUAL, 1000));

    // 第 1 次重试: +1000 (base<<0)
    EXPECT_TRUE(t.checkAutoCancel(CODE, 1999, 2410.0, 0.5, false).empty());
    const auto& a1 = t.checkAutoCancel(CODE, 2000, 2410.0, 0.5, false);
    ASSERT_EQ(a1.size(), 1u);
    EXPECT_EQ(a1[0].order_id, 7u);

    // 第 2 次重试: +2000 (base<<1), 提前 1ms 不得触发 (固定间隔旧实现此时已误重试)
    EXPECT_TRUE(t.checkAutoCancel(CODE, 3999, 2410.0, 0.5, false).empty());
    const auto& a2 = t.checkAutoCancel(CODE, 4000, 2410.0, 0.5, false);
    ASSERT_EQ(a2.size(), 1u);

    // 第 3 次重试: +4000 (base<<2)
    EXPECT_TRUE(t.checkAutoCancel(CODE, 7999, 2410.0, 0.5, false).empty());
    const auto& a3 = t.checkAutoCancel(CODE, 8000, 2410.0, 0.5, false);
    ASSERT_EQ(a3.size(), 1u);

    // 耗尽: 再过 8000 (base<<3) 置 zombie, 不再重试
    EXPECT_TRUE(t.checkAutoCancel(CODE, 15999, 2410.0, 0.5, false).empty());
    const auto& a4 = t.checkAutoCancel(CODE, 16000, 2410.0, 0.5, false);
    EXPECT_TRUE(a4.empty());

    UnifiedOrderInfo oi;
    ASSERT_TRUE(t.getOrderInfoCopy(7, oi));
    EXPECT_TRUE(oi.isZombie());
    EXPECT_EQ(oi.cancel_retry_count, 3u);
    ASSERT_EQ(t.getZombieEscalations().size(), 1u);
    EXPECT_EQ(t.getZombieEscalations()[0], CODE);
}

//==============================================================================
// B3: getZombieOrderIds — 长闩锁升级诊断
//==============================================================================

TEST(V20_8_LatchPolicy, ZombieOrderIdsForDiagnostics)
{
    UnifiedOrderTracker t;
    UnifiedTrackerConfig cfg;
    cfg.pending_cancel_timeout_ms = 300;
    cfg.cancel_max_retries = 3;
    t.setConfig(cfg);

    t.trackMMOrder(1, 0, CODE, 2410.0, 10, 2410.0, 1000, true);
    t.trackMMOrder(2, 0, CODE, 2410.0, 5, 2410.0, 1000, false);
    ASSERT_TRUE(t.tryMarkPendingCancel(1, CancelReason::MANUAL, 1000));
    ASSERT_TRUE(t.tryMarkPendingCancel(2, CancelReason::MANUAL, 1000));

    // 未 zombie 前: 清单为空
    EXPECT_TRUE(t.getZombieOrderIds(CODE).empty());

    for (uint64_t now : {1300u, 1900u, 3100u, 5500u})
        t.checkAutoCancel(CODE, now, 2410.0, 0.5, false);

    const auto ids = t.getZombieOrderIds(CODE);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_NE(std::find(ids.begin(), ids.end(), "1"), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), "2"), ids.end());
    // 其他合约: 空
    EXPECT_TRUE(t.getZombieOrderIds("SHFE.ao.ao2611").empty());
}
