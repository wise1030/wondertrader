/*!
 * \file test_toxicity_direction.cpp
 * \brief V8-T1/T5 毒性方向语义 unit tests (表驱动)
 *
 * 背景: AlphaResult 构造漏填 ofi_component -> toxic_side 恒 0, 单边抑制从不生效;
 *       ToxicityPolicy 方向映射疑似错边 (激进买流停 bid 而非 ask)。
 *
 * 统一语义 (报告 §6.2, 用户已确认): toxic_side==1 为激进买流 (ofi>0 且
 * trade imbalance>0) -> 知情买方吃我方 ask -> 抑制 ask; ==-1 -> 抑制 bid;
 * ==0 (信号分歧/无方向) -> 双边抑制。
 */
#include "../WtFutuCore/ToxicFlowDetector.h"
#include "../WtFutuCore/PredictiveToxicity.h"
#include "../WtFutuCore/RealizedToxicity.h"
#include "../WtFutuCore/SelfTradeCalibrator.h"
#include "../WtFutuCore/QuotePolicyChain.h"
#include "gtest/gtest.h"

#include <memory>

using namespace futu;

namespace
{

// 默认 ToxicityParams: adverse_threshold=0.10, alpha_weight=0.3, book_weight=0.3,
// extreme_signal_weight=0.8, min_warmup_buckets=5
// 注: 必须显式 setParams -- 默认构造的 detector 其 _predictive 子组件
// 用自身默认阈值 (alpha_threshold=0.7), 与门面 _params 不一致
ToxicityMetrics feedAndAnalyze(double ofi, double imbalance)
{
    ToxicFlowDetector detector;
    detector.setParams(ToxicityParams());
    AlphaResult alpha;
    alpha.alpha = ofi;
    alpha.ofi_component = ofi; // V8-T1: 此前链路上游漏填的就是这个分量
    alpha.timestamp = 1000;
    TradeImbalanceResult imb;
    imb.imbalance_ratio = imbalance;
    imb.net_flow = imbalance * 100;
    detector.updateMarketAlpha(alpha, imb);
    return detector.analyze();
}

// 毒性未触发 (信号弱): 0.3*0.05 + 0.3*0.025 = 0.0225 < 0.10 阈值, 无 extreme
ToxicityMetrics weakSignal()
{
    return feedAndAnalyze(0.05, 0.05);
}

QuotePolicyContext makeCtx(ToxicFlowDetector* tox)
{
    QuotePolicyContext ctx;
    ctx.code = "SHFE.ec.ec2509";
    ctx.use_toxicity_detector = true;
    ctx.toxicity = tox;
    ctx.timestamp = 10000;
    ctx.toxicity_cooloff_ms = 5000;
    return ctx;
}

std::unique_ptr<ToxicFlowDetector> makeDetector(double ofi, double imbalance)
{
    auto detector = std::make_unique<ToxicFlowDetector>();
    detector->setParams(ToxicityParams());
    AlphaResult alpha;
    alpha.ofi_component = ofi;
    TradeImbalanceResult imb;
    imb.imbalance_ratio = imbalance;
    detector->updateMarketAlpha(alpha, imb);
    return detector;
}

} // namespace

//------------------------------------------------------------------------------
// 检测层: toxic_side 方向判定 (ofi 与 imbalance 同号才给方向)
//------------------------------------------------------------------------------

TEST(ToxicDirection, AggressiveBuyFlowGivesPositiveSide)
{
    ToxicityMetrics m = feedAndAnalyze(0.9, 0.9);
    EXPECT_TRUE(m.is_toxic);
    EXPECT_EQ(m.toxic_side, 1);
}

TEST(ToxicDirection, AggressiveSellFlowGivesNegativeSide)
{
    ToxicityMetrics m = feedAndAnalyze(-0.9, -0.9);
    EXPECT_TRUE(m.is_toxic);
    EXPECT_EQ(m.toxic_side, -1);
}

TEST(ToxicDirection, DisagreeingSignalsGiveNoSide)
{
    // ofi 与 imbalance 分歧 -> toxic_side=0 (双边抑制)
    ToxicityMetrics m = feedAndAnalyze(0.9, -0.9);
    EXPECT_TRUE(m.is_toxic);
    EXPECT_EQ(m.toxic_side, 0);
}

TEST(ToxicDirection, WeakSignalNotToxic)
{
    ToxicityMetrics m = weakSignal();
    EXPECT_FALSE(m.is_toxic);
}

//------------------------------------------------------------------------------
// 抑制层: ToxicityPolicy 方向映射 (V8-T5 交换后)
//------------------------------------------------------------------------------

TEST(ToxicDirection, BuyFlowSuppressesAskNotBid)
{
    auto detector = makeDetector(0.9, 0.9);
    ASSERT_TRUE(detector->analyze().is_toxic);

    ToxicityPolicy policy;
    QuoteState st;
    policy.apply(makeCtx(detector.get()), st);
    EXPECT_FALSE(st.allow_ask); // 激进买流: ask 面临逆向选择
    EXPECT_TRUE(st.allow_bid);  // bid 不抑制
}

TEST(ToxicDirection, SellFlowSuppressesBidNotAsk)
{
    auto detector = makeDetector(-0.9, -0.9);
    ASSERT_TRUE(detector->analyze().is_toxic);

    ToxicityPolicy policy;
    QuoteState st;
    policy.apply(makeCtx(detector.get()), st);
    EXPECT_FALSE(st.allow_bid); // 激进卖流: bid 面临逆向选择
    EXPECT_TRUE(st.allow_ask);
}

TEST(ToxicDirection, NoSideSuppressesBoth)
{
    // ofi 与 imbalance 分歧 -> side=0 -> 双边抑制
    auto detector = makeDetector(0.9, -0.9);
    ASSERT_TRUE(detector->analyze().is_toxic);

    ToxicityPolicy policy;
    QuoteState st;
    policy.apply(makeCtx(detector.get()), st);
    EXPECT_FALSE(st.allow_bid);
    EXPECT_FALSE(st.allow_ask);
}

TEST(ToxicDirection, CooloffKeepsBothSidesPausedAfterToxicLifts)
{
    // 先触发毒性 -> 置冷却; 随后信号转弱 (is_toxic=false) 但仍在冷却期内 -> 双边抑制
    auto toxic_detector = makeDetector(0.9, 0.9);

    ToxicityPolicy policy;
    QuoteState st1;
    policy.apply(makeCtx(toxic_detector.get()), st1); // toxic, 置 _resume_time=15000
    ASSERT_FALSE(st1.allow_ask);

    auto calm_detector = std::make_unique<ToxicFlowDetector>(); // 无数据 -> not toxic
    calm_detector->setParams(ToxicityParams());
    QuotePolicyContext ctx2 = makeCtx(calm_detector.get());
    ctx2.timestamp = 12000; // < 15000 冷却期内
    QuoteState st2;
    policy.apply(ctx2, st2);
    EXPECT_FALSE(st2.allow_bid);
    EXPECT_FALSE(st2.allow_ask);
}

//------------------------------------------------------------------------------
// VPIN warmup 门: 预热期(桶数不足) vpin 强制 0, 不参与触发
//------------------------------------------------------------------------------

TEST(ToxicWarmup, VpinGatedUntilWarmupBuckets)
{
    PredictiveToxicity pt;
    PredictiveToxicityConfig cfg; // bucket_size=1000, min_warmup_buckets=5
    pt.setConfig(cfg);
    pt.setBucketSize(1000); // onTrade 需显式初始化桶容量 (生产由 onTickVolume 惰性置位)

    // 4 桶 (每桶 4 笔 x 300 = 1200 >= 1000 关桶): 不足 5 -> vpin=0
    for (int b = 0; b < 4; b++)
        for (int i = 0; i < 4; i++)
            pt.onTrade(100.0 + b, 300, true, 1000 + b * 100 + i);

    PredictiveToxicityResult r = pt.analyze();
    EXPECT_EQ(r.vpin, 0.0); // warmup 门
    EXPECT_FALSE(r.vpin_ready);
    EXPECT_FALSE(r.is_toxic);

    // 第 5 桶 -> vpin 生效 (纯买流 imbalance 高)
    for (int i = 0; i < 4; i++)
        pt.onTrade(200.0, 300, true, 2000 + i);
    r = pt.analyze();
    EXPECT_GT(r.vpin, 0.0);
}

//------------------------------------------------------------------------------
// R2: 归一化数值 (V8 T2/T3/T4/T6)
//------------------------------------------------------------------------------

// T3: VPIN 严格有界 [0,1] -- 纯单边流 vpin 恰为 1.0 (原口径 |buy-sell|/bucket_size
// 按整桶量算可为 1200/1000=1.2, 无界且系统性高估)
TEST(ToxicNormalization, VpinBoundedToOneForOneSidedFlow)
{
    PredictiveToxicity pt;
    pt.setConfig(PredictiveToxicityConfig());
    pt.setBucketSize(1000);

    for (int b = 0; b < 6; b++)
        for (int i = 0; i < 4; i++)
            pt.onTrade(100.0 + b, 300, true, 1000 + b * 100 + i);

    PredictiveToxicityResult r = pt.analyze();
    EXPECT_TRUE(r.vpin_ready);
    EXPECT_DOUBLE_EQ(r.vpin, 1.0); // 桶归一: |buy-sell|/total = 1200/1200
}

// T3: 均衡流 vpin 为 0 (每桶 2 买 + 2 卖 = 1200, imbalance=0)
TEST(ToxicNormalization, VpinZeroForBalancedFlow)
{
    PredictiveToxicity pt;
    pt.setConfig(PredictiveToxicityConfig());
    pt.setBucketSize(1000);

    for (int b = 0; b < 6; b++) {
        for (int i = 0; i < 2; i++) {
            pt.onTrade(100.0, 300, true, 1000 + b * 100 + i * 2);
            pt.onTrade(100.0, 300, false, 1000 + b * 100 + i * 2 + 1);
        }
    }

    PredictiveToxicityResult r = pt.analyze();
    EXPECT_TRUE(r.vpin_ready);
    EXPECT_DOUBLE_EQ(r.vpin, 0.0);
}

// T6: alpha 通道权重归一 + combined 通道加权 (vpin_w 可配置)
TEST(ToxicNormalization, CombinedWeightedNormalized)
{
    PredictiveToxicity pt;
    pt.setConfig(PredictiveToxicityConfig()); // ofi_w=trade_w=0.5, vpin_w=0.5

    AlphaResult a;
    a.ofi_component = 1.0;
    TradeImbalanceResult t; // imbalance_ratio=0, large_trade_ratio=0 -> trade_tox=0
    pt.updateAlpha(a, t);

    PredictiveToxicityResult r = pt.analyze();
    EXPECT_DOUBLE_EQ(r.alpha_toxicity, 0.5); // 0.5×1.0 + 0.5×0
    EXPECT_DOUBLE_EQ(r.combined_score, 0.25); // vpin=0(warmup): 0.5×0 + 0.5×0.5
}

// T2: realized 通道不再内部乘 weight (原 0.8×0.4×1.0=0.32 被稀释 2.5 倍,
// 门面再乘 realized_weight 形成平方)
TEST(ToxicNormalization, RealizedNoInternalWeight)
{
    RealizedToxicity rt;
    RealizedToxicityConfig cfg;
    cfg.weight = 0.4;
    cfg.min_samples = 3;
    rt.setConfig(cfg);

    CalibrationResult cal;
    cal.toxicity_level = 0.8;
    cal.sample_size = 10;
    cal.confidence = 1.0;
    rt.onCalibration(cal);

    RealizedToxicityResult r = rt.analyze();
    EXPECT_DOUBLE_EQ(r.decayed_score, 0.8); // adverse_ratio × confidence, 无内部 weight
}

// T4: VPIN 独立触发 -- alpha 通道安静 (toxic_score=0 < 阈值) 但单边流
// vpin 超阈值时 is_toxic 仍为 true (此前 pred 的 OR 条件被门面丢弃)
TEST(ToxicNormalization, VpinIndependentTrigger)
{
    ToxicFlowDetector d;
    ToxicityParams p;
    p.adverse_threshold = 0.9; // combined 通道阈值调高 (不触发)
    p.vpin_threshold = 0.6;
    d.setParams(p);
    d.setBucketSize(1000);

    // 无 alpha 数据; 6 桶纯买单 -> vpin=1.0 > 0.6
    for (int b = 0; b < 6; b++)
        for (int i = 0; i < 4; i++)
            d.onTrade(100.0 + b, 300, true, 1000 + b * 100 + i);

    ToxicityMetrics m = d.analyze();
    // combined 含 vpin 通道: 0.5(vpin_weight)×1.0 = 0.5, 仍 < adverse 0.9
    EXPECT_DOUBLE_EQ(m.toxic_score, 0.5);
    EXPECT_TRUE(m.is_toxic); // VPIN 独立触发 (V8-T4)
}
