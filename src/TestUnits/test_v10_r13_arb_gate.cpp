/**
 * @file test_v10_r13_arb_gate.cpp
 * @brief V10-R13 套利风控闸门方向感知 + MR 信号链修复 回归锁定
 *
 * 背景 (09-04 生产实证, 详见 AGENTS.md R13 立案):
 *   1. 前置探针 canOpenPosition(pair, 1.0) 恒按正向试探 —— 空头侧 pos=-3 时
 *      -3+1=-2 恰好放行, 一批打到 -8 (POSITION_LIMIT val=8>thr=3 持续告警)
 *   2. 该探针对 STOP_LOSS/TIMEOUT 一并误杀 —— "越超限越不许止损"
 *   3. MR 加仓块不在 else-if 链内 —— TIMEOUT_EXIT 命中后被加仓 if 覆盖成 OPEN
 *   4. primaryStrategy "statistical_arb" 静默回落 mean_reversion 无告警
 *
 * 修复语义锁定:
 *   - checkPositionLimits(pair, signed_size): size 带符号, 方向感知投影
 *   - signedOpenQty: 仅 OPEN_*_SPREAD 产生非零投影, 减险信号恒 0 (不受门控)
 *   - MR: 加仓块包进 else, TIMEOUT/CLOSE/STOP 优先
 *   - strategyFromString: "statistical_arb" 识别, 未知值显式回落
 */
#include <gtest/gtest.h>
#include "../WtFutuCore/arb/SpreadRiskManager.h"
#include "../WtFutuCore/arb/SpreadArbitrageManager.h"
#include "../WtFutuCore/arb/MeanReversionStrategy.h"

namespace
{

using namespace futu;

//==============================================================================
// T1: checkPositionLimits 带符号投影 (C1 核心语义)
//==============================================================================
TEST(V10R13ArbGate, PositionGateSignedProjection)
{
    SpreadRiskManager rm;
    SpreadRiskConfig cfg; // 默认 max_single_pair=20
    cfg.max_single_pair = 3.0;
    cfg.max_total_position = 100.0;
    rm.setConfig(cfg);

    SpreadState st;
    st.spread_position = -3.0;
    rm.updatePairState("ao1112", st);

    // 空头方向继续加仓: -3 + (-5) = -8, |8|>3 → 拒 (旧探针 -3+1=-2 恰好放行)
    EXPECT_FALSE(rm.checkPositionLimits("ao1112", -5.0));
    // 多头方向减仓投影: -3 + 1 = -2, |2|<=3 → 放
    EXPECT_TRUE(rm.checkPositionLimits("ao1112", 1.0));
    // 空头小幅: -3 + (-0.5) = -3.5 > 3 → 拒
    EXPECT_FALSE(rm.checkPositionLimits("ao1112", -0.5));

    // 正仓位侧对称: pos=+2, +2 → 4>3 拒; -2 → 0 放
    SpreadState st2;
    st2.spread_position = 2.0;
    rm.updatePairState("ao1112", st2);
    EXPECT_FALSE(rm.checkPositionLimits("ao1112", 2.0));
    EXPECT_TRUE(rm.checkPositionLimits("ao1112", -2.0));
}

//==============================================================================
// T2: signedOpenQty 信号类型 → 带符号投影映射 (C2 核心: 减险信号不受门控)
//==============================================================================
TEST(V10R13ArbGate, SignedOpenQtyMapping)
{
    // 开仓类: 方向映射
    EXPECT_DOUBLE_EQ(SpreadArbitrageManager::signedOpenQty(SpreadSignalType::OPEN_LONG_SPREAD, 5.0), 5.0);
    EXPECT_DOUBLE_EQ(SpreadArbitrageManager::signedOpenQty(SpreadSignalType::OPEN_SHORT_SPREAD, 5.0), -5.0);

    // 减险/无效类: 恒 0 (不受仓位上限门控 —— 止损豁免)
    EXPECT_DOUBLE_EQ(SpreadArbitrageManager::signedOpenQty(SpreadSignalType::STOP_LOSS, 8.0), 0.0);
    EXPECT_DOUBLE_EQ(SpreadArbitrageManager::signedOpenQty(SpreadSignalType::TIMEOUT_EXIT, 8.0), 0.0);
    EXPECT_DOUBLE_EQ(SpreadArbitrageManager::signedOpenQty(SpreadSignalType::CLOSE_LONG_SPREAD, 3.0), 0.0);
    EXPECT_DOUBLE_EQ(SpreadArbitrageManager::signedOpenQty(SpreadSignalType::CLOSE_SHORT_SPREAD, 3.0), 0.0);
    EXPECT_DOUBLE_EQ(SpreadArbitrageManager::signedOpenQty(SpreadSignalType::NONE, 1.0), 0.0);
}

//==============================================================================
// T3: MR 超时退出不被加仓覆盖 (C3)
//==============================================================================
TEST(V10R13ArbGate, MRTimeoutNotOverwrittenByAdd)
{
    MeanReversionStrategy mr;
    SpreadState st;
    st.is_active = true;        // min_samples 预热门 (calculator hasEnoughSamples 置位)
    st.spread_position = -2.0;  // 持有空价差
    st.zscore = 2.0;            // 落在加仓区 (entry*0.5=1.0 < z < stop*0.75=3.0)
    st.half_life = 0.0;         // 不过半衰期滤网

    // current_time 与 position_open_time 同域; duration = 3.601e9 > convergence_timeout(3600)
    uint64_t now = 3601000000ULL;
    st.position_open_time = 0;

    SpreadSignal sig = mr.generateSignal(st, now);

    // 修复前: TIMEOUT_EXIT 命中后被下方加仓 if 覆盖为 OPEN_SHORT_SPREAD
    EXPECT_EQ(sig.type, SpreadSignalType::TIMEOUT_EXIT);
    EXPECT_DOUBLE_EQ(sig.suggested_size, 2.0);
}

//==============================================================================
// T4: 策略名映射 (C5)
//==============================================================================
TEST(V10R13ArbGate, StrategyFromString)
{
    ArbitrageStrategy out = ArbitrageStrategy::MEAN_REVERSION;

    EXPECT_TRUE(strategyFromString("statistical_arb", out));
    EXPECT_EQ(out, ArbitrageStrategy::STATISTICAL_ARB); // 修复前静默回落 MR

    EXPECT_TRUE(strategyFromString("statistical", out));
    EXPECT_EQ(out, ArbitrageStrategy::STATISTICAL_ARB);

    EXPECT_TRUE(strategyFromString("mean_reversion", out));
    EXPECT_EQ(out, ArbitrageStrategy::MEAN_REVERSION);

    EXPECT_TRUE(strategyFromString("pairs_trading", out));
    EXPECT_EQ(out, ArbitrageStrategy::PAIRS_TRADING);

    EXPECT_TRUE(strategyFromString("trend_following", out));
    EXPECT_EQ(out, ArbitrageStrategy::TREND_FOLLOWING);

    // 未知值: false (调用方 warn + 显式回落 MEAN_REVERSION)
    EXPECT_FALSE(strategyFromString("garbage", out));
    EXPECT_FALSE(strategyFromString("", out));
}

} // namespace
