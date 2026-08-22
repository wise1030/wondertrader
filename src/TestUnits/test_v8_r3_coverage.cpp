/*!
 * \file test_v8_r3_coverage.cpp
 * \brief V8-R3 TestUnits 补缺: signals/arb 子系统回归护栏
 *
 * 背景: 代码注释里每个"已修复"都对应一个应有而未有的测试 (V8 报告 §4)。
 * 本文件覆盖两块纯计算核心:
 *   - SpreadCalculator: 价差配对/统计 (arb 子系统唯一在产计算核心)
 *   - TickTransactionInferer: tick 推断 + V8-R3 数值泄漏修复回归
 */
#include "../WtFutuCore/arb/SpreadCalculator.h"
#include "../WtFutuCore/TickTransactionInferer.h"
#include "gtest/gtest.h"

#include <cmath>

using namespace futu;

//==============================================================================
// SpreadCalculator: 双腿新鲜配对 + 价差统计
//==============================================================================

namespace
{

// 喂一对腿的 tick (leg1 恒 100, leg2 恒 90 -> spread 恒 10)
// (SpreadCalculator::onTick(code,..) 为声明未定义死接口已删 -- 按 leg 直接喂)
void feedPairTicks(SpreadCalculator& calc, uint64_t ts)
{
    calc.onLeg1Tick(100.0, ts);
    calc.onLeg2Tick(90.0, ts);
}

} // namespace

TEST(SpreadCalculator, FreshPairingProducesConstantSpread)
{
    SpreadCalculator calc;
    SpreadCalculatorConfig cfg;
    cfg.min_samples = 10;
    calc.setConfig(cfg);

    // 未有足够样本: is_active=false
    for (uint32_t i = 0; i < 5; i++)
        feedPairTicks(calc, 1000 + i);
    EXPECT_FALSE(calc.getState().is_active);

    // 达到 min_samples 后激活, spread=leg1-leg2=10
    for (uint32_t i = 5; i < 12; i++)
        feedPairTicks(calc, 1000 + i);
    SpreadState st = calc.getState();
    EXPECT_TRUE(st.is_active);
    EXPECT_DOUBLE_EQ(st.current_spread, 10.0);
    EXPECT_DOUBLE_EQ(st.current_price, 10.0); // V8-A2: 止损链路接线
    EXPECT_NEAR(st.spread_std, 0.0, 1e-9); // 恒定价差 -> 零方差
}

TEST(SpreadCalculator, ZScoreReflectsSpreadDeviation)
{
    SpreadCalculator calc;
    SpreadCalculatorConfig cfg;
    cfg.min_samples = 10;
    calc.setConfig(cfg);

    // 阶段 1: 建立 spread=10 基线
    for (uint32_t i = 0; i < 30; i++)
        feedPairTicks(calc, 1000 + i);
    SpreadState base = calc.getState();
    EXPECT_NEAR(base.zscore, 0.0, 1e-9); // 恰在均值上

    // 阶段 2: 价差跳到 20 (leg1 涨) -- z-score 应显著为正
    for (uint32_t i = 0; i < 5; i++) {
        calc.onLeg1Tick(110.0, 2000 + i);
        calc.onLeg2Tick(90.0, 2000 + i);
    }
    SpreadState shifted = calc.getState();
    EXPECT_GT(shifted.zscore, 1.0); // 显著偏离 (相对零方差基线)
}

//==============================================================================
// TickTransactionInferer: V8-R3 数值泄漏回归
//==============================================================================

TEST(TickInferer, NoVolumeLeakAfterWindowExpiry)
{
    // 注: 时间戳基址须远大于 window(5000ms) -- pruneHistory 的
    // current_time-window 为无符号运算, 小时间戳会下溢成巨数误剪全部记录
    constexpr uint64_t T0 = 10000000ULL;

    TickTransactionInferer inf;
    TickInfererConfig cfg;
    cfg.imbalance_window_ms = 5000;
    cfg.large_trade_threshold = 50.0; // 大单线: B 帧 vol=50 大单, A 帧 vol=10 小单
    inf.setConfig(cfg);

    // prime: bid/ask 99.0/99.5, 盘口量 1000
    inf.inferFromTick(99.0, 99.5, 1000, 1000, 99.2, 0, T0);

    // 窗口 A: 10 笔小单买方向推断 (ask 每次消耗 10 -> vol=10 < 50, conf=0.2+10/30)
    for (int i = 1; i <= 10; i++) {
        auto t = inf.inferFromTick(99.0, 99.5, 1000, 1000.0 - 10 * i, 99.5, 10, T0 + i);
        ASSERT_TRUE(t.is_buy_initiated) << "iter " << i;
        ASSERT_DOUBLE_EQ(t.volume, 10.0);
        ASSERT_LT(t.confidence, 1.0); // 泄漏条件: (1-confidence)×volume > 0
    }
    {
        auto stats = inf.getFlowStats();
        EXPECT_NEAR(stats.buy_volume, 10 * 10 * (0.2 + 10.0 / 30.0), 1e-6);
    }

    // 窗口 B: 时间前进 4×window (A 全部过期), 一笔大单 (ask 消耗 50 -> vol=50 >= 50)
    // V8-R3 修复前: _total_volume 残留 A 窗 Σ(1-conf)×vol = 10×10×0.4667 = 46.7,
    //   large_trade_ratio = 50/(50+46.7) = 0.52 (失真);
    // 修复后: add/prune 对称, _total_volume = 50, ratio = 1.0
    auto big = inf.inferFromTick(99.0, 99.5, 1000, 850.0, 99.5, 10, T0 + 20000);
    ASSERT_TRUE(big.is_buy_initiated);
    ASSERT_DOUBLE_EQ(big.volume, 50.0);

    auto imb = inf.getInferredImbalance();
    EXPECT_NEAR(imb.large_trade_ratio, 1.0, 1e-6);

    auto stats = inf.getFlowStats();
    EXPECT_NEAR(stats.buy_volume, 50.0 * 0.9, 1e-6); // 大单 conf clamp 至 0.9
    EXPECT_EQ(stats.tick_count, 11u); // 累计计数 (prune 不回退): A 窗 10 + B 帧 1
}

TEST(TickInferer, DirectionClassificationByPricePosition)
{
    TickTransactionInferer inf;
    inf.setConfig(TickInfererConfig());

    constexpr uint64_t T0 = 10000000ULL;
    // last 上穿 ask (含 ask 消耗) -> 买方发起
    inf.inferFromTick(99.0, 99.5, 30, 30, 99.2, 0, T0); // prime
    auto buy = inf.inferFromTick(99.0, 99.5, 30, 25, 99.5, 10, T0 + 100);
    EXPECT_TRUE(buy.is_buy_initiated);

    // last 跌破 bid (含 bid 消耗) -> 卖方发起
    TickTransactionInferer inf2;
    inf2.setConfig(TickInfererConfig());
    inf2.inferFromTick(99.0, 99.5, 30, 30, 99.2, 0, T0); // prime
    auto sell = inf2.inferFromTick(99.0, 99.5, 25, 30, 99.0, 10, T0 + 100);
    EXPECT_TRUE(sell.is_sell_initiated);
}
