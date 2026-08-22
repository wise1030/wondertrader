/*!
 * \file test_v8_r5_fixes.cpp
 * \brief V8 诊断报告 R5 收尾轮修复的回归护栏
 *
 * 覆盖:
 *   - R5  FutuConfig 读取语义 (空值/类型错回落默认, 布尔数值兼容)
 *   - R6  FutuConfigLoader fail-fast (contracts 缺失 / anchorCode 校验)
 *   - A11 NaN 护栏 (estimateHalfLife 零方差 / calculateVolatilityFeature n==10)
 *   - S6  momentum window 配置接线 (原固定全缓冲 128)
 *   - R3  PerformanceAnalyzer 时钟统一后 30s 低流动性强制过期生效
 *   - R4  TscClock invariant 探测 + 校准
 */
#include "../WtFutuCore/FutuConfig.h"
#include "../WtFutuCore/FutuConfigLoader.h"
#include "../WtFutuCore/TscClock.h"
#include "../WtFutuCore/PerformanceAnalyzer.h"
#include "../WtFutuCore/signals/MarketDataContext.h"
#include "../WtFutuCore/signals/MomentumSignalSource.h"
#include "../WtFutuCore/arb/SpreadCalculator.h"
#include "../WtFutuCore/arb/StatisticalArbStrategy.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "gtest/gtest.h"

#include <cmath>
#include <fstream>
#include <vector>

using namespace futu;

namespace
{

std::string writeTmpYaml(const char* content)
{
    static int seq = 0;
    std::string path = "/tmp/opencode/r5_test_" + std::to_string(++seq) + ".yaml";
    std::ofstream f(path);
    f << content;
    return path;
}

wtp::WTSTickData* makeTick(const char* code, double bid, double ask, uint32_t action_time)
{
    auto* t = wtp::WTSTickData::create(code);
    auto& s = t->getTickStruct();
    s.bid_prices[0] = bid;
    s.bid_qty[0] = 10;
    s.ask_prices[0] = ask;
    s.ask_qty[0] = 10;
    s.price = (bid + ask) / 2.0;
    s.action_time = action_time;
    return t;
}

} // namespace

//==============================================================================
// R5: FutuConfig 读取语义
//==============================================================================

TEST(FutuConfigRead, EmptyAndMissingFallBackToDefault)
{
    std::string yaml = writeTmpYaml(
        "root:\n"
        "  emptyVal:\n"
        "  numVal: 3.5\n"
        "  objVal:\n"
        "    a: 1\n");
    wtp::WTSVariant* cfg = WTSCfgLoader::load_from_file(yaml.c_str());
    ASSERT_NE(cfg, nullptr);
    wtp::WTSVariant* root = cfg->get("root");
    ASSERT_NE(root, nullptr);

    // 键存在但值为空 -> 默认值 (旧语义: asDouble() 空串 -> 0.0, 静默关保护)
    EXPECT_DOUBLE_EQ(FutuConfig::readDouble(root, "emptyVal", 7.0), 7.0);
    EXPECT_EQ(FutuConfig::readUInt32(root, "emptyVal", 7u), 7u);
    // 键缺失 -> 默认值
    EXPECT_DOUBLE_EQ(FutuConfig::readDouble(root, "missing", 7.0), 7.0);
    // 类型错 (Object) -> 默认值
    EXPECT_DOUBLE_EQ(FutuConfig::readDouble(root, "objVal", 7.0), 7.0);
    // 正常值不受影响
    EXPECT_DOUBLE_EQ(FutuConfig::readDouble(root, "numVal", 7.0), 3.5);

    cfg->release();
}

TEST(FutuConfigRead, BoolAcceptsNumericAndDefaultFallback)
{
    std::string yaml = writeTmpYaml(
        "root:\n"
        "  boolNum: 1\n"
        "  boolZero: 0\n"
        "  boolFalse: false\n"
        "  boolTrue: true\n"
        "  emptyVal:\n");
    wtp::WTSVariant* cfg = WTSCfgLoader::load_from_file(yaml.c_str());
    ASSERT_NE(cfg, nullptr);
    wtp::WTSVariant* root = cfg->get("root");
    ASSERT_NE(root, nullptr);

    // 数值 1 -> true (旧语义: asBoolean 只认 "true"/"yes" 字符串 -> false)
    EXPECT_TRUE(FutuConfig::readBool(root, "boolNum", false));
    EXPECT_FALSE(FutuConfig::readBool(root, "boolZero", true));
    // 布尔/字符串原语义保持
    EXPECT_FALSE(FutuConfig::readBool(root, "boolFalse", true));
    EXPECT_TRUE(FutuConfig::readBool(root, "boolTrue", false));
    // 空值/缺失 -> 默认值
    EXPECT_TRUE(FutuConfig::readBool(root, "emptyVal", true));
    EXPECT_TRUE(FutuConfig::readBool(root, "missing", true));

    cfg->release();
}

//==============================================================================
// R6: FutuConfigLoader fail-fast
//==============================================================================

namespace
{

bool loadFromYaml(const char* content, FutuMmConfig& cfg, std::vector<ContractInfo>& contracts)
{
    std::string path = writeTmpYaml(content);
    wtp::WTSVariant* root = WTSCfgLoader::load_from_file(path.c_str());
    if (!root)
        return false;
    bool ok = FutuConfigLoader::load(root, cfg, contracts, "r5_test");
    root->release();
    return ok;
}

} // namespace

TEST(ConfigLoaderFailFast, MissingContractsRejected)
{
    FutuMmConfig cfg;
    std::vector<ContractInfo> contracts;
    // 无 contracts 段 -> 旧实现静默通过零合约空跑
    EXPECT_FALSE(loadFromYaml("anchorCode: SHFE.ag.ag2608\n", cfg, contracts));
}

TEST(ConfigLoaderFailFast, EmptyAnchorRejected)
{
    FutuMmConfig cfg;
    std::vector<ContractInfo> contracts;
    EXPECT_FALSE(loadFromYaml("contracts:\n  - code: SHFE.ag.ag2608\n", cfg, contracts));
}

TEST(ConfigLoaderFailFast, AnchorNotInContractsRejected)
{
    FutuMmConfig cfg;
    std::vector<ContractInfo> contracts;
    EXPECT_FALSE(loadFromYaml(
        "anchorCode: SHFE.ag.ag2609\n"
        "contracts:\n"
        "  - code: SHFE.ag.ag2608\n",
        cfg,
        contracts));
}

TEST(ConfigLoaderFailFast, MinimalValidAccepted)
{
    FutuMmConfig cfg;
    std::vector<ContractInfo> contracts;
    EXPECT_TRUE(loadFromYaml(
        "anchorCode: SHFE.ag.ag2608\n"
        "contracts:\n"
        "  - code: SHFE.ag.ag2608\n",
        cfg,
        contracts));
    ASSERT_EQ(contracts.size(), 1u);
    EXPECT_EQ(contracts[0].code, "SHFE.ag.ag2608");
}

//==============================================================================
// A11: NaN 护栏
//==============================================================================

TEST(HalfLifeGuard, ZeroVarianceSpreadReturnsZeroNotNaN)
{
    SpreadCalculator calc;
    SpreadCalculatorConfig cfg;
    cfg.min_samples = 10;
    calc.setConfig(cfg);

    // 恒定价差 (零方差): 50 个样本触发 estimateHalfLife (_welford_n%50==0)
    // 旧代码分母 0/0 -> theta=NaN -> half_life=NaN 绕过 MeanReversion 过滤
    for (uint32_t i = 0; i < 60; i++) {
        calc.onLeg1Tick(100.0, 1000 + i);
        calc.onLeg2Tick(90.0, 1000 + i);
    }
    double hl = calc.getHalfLife();
    EXPECT_TRUE(std::isfinite(hl));
    EXPECT_DOUBLE_EQ(hl, 0.0);
}

TEST(StatArbVolGuard, EmptyHistWindowReturnsNeutralNotNaN)
{
    StatisticalArbStrategy stra;
    StatisticalArbConfig cfg;
    cfg.min_samples = 10; // 恰好 n==10 时 hist 段为空 (旧代码 0/0=NaN 滑过守卫)
    stra.setConfig(cfg);

    SpreadState state;
    state.pair_id = "p1";
    state.zscore = 0;
    state.spread_std = 0; // 波动率历史全 0
    state.correlation = 0;
    for (int i = 0; i < 10; i++)
        stra.update(state, 1000 + i);

    SpreadSignal sig = stra.generateSignal(state, 2000);
    // 旧代码: vol_ratio=NaN -> composite 钳位 +1.0 (最强做空) -> OPEN_SHORT_SPREAD
    // 修复后: vol_ratio=1.0 (中性) -> composite=0 -> NONE
    EXPECT_EQ(sig.type, SpreadSignalType::NONE);
}

//==============================================================================
// S6: momentum window 配置接线
//==============================================================================

TEST(MomentumWindow, ConfiguredWindowIsHonored)
{
    MarketDataContext ctx;
    ctx.setContract("SHFE.ec.ec2509", 0.5);

    MomentumSignalSource::Config cfg;
    cfg.window = 10;
    cfg.ema_alpha = 1.0; // EMA 直通, 直接观察窗口动量
    MomentumSignalSource mom(cfg);

    // 先 10 个负收益 (mid 每 tick ×e^-0.01), 后 10 个正收益 (×e^+0.01)
    // window=10 生效 -> 均值只取最近 10 个正收益 -> tanh 饱和 ≈ +1
    // 旧实现 (全缓冲) -> 均值 ≈ 0 -> alpha ≈ 0
    double mid = 1000.0;
    uint32_t ts = 1000;
    auto feed = [&](double lr) {
        mid *= std::exp(lr);
        auto* t = makeTick("SHFE.ec.ec2509", mid - 0.25, mid + 0.25, ts++);
        ctx.onTick(t);
        mom.update(ctx);
        t->release();
    };
    feed(0.0); // 首帧建立 _last_mid
    for (int i = 0; i < 10; i++)
        feed(-0.01);
    for (int i = 0; i < 10; i++)
        feed(+0.01);

    ASSERT_TRUE(mom.result().valid);
    EXPECT_GT(mom.getAlphaValue(), 0.9); // tanh(0.01*1000)≈1, 窗口化后饱和
}

//==============================================================================
// R3: PerformanceAnalyzer 同时钟后低流动性超时过期生效
//==============================================================================

TEST(PerfAnalyzerClock, ForcedExpiryWorksOnSameClock)
{
    PerformanceAnalyzer pa;

    TradeRecord trade;
    trade.code = "SHFE.ec.ec2509";
    trade.is_buy = true;
    trade.qty = 1;
    trade.price = 100.0;
    trade.mid_at_trade = 100.0;
    trade.spread_at_trade = 1.0;
    trade.timestamp = 1000000; // replay ms 基准 (V8-R3 后与 onTickUpdate 同域)
    pa.recordTrade(trade);
    ASSERT_EQ(pa.pendingAdverseCount(), 1u);

    // 31s 后来一个 tick (低流动性场景): 旧双域时钟 (actiontime ~9e7 vs 合成
    // ~2e13) 下 now > trade_timestamp 恒假, 永不强制过期; 统一后应过期清理
    pa.onTickUpdate("SHFE.ec.ec2509", 100.0, 1000000 + 31000);
    EXPECT_EQ(pa.pendingAdverseCount(), 0u);
}

//==============================================================================
// R4: TscClock
//==============================================================================

TEST(TscClock, CalibrateAndConvert)
{
    bool ok = TscClock::calibrate(); // 本机 invariant TSC 应探测成功
    EXPECT_TRUE(ok);
    EXPECT_TRUE(TscClock::calibrated());
    if (ok) {
        uint64_t t0 = TscClock::now();
        uint64_t t1 = TscClock::now();
        EXPECT_GE(t1, t0); // lfence 序列化后单调
        uint64_t ns = TscClock::toNs(1000);
        EXPECT_GT(ns, 0u);
        EXPECT_LT(ns, 100000u); // 0.05~2.0 GHz 合理区间
    }
}
