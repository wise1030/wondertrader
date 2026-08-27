/*!
 * \file test_v10_r7c.cpp
 * \brief V10 诊断报告 R7-c arb 安全包回归测试
 *
 * 覆盖:
 *   - P2-8: StatisticalArbStrategy 止损可达性 (raw_composite_signal 越阈 → STOP_LOSS;
 *     旧实现对 clamp 后的 [-1,1] 值判 >1.5 恒假, z 类止损静默缺位)
 *   - P2-6: convertLegQtyToOpposite ratio 换算 (失败腿手数 → 对侧计划对冲手数)
 *
 * 说明: B5 open 冷却(P2-7, applyB3Gate 私有)/FAK 越价(P2-9, 需 MM 快照与网关)/
 * 通道过滤(P2-10, 需 ctx) 无独立接缝, 验证靠编译+既有全量+v5 冒烟+arb 灰度回测
 * (见 AGENTS.md R7-c 方案条目)。
 */
#include "../WtFutuCore/arb/SpreadArbitrageManager.h"
#include "../WtFutuCore/arb/StatisticalArbStrategy.h"
#include "../WtFutuCore/arb/SpreadArbitrageTypes.h"
#include "gtest/gtest/gtest.h"

using namespace futu;

// === V10-P2-8: SA 止损可达性 ===

namespace
{

SpreadState saState(double zscore, double spread_position, double spread_std)
{
    SpreadState st;
    st.pair_id = "sa1";
    st.leg1_code = "A";
    st.leg2_code = "B";
    st.zscore = zscore;
    st.spread_position = spread_position;
    st.spread_std = spread_std;
    return st;
}

} // namespace

TEST(test_v10_r7c, SAStopLossReachableWithRawComposite)
{
    StatisticalArbStrategy sa;
    // 喂 min_samples(50)+ 条样本: zscore 恒 10 (归一化后 1.0);
    // spread_std 前 50 条=1.0、后 10 条=100 → recent/hist=100 → vol 因子贡献
    // 0.176×99 ≈ 17.4, raw composite ≈ 17.8 (旧实现: clamp 到 1.0 后判 >1.5 恒假)
    for (int i = 0; i < 50; ++i)
        sa.update(saState(10.0, 5.0, 1.0), i);
    for (int i = 50; i < 60; ++i)
        sa.update(saState(10.0, 5.0, 100.0), i);

    SpreadSignal sig = sa.generateSignal(saState(10.0, 5.0, 100.0), 1000);
    EXPECT_EQ(sig.type, SpreadSignalType::STOP_LOSS)
        << "raw_composite 越阈必须触发 z 类止损 (旧实现恒不触发)";
    EXPECT_GT(sig.suggested_size, 0.0);
}

TEST(test_v10_r7c, SAStopLossDefaultThresholdDocuments)
{
    StatisticalArbConfig cfg;
    // 语义固化: composite 单位; 0.40 = z 主导 + vol 因子同向放大才触发
    // (仅 z 饱和的 |raw| 上限 ≈ 0.353), 且不再是钳位单位下的死值 1.5
    EXPECT_DOUBLE_EQ(cfg.stop_loss_threshold, 0.40);
}

// === V10-P2-6: ratio 换算 ===

TEST(test_v10_r7c, ConvertLegQtyToOpposite)
{
    SpreadArbitrageManager mgr;
    SpreadPairConfig pair;
    pair.pair_id = "p1";
    pair.leg1_code = "A";
    pair.leg2_code = "B";
    pair.leg1_ratio = 1.0;
    pair.leg2_ratio = 3.0;
    ASSERT_TRUE(mgr.addSpreadPair(pair));

    double out = 0;
    // leg1 (r=1) 失败 10 手 → 对侧 leg2 计划对冲 = 10×(3/1) = 30
    EXPECT_TRUE(mgr.convertLegQtyToOpposite("p1", "A", 10.0, out));
    EXPECT_DOUBLE_EQ(out, 30.0);
    // leg2 (r=3) 失败 30 手 → 对侧 leg1 计划对冲 = 30×(1/3) = 10
    EXPECT_TRUE(mgr.convertLegQtyToOpposite("p1", "B", 30.0, out));
    EXPECT_DOUBLE_EQ(out, 10.0);
    // 未知腿 / 未知 pair / 非法量 → false (调用方回退 leg_qty 原值)
    EXPECT_FALSE(mgr.convertLegQtyToOpposite("p1", "C", 10.0, out));
    EXPECT_FALSE(mgr.convertLegQtyToOpposite("pX", "A", 10.0, out));
    EXPECT_FALSE(mgr.convertLegQtyToOpposite("p1", "A", 0.0, out));
}

TEST(test_v10_r7c, ConvertLegQtyDefaultsToOneWhenRatioUnset)
{
    SpreadArbitrageManager mgr;
    SpreadPairConfig pair;
    pair.pair_id = "p2";
    pair.leg1_code = "X";
    pair.leg2_code = "Y";
    pair.leg1_ratio = 0.0; // 未配 → 回退 1.0
    pair.leg2_ratio = 0.0;
    ASSERT_TRUE(mgr.addSpreadPair(pair));
    double out = 0;
    EXPECT_TRUE(mgr.convertLegQtyToOpposite("p2", "X", 7.0, out));
    EXPECT_DOUBLE_EQ(out, 7.0);
}
