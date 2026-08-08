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
    // 请求 49(组合净delta), 持仓 36(anchor实际) -> 返回 36 (不开反向仓)
    // 这正是 00:58 平仓又开仓事故的场景
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

TEST(test_clamp_reduce_qty, closeout_scenario_no_reverse_open)
{
    // 模拟 00:58 事故场景:
    // 组合净 delta = 49, anchor(ao2609) 实际净持仓 = 36
    // 旧 CloseoutExecutor: batch_qty = min(49, |remaining|=49) = 49 -> 卖 49
    //   -> 平 36 多 + 开 13 空 (平仓又开仓!)
    // 新: batch_qty = clampReduceQty(49, 36) = 36 -> 只平 36, 不开反向
    double portfolio_delta = 49.0;
    double anchor_position = 36.0;
    double old_batch = std::min(portfolio_delta, std::abs(portfolio_delta));  // 旧逻辑
    double new_batch = clampReduceQty(old_batch, anchor_position);            // 新逻辑

    EXPECT_DOUBLE_EQ(old_batch, 49.0);   // 旧: 不截断到实际持仓
    EXPECT_DOUBLE_EQ(new_batch, 36.0);   // 新: 截断, 不开反向仓
    EXPECT_LT(new_batch, old_batch);      // 新 < 旧 -> 不超卖
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
