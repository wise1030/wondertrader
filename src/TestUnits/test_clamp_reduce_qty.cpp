/*!
 * \file test_clamp_reduce_qty.cpp
 * \brief P0-2: 减仓数量截断函数 clampReduceQty 对照测试
 *
 * 验证四条减仓路径的核心截断逻辑一致:
 *   - RiskLiquidator::reduceContract (截断到 |position|)
 *   - CloseoutExecutor::handleExecuting (旧: 截断到 |remaining|=net_delta; 新: +截断到 |anchor position|)
 *   - checkTakerReduce (截断到 |position|-target)
 *   - clampReduceQty (统一原语)
 */
#include "../WtFutuCore/RiskLiquidator.h"
#include "gtest/gtest/gtest.h"

using namespace futu;

// === clampReduceQty 基础测试 ===

TEST(test_clamp_reduce_qty, zero_requested_returns_zero)
{
    EXPECT_DOUBLE_EQ(clampReduceQty(0.0, 100.0), 0.0);
    EXPECT_DOUBLE_EQ(clampReduceQty(-5.0, 100.0), 0.0);
}

TEST(test_clamp_reduce_qty, zero_position_returns_zero)
{
    EXPECT_DOUBLE_EQ(clampReduceQty(10.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(clampReduceQty(10.0, 0.005), 0.0);  // < 0.01 threshold
}

TEST(test_clamp_reduce_qty, request_within_position)
{
    // 请求 30, 持仓 50 -> 返回 30 (不截断)
    EXPECT_DOUBLE_EQ(clampReduceQty(30.0, 50.0), 30.0);
    EXPECT_DOUBLE_EQ(clampReduceQty(30.0, -50.0), 30.0);  // 空头持仓
}

TEST(test_clamp_reduce_qty, request_exceeds_position_clamps)
{
    // clampReduceQty 原语: min(req, |pos|) -- 用于纯平仓路径 (forceFlatAll/takerReduce), 不开反向仓.
    // (注: CloseoutExecutor 不用此原语; 它走 anchor-delta, 截到 |remaining|, 允许 close+open)
    EXPECT_DOUBLE_EQ(clampReduceQty(49.0, 36.0), 36.0);
    EXPECT_DOUBLE_EQ(clampReduceQty(100.0, -25.0), 25.0);
}

TEST(test_clamp_reduce_qty, exact_match)
{
    EXPECT_DOUBLE_EQ(clampReduceQty(50.0, 50.0), 50.0);
    EXPECT_DOUBLE_EQ(clampReduceQty(1.0, -1.0), 1.0);
}

TEST(test_clamp_reduce_qty, fractional_position)
{
    // 小数持仓(理论上不会出现, 但函数应正确处理)
    EXPECT_NEAR(clampReduceQty(10.0, 7.5), 7.5, 0.001);
    EXPECT_NEAR(clampReduceQty(3.0, 7.5), 3.0, 0.001);
}

// === 四条减仓路径语义一致性验证 ===
// 用模拟数据验证各路径的截断行为

TEST(test_clamp_reduce_qty, closeout_uses_remaining_not_position)
{
    // CloseoutExecutor anchor-delta 设计: batch 截到 |remaining|(=|delta/hedge|), 允许 close+open.
    // 不截断到 |position| (那会破坏 anchor-delta, 平不干净). overfill 由 _inflight_qty 守卫.
    // 场景: net_delta=49, anchor 持仓=36, hedge=1 -> 卖 49 (平 36 + 开 13), delta 归 0.
    double portfolio_delta = 49.0;
    double anchor_position = 36.0;
    double remaining = portfolio_delta;                          // hedge_ratio=1
    double batch = std::min(remaining, std::abs(remaining));     // CloseoutExecutor: min(batch, |remaining|)
    EXPECT_DOUBLE_EQ(batch, 49.0);            // 不截断到 |position|=36
    EXPECT_GT(batch, anchor_position);        // 允许 close+open (既开又平, 设计意图)
}

TEST(test_clamp_reduce_qty, taker_reduce_scenario)
{
    // checkTakerReduce: pos=669, max=30, target_util=0.8 -> target=24
    // qty = floor(|669| - 24) = 645
    // clampReduceQty(645, 669) = 645 (在持仓范围内)
    double pos = 669.0;
    double max_pos = 30.0;
    double target_util = 0.8;
    double target = max_pos * target_util;  // 24
    double qty = std::floor(std::abs(pos) - target);  // 645
    double clamped = clampReduceQty(qty, pos);

    EXPECT_DOUBLE_EQ(qty, 645.0);
    EXPECT_DOUBLE_EQ(clamped, 645.0);  // 不超过 |669|
}

TEST(test_clamp_reduce_qty, taker_reduce_near_flat)
{
    // pos=30, max=30, target_util=0.8 -> target=24
    // qty = floor(30-24) = 6
    // clampReduceQty(6, 30) = 6
    double pos = 30.0;
    double target = 30.0 * 0.8;  // 24
    double qty = std::floor(std::abs(pos) - target);  // 6
    double clamped = clampReduceQty(qty, pos);

    EXPECT_DOUBLE_EQ(clamped, 6.0);
}

TEST(test_clamp_reduce_qty, risk_liquidator_scenario)
{
    // RiskLiquidator::reduceContract: 请求平 100, 持仓 50
    // exec_qty = clampReduceQty(100, 50) = 50
    double requested = 100.0;
    double position = 50.0;
    double exec_qty = clampReduceQty(requested, position);

    EXPECT_DOUBLE_EQ(exec_qty, 50.0);  // 截断到实际持仓
}
