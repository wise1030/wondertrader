/*!
 * \file test_v8_r4b.cpp
 * \brief V8-R4b (功能性修复+适度重构轮) 回归护栏
 *
 * 覆盖:
 *   - A-1  arb 乘数死亡链修复 (pair multiplier 装配进 calculator)
 *   - A-2  孤儿腿对冲失败有限重试 (拒单保留/节流/上限放弃)
 *   - A-3  双 in_flight 按通道精确释放 (含 A-3b 超时单位修复)
 *   - A-4  PairsTrading 私有历史配对去重
 *   - A-5  权重归一化排除未启用信号
 *   - S-1/S-2 AdaptiveWeightFramework::processTick 收拢 (IC/regime/权重单入口)
 */
#include "../WtFutuCore/arb/SpreadArbitrageManager.h"
#include "../WtFutuCore/arb/AsyncArbitrageExecutor.h"
#include "../WtFutuCore/arb/PairsTradingStrategy.h"
#include "../WtFutuCore/FutuPortfolio.h"
#include "../WtFutuCore/signals/ICWeightTracker.h"
#include "gtest/gtest.h"

#include <cmath>

using namespace futu;

//==============================================================================
// A-1: 乘数装配
//==============================================================================

TEST(ArbMultiplier, WiredIntoSpreadCalculation)
{
    SpreadCalculatorManager mgr;
    SpreadCalculatorConfig cfg;
    cfg.min_samples = 2;
    mgr.setConfig(cfg);

    SpreadPairConfig pair;
    pair.pair_id = "p1";
    pair.leg1_code = "A";
    pair.leg2_code = "B";
    pair.spread_type = SpreadType::WEIGHTED;
    pair.leg1_ratio = 1.0;
    pair.leg2_ratio = 1.0;
    pair.leg1_multiplier = 2.0; // V8-A1 前: 无 setter, 恒 1.0
    pair.leg2_multiplier = 1.0;
    mgr.addSpreadPair(pair);

    // fresh-pairing: leg2 先喂 (fresh 标记), leg1 触发配对
    mgr.onTick("B", 90.0, 1000);
    mgr.onTick("A", 100.0, 1001);
    mgr.onTick("B", 90.0, 1002);
    mgr.onTick("A", 100.0, 1003);

    // WEIGHTED: r1*P1*m1 - r2*P2*m2 = 100*2 - 90*1 = 110
    SpreadState st = mgr.getSpreadState("p1");
    EXPECT_DOUBLE_EQ(st.current_spread, 110.0);
}

TEST(ArbMultiplier, DefaultIsOneNotThreeHundred)
{
    SpreadPairConfig pair; // V8-A1: 默认 300 烟雾弹已改 1.0
    EXPECT_DOUBLE_EQ(pair.leg1_multiplier, 1.0);
    EXPECT_DOUBLE_EQ(pair.leg2_multiplier, 1.0);
}

//==============================================================================
// A-2: 孤儿腿对冲重试
//==============================================================================

namespace
{

AsyncArbitrageExecutor::OrphanLeg makeOrphan(uint64_t ts_us)
{
    AsyncArbitrageExecutor::OrphanLeg leg;
    leg.pair_id = "p1";
    leg.leg1_req_id = 1;
    leg.leg1_code = "A";
    leg.leg2_code = "B";
    leg.leg1_is_buy = true;
    leg.leg1_qty = 5;
    leg.leg1_price = 100.0;
    leg.timestamp = ts_us;
    leg.hedge_qty = 10.0; // ratio 1:2 场景
    return leg;
}

} // namespace

TEST(OrphanHedge, RejectionRetainsLegWithRetry)
{
    AsyncArbitrageExecutor exe;
    exe.setReplayNowUs(1000000);
    ASSERT_TRUE(exe.enqueueOrphanLeg(makeOrphan(0)));

    int calls = 0;
    double last_qty = 0;
    // 回调拒绝 (模拟 rate_limited) → leg 保留重试
    auto reject = [&](const std::string&, bool, double qty, bool) {
        ++calls;
        last_qty = qty;
        return false;
    };
    exe.setReplayNowUs(1000000 + 6000000); // age=6s > timeout 5s
    exe.processOrphanLegs(reject, 5000, 30000, 0.0);
    EXPECT_EQ(calls, 1);
    EXPECT_DOUBLE_EQ(last_qty, 10.0); // V8-A2: 用计划 leg2 量而非 leg1_qty=5
    EXPECT_EQ(exe.orphanLegsDeferredCount(), 1u); // 旧实现: 一次性移出

    // 500ms 节流内不重复尝试
    exe.setReplayNowUs(1000000 + 6100000);
    exe.processOrphanLegs(reject, 5000, 30000, 0.0);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(exe.orphanLegsDeferredCount(), 1u);

    // 节流过后继续重试至上限 (3 次) 后放弃
    exe.setReplayNowUs(1000000 + 6700000);
    exe.processOrphanLegs(reject, 5000, 30000, 0.0);
    exe.setReplayNowUs(1000000 + 7400000);
    exe.processOrphanLegs(reject, 5000, 30000, 0.0);
    exe.setReplayNowUs(1000000 + 8100000);
    exe.processOrphanLegs(reject, 5000, 30000, 0.0);
    EXPECT_EQ(calls, 4);                      // 1 初次 + 3 重试
    EXPECT_EQ(exe.orphanLegsDeferredCount(), 0u); // 上限后放弃 (有界)
}

TEST(OrphanHedge, AcceptedRemovesLeg)
{
    AsyncArbitrageExecutor exe;
    exe.setReplayNowUs(1000000);
    ASSERT_TRUE(exe.enqueueOrphanLeg(makeOrphan(0)));

    auto accept = [](const std::string&, bool, double, bool) { return true; };
    exe.setReplayNowUs(1000000 + 6000000);
    size_t n = exe.processOrphanLegs(accept, 5000, 30000, 0.0);
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(exe.orphanLegsDeferredCount(), 0u);
}

//==============================================================================
// A-3: 双 in_flight 按通道精确释放
//==============================================================================

namespace
{

// 构造价差偏离场景: 基线 spread=10, 后跳到 20 → zscore 显著为正
void feedDivergingTicks(SpreadArbitrageManager& mgr)
{
    uint64_t ts = 1000;
    for (int i = 0; i < 30; i++) { // 基线
        mgr.onTick("A", 100.0, ts++);
        mgr.onTick("B", 90.0, ts++);
    }
    for (int i = 0; i < 5; i++) { // 偏离
        mgr.onTick("A", 110.0, ts++);
        mgr.onTick("B", 90.0, ts++);
    }
}

} // namespace

TEST(ArbInFlight, DropReleasesOnlyMatchingChannel)
{
    SpreadArbitrageManager mgr;
    mgr.enable();
    FutuPortfolio portfolio; // B-3 门需 Portfolio 注入 (否则 gate 旁路, in_flight 不武装)
    mgr.setPortfolio(&portfolio);
    SpreadCalculatorConfig ccfg;
    ccfg.min_samples = 10;
    mgr.setCalculatorConfig(ccfg);

    SpreadPairConfig pair;
    pair.pair_id = "p1";
    pair.leg1_code = "A";
    pair.leg2_code = "B";
    pair.entry_z_threshold = 2.0;
    mgr.addSpreadPair(pair);
    feedDivergingTicks(mgr);

    uint64_t t = 1000000000ULL; // µs 域 (generateSignal 约定)
    // 首个信号: 过 B-3 门并武装 open in_flight
    SpreadSignal s1 = mgr.generateSignal("p1", t);
    ASSERT_EQ(s1.type, SpreadSignalType::OPEN_SHORT_SPREAD); // 价差上跳 -> 均值回归开空

    // in_flight 阻止重复发射
    SpreadSignal s2 = mgr.generateSignal("p1", t + 2000000ULL);
    EXPECT_EQ(s2.type, SpreadSignalType::NONE);

    // V8-A3 回归: 错通道释放 (is_close=true) 不得清 open 闸门 —
    // 旧实现无差别双清, 此处会错误放行
    mgr.onArbSignalDropped("p1", true);
    SpreadSignal s3 = mgr.generateSignal("p1", t + 4000000ULL);
    EXPECT_EQ(s3.type, SpreadSignalType::NONE);

    // 正确通道释放 → 重新发射
    mgr.onArbSignalDropped("p1", false);
    SpreadSignal s4 = mgr.generateSignal("p1", t + 6000000ULL);
    EXPECT_EQ(s4.type, SpreadSignalType::OPEN_SHORT_SPREAD);

    // 撤单事件释放实际在途通道
    mgr.onArbLegCancelled("p1");
    SpreadSignal s5 = mgr.generateSignal("p1", t + 8000000ULL);
    EXPECT_EQ(s5.type, SpreadSignalType::OPEN_SHORT_SPREAD);
}

//==============================================================================
// A-4: PairsTrading 配对去重
//==============================================================================

TEST(PairsTradingPairing, StaleLegTicksDoNotEnterHistory)
{
    PairsTradingStrategy stra;
    SpreadState st;
    st.pair_id = "p1";
    st.leg1_price = 100.0;
    st.leg2_price = 90.0;

    // 模拟 calculator fresh-pairing: last_update 仅在新配对样本时推进
    // 6 次 update 仅 3 个新样本 (任一腿 tick 不再稀释私有历史)
    uint64_t sample_ts[] = {1, 1, 2, 2, 3, 3};
    for (uint64_t ts : sample_ts) {
        st.last_update = ts;
        stra.update(st, ts);
    }
    EXPECT_EQ(stra.priceSampleCount(), 3u); // 旧实现: 6
}

//==============================================================================
// A-5: 归一化排除未启用信号
//==============================================================================

TEST(WeightNorm, DisabledSignalExcludedFromNormalization)
{
    AdaptiveWeightFramework fw;
    auto regime = MarketRegime::detect(50.0, 100.0, 100.0, 1000.0);
    double vals[5] = {0.1, 0.1, 0.1, 0.1, 0.1};
    bool all_on[5] = {true, true, true, true, true};

    auto w_all = fw.computeWeights(regime, vals, false, all_on);
    double sum_all = 0;
    for (double w : w_all)
        sum_all += w;
    EXPECT_NEAR(sum_all, 1.0, 1e-9);

    bool ll_off[5] = {true, true, true, true, false};
    auto w_part = fw.computeWeights(regime, vals, false, ll_off);
    size_t ll_idx = static_cast<size_t>(WeightedSignalType::LEAD_LAG);
    EXPECT_DOUBLE_EQ(w_part[ll_idx], 0.0);
    double sum_part = 0;
    for (double w : w_part)
        sum_part += w;
    // 归一化分母不含禁用信号 → 启用信号权重和仍为 1 (cap 后可能略低)
    EXPECT_GT(sum_part, 0.9);
    // 禁用后其余信号份额上升 (不再被摊薄)
    EXPECT_GT(w_part[0], w_all[0] - 1e-9);
}

//==============================================================================
// S-1/S-2: processTick 收拢
//==============================================================================

TEST(FrameworkProcessTick, WeightsProducedAndResettable)
{
    AdaptiveWeightFramework fw;
    double vals[5] = {0.1, -0.05, 0.02, 0.0, 0.0};
    bool enabled[5] = {true, true, true, true, true};
    std::array<double, static_cast<size_t>(WeightedSignalType::COUNT)> w{};

    for (int i = 0; i < 100; i++) {
        EXPECT_TRUE(fw.processTick(vals, enabled, 100.0 + i * 0.1, 50.0, 1000.0, false, w));
    }
    double sum = 0;
    for (double x : w)
        sum += x;
    EXPECT_NEAR(sum, 1.0, 0.05); // cap-after-normalize 语义允许略低

    fw.resetTickState();
    EXPECT_TRUE(fw.processTick(vals, enabled, 100.0, 50.0, 1000.0, false, w)); // 复位后可重新计数
}
