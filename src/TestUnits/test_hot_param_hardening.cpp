/*!
 * \file test_hot_param_hardening.cpp
 * \brief 热参数加固 (2026-08-24) unit tests
 *
 * 背景: 26 个热参数稳态权威是 hotparams.yaml (实盘 ~1s 覆盖 config/coordinator 同名键;
 * 回测不跑 watcher 热参不生效)。三处误配风险:
 *   ① 边界表宽于 loader error 级 (base_spread{0,1000} vs loader (0,20] 等);
 *   ② 交叉校验只跑启动期一次, 热更路径零复查;
 *   ③ hotparams 与 config 漂移无感知 (回测/实盘参数分叉)。
 *
 * 加固后契约:
 *   - 边界表对齐 FutuConfigLoader error 级 / FutuConfigValidator 口径
 *   - applyAll 末尾 crossCheckIssues 交叉复查 (warn 级)
 *   - 启动期 collectDriftLines/logDriftSummary 漂移摘要 (回测也打印)
 */
#include "../WtFutuCore/FutuHotParamManager.h"
#include "gtest/gtest.h"

#include <fstream>
#include <string>
#include <vector>

using namespace futu;

namespace
{

// 写临时 yaml 并返回路径
std::string writeTmpYaml(const char* content)
{
    static int seq = 0;
    std::string path = "/tmp/opencode/hp_hardening_" + std::to_string(++seq) + ".yaml";
    std::ofstream f(path);
    f << content;
    return path;
}

FutuHotParamManager::HotCrossCheckInput makeDefaultInput()
{
    FutuHotParamManager::HotCrossCheckInput in{};
    // 权重和=1.0
    in.ofi_weight = 0.35;
    in.trade_weight = 0.25;
    in.book_imbalance_weight = 0.20;
    in.momentum_weight = 0.15;
    in.lead_lag_weight = 0.05;
    // 挂单结构: numLevels=2, L1=义务层 → depth = scout(1) + base_qty(10) = 11 ≥ obligationMinQty(10)
    in.base_qty = 10.0;
    in.level_qty_multiplier = 0.7;
    in.num_levels = 2;
    in.obligation_level = 1;
    in.scout_qty = 1.0;
    in.obligation_min_qty = 10.0;
    // 软限 vs 硬顶 / GLFT 区间
    in.portfolio_max_delta = 30.0;
    in.contract_max_positions = nullptr;
    in.max_spread_mult = 3.0;
    in.min_spread_mult = 1.0;
    in.confidence_weight_min = 0.3;
    in.confidence_weight_max = 1.0;
    return in;
}

} // namespace

//==========================================================================
// ① 边界表收紧: 与 loader/validator 对齐
//==========================================================================

TEST(HotParamBounds, BaseSpreadAlignedWithLoader)
{
    std::vector<std::pair<uint32_t, double>> out;
    // 旧表 {0,1000} 会放行的值, 新表 [0.5,20] 拒收
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("base_spread: 30\n").c_str(), out));
    EXPECT_TRUE(out.empty());
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("base_spread: 0.4\n").c_str(), out));
    EXPECT_TRUE(out.empty());
    // 合法值放行
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("base_spread: 3.5\n").c_str(), out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_DOUBLE_EQ(out[0].second, 3.5);
}

TEST(HotParamBounds, ProtectTicksRejectsZero)
{
    std::vector<std::pair<uint32_t, double>> out;
    // 0 = 静默关闭价格软保护 (防误配拒收; 关闭应走 price_protection 开关)
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("protect_ticks: 0\n").c_str(), out));
    EXPECT_TRUE(out.empty());
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("protect_ticks: 0.3\n").c_str(), out));
    EXPECT_TRUE(out.empty());
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("protect_ticks: 0.5\n").c_str(), out));
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("protect_ticks: 1.0\n").c_str(), out));
    ASSERT_EQ(out.size(), 1u);
}

TEST(HotParamBounds, StickyThresholdRejectsZero)
{
    std::vector<std::pair<uint32_t, double>> out;
    // 0 = threshold×tick_size=0 → 任何价格变化都重挂 (churn 风暴)
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("sticky_threshold: 0\n").c_str(), out));
    EXPECT_TRUE(out.empty());
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("sticky_threshold: 0.02\n").c_str(), out));
    ASSERT_EQ(out.size(), 1u);
}

TEST(HotParamBounds, PhiDomainExtendedToUpper)
{
    std::vector<std::pair<uint32_t, double>> out;
    // validator 口径 [0.01, 2.0]: 旧表上界 1.0 过紧, 1.5 现在合法; 下界同步收紧
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("phi: 1.5\n").c_str(), out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_DOUBLE_EQ(out[0].second, 1.5);
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("phi: 0.005\n").c_str(), out));
    EXPECT_TRUE(out.empty());
}

TEST(HotParamBounds, MaxDeltaRejectsZeroAndHuge)
{
    std::vector<std::pair<uint32_t, double>> out;
    // 0 = 静默关组合 skew + WIDEN util 分母; >1e8 超 loader 上限
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("max_delta: 0\n").c_str(), out));
    EXPECT_TRUE(out.empty());
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("max_delta: 1000000000\n").c_str(), out));
    EXPECT_TRUE(out.empty());
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("max_delta: 50\n").c_str(), out));
    ASSERT_EQ(out.size(), 1u);
}

TEST(HotParamBounds, DeltaSkewThresholdValidatorRange)
{
    std::vector<std::pair<uint32_t, double>> out;
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("delta_skew_threshold: 0.95\n").c_str(), out));
    EXPECT_TRUE(out.empty());
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("delta_skew_threshold: 0.9\n").c_str(), out));
    ASSERT_EQ(out.size(), 1u);
}

TEST(HotParamBounds, LevelStepAndBaseQtyLoaderRanges)
{
    std::vector<std::pair<uint32_t, double>> out;
    // level_step loader error (0,100]; base_qty loader error (0,100]
    ASSERT_TRUE(FutuHotParamManager::parseHotParamFile(writeTmpYaml("level_step: 0\nlevel_step: 200\nbase_qty: 150\n").c_str(), out));
    EXPECT_TRUE(out.empty());
}

//==========================================================================
// ② crossCheckIssues 交叉复查五检查项
//==========================================================================

TEST(HotParamCrossCheck, DefaultConfigPassesClean)
{
    auto issues = FutuHotParamManager::crossCheckIssues(makeDefaultInput());
    EXPECT_TRUE(issues.empty()) << (issues.empty() ? "" : issues[0]);
}

TEST(HotParamCrossCheck, DetectsWeightSumDrift)
{
    auto in = makeDefaultInput();
    in.ofi_weight = 0.15; // sum = 0.8, 偏离 >0.1
    auto issues = FutuHotParamManager::crossCheckIssues(in);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_NE(issues[0].find("weights sum"), std::string::npos);
}

TEST(HotParamCrossCheck, DetectsInsufficientSideDepth)
{
    auto in = makeDefaultInput();
    in.base_qty = 4.0; // depth = scout(1) + 4 = 5 < obligationMinQty(10)
    auto issues = FutuHotParamManager::crossCheckIssues(in);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_NE(issues[0].find("full-side depth"), std::string::npos);
}

TEST(HotParamCrossCheck, DetectsSoftLimitAboveHardCap)
{
    auto in = makeDefaultInput();
    std::vector<double> positions = {50.0, 60.0};
    in.contract_max_positions = &positions;
    in.portfolio_max_delta = 80.0; // > 任一合约 maxPosition
    auto issues = FutuHotParamManager::crossCheckIssues(in);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_NE(issues[0].find("portfolio_max_delta"), std::string::npos);

    // 恰好等于硬顶不算违规 (严格大于): 全部合约 maxPosition ≥ 软限时通过
    positions.clear();
    positions.push_back(60.0);
    in.portfolio_max_delta = 60.0;
    issues = FutuHotParamManager::crossCheckIssues(in);
    EXPECT_TRUE(issues.empty());

    // 未提供合约硬顶列表 (null): 跳过该检查
    in.portfolio_max_delta = 999.0;
    in.contract_max_positions = nullptr;
    issues = FutuHotParamManager::crossCheckIssues(in);
    EXPECT_TRUE(issues.empty());
}

TEST(HotParamCrossCheck, DetectsGlftIntervalInversions)
{
    auto in = makeDefaultInput();
    in.min_spread_mult = 4.0; // > max_spread_mult(3.0)
    auto issues = FutuHotParamManager::crossCheckIssues(in);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_NE(issues[0].find("clamp"), std::string::npos);

    in = makeDefaultInput();
    in.confidence_weight_min = 0.3;
    in.confidence_weight_max = 0.2; // min > max 反向
    issues = FutuHotParamManager::crossCheckIssues(in);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_NE(issues[0].find("插值反向"), std::string::npos);
}

TEST(HotParamCrossCheck, AggregatesMultipleIssues)
{
    auto in = makeDefaultInput();
    in.ofi_weight = 10.0;      // sum=10.75 偏离
    in.min_spread_mult = 5.0;  // 区间倒置
    auto issues = FutuHotParamManager::crossCheckIssues(in);
    EXPECT_EQ(issues.size(), 2u);
}

//==========================================================================
// ③ collectDriftLines 启动漂移检测
//==========================================================================

TEST(HotParamDrift, NoDiffReturnsZero)
{
    double defaults[HP_COUNT] = {};
    defaults[HP_BASE_SPREAD] = 2.0;
    defaults[HP_BASE_QTY] = 10.0;

    std::vector<std::string> lines;
    int32_t n = FutuHotParamManager::collectDriftLines(
        writeTmpYaml("base_spread: 2.0\nbase_qty: 10\n").c_str(), defaults, lines);
    EXPECT_EQ(n, 0);
    EXPECT_TRUE(lines.empty());
}

TEST(HotParamDrift, DiffReportsKeysInOrder)
{
    double defaults[HP_COUNT] = {};
    defaults[HP_BASE_SPREAD] = 2.0;
    defaults[HP_BASE_QTY] = 10.0;

    std::vector<std::string> lines;
    int32_t n = FutuHotParamManager::collectDriftLines(
        writeTmpYaml("base_spread: 3.5\nbase_qty: 10\nphi: 0.2\n").c_str(), defaults, lines);
    // phi 未在 defaults 数组中显式赋值 (0.0 != 0.2) 也算差异
    ASSERT_EQ(n, 2);
    EXPECT_NE(lines[0].find("'base_spread'"), std::string::npos);
    EXPECT_NE(lines[0].find("config_default=2.0000"), std::string::npos);
    EXPECT_NE(lines[0].find("hotparams=3.5000"), std::string::npos);
    EXPECT_NE(lines[1].find("'phi'"), std::string::npos);
}

TEST(HotParamDrift, UnloadableFileReturnsMinusOne)
{
    double defaults[HP_COUNT] = {};
    std::vector<std::string> lines;
    EXPECT_EQ(
        FutuHotParamManager::collectDriftLines("/tmp/opencode/__no_such_drift__.yaml", defaults, lines), -1);
    EXPECT_TRUE(lines.empty());
}
