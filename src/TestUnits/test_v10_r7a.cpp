/*!
 * \file test_v10_r7a.cpp
 * \brief V10 诊断报告 R7-a 热修包回归测试
 *
 * 覆盖:
 *   - P1-1a: SpreadRiskManager 告警 warmup 门控 (is_active=false 预热期零告警)
 *   - P1-1b: arbAlertThrottleWindowMs 分级节流窗口 (WARNING 60s / CRITICAL 10s / 其余不节流)
 *   - P2-15: Source::RISK_REDUCE 与 CLOSEOUT 枚举值独立 (回归保护, 防枚举复用)
 *
 * 说明: taker 冷却戳(RiskCoordinator)/pending_cancel 宽限计数(OrderRouter)/
 * settle 静止门禁(CloseoutExecutor) 无 ctx mock 接缝, 验证靠编译+既有全量+
 * configbt_v5 冒烟 (见 AGENTS.md R7-a 方案条目)。
 */
#include "../WtFutuCore/arb/SpreadRiskManager.h"
#include "../WtFutuCore/RiskLiquidator.h"
#include "../WtFutuCore/FutuConfig.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "gtest/gtest/gtest.h"

#include <fstream>
#include <string>

using namespace futu;

namespace
{

std::string writeTmpYaml(const char* content)
{
    static int seq = 0;
    std::string path = "/tmp/opencode/v10r7a_test_" + std::to_string(++seq) + ".yaml";
    std::ofstream f(path);
    f << content;
    return path;
}

SpreadState makeState(const char* pair_id, bool is_active, double correlation, double zscore)
{
    SpreadState st;
    st.pair_id = pair_id;
    st.leg1_code = "SHFE.ao.ao2609";
    st.leg2_code = "SHFE.ao.ao2610";
    st.is_active = is_active;
    st.correlation = correlation;
    st.zscore = zscore;
    return st;
}

bool hasAlert(const std::vector<RiskAlert>& alerts, RiskAlert::Type type)
{
    for (const auto& a : alerts)
        if (a.type == type)
            return true;
    return false;
}

} // namespace

// === V10-P1-1a: 告警 warmup 门控 ===

TEST(test_v10_r7a, WarmupNoCorrelationBreakAlert)
{
    SpreadRiskManager rm;
    // 预热期: is_active=false (calculator 样本不足), correlation 恒 0 < max_correlation_break(0.3)
    rm.updatePairState("p1", makeState("p1", false, 0.0, 0.0));
    auto alerts = rm.generateAlerts();
    EXPECT_FALSE(hasAlert(alerts, RiskAlert::Type::CORRELATION_BREAK))
        << "warmup 期 correlation=0 触发告警 = 08-26 刷屏事故根因";
}

TEST(test_v10_r7a, ActiveCorrelationBreakStillFires)
{
    SpreadRiskManager rm;
    rm.updatePairState("p1", makeState("p1", true, 0.0, 0.0));
    auto alerts = rm.generateAlerts();
    EXPECT_TRUE(hasAlert(alerts, RiskAlert::Type::CORRELATION_BREAK));
}

TEST(test_v10_r7a, WarmupNoDivergenceAlert)
{
    SpreadRiskManager rm;
    // 预热期 zscore 数值无统计意义 (陈旧/0), 不应触发 DIVERGENCE
    rm.updatePairState("p1", makeState("p1", false, 0.9, 10.0));
    auto alerts = rm.generateAlerts();
    EXPECT_FALSE(hasAlert(alerts, RiskAlert::Type::DIVERGENCE));
}

TEST(test_v10_r7a, ActiveDivergenceStillFires)
{
    SpreadRiskManager rm;
    // |z|=10 > 5.0*0.8 且 is_active -> 必须保留告警能力
    rm.updatePairState("p1", makeState("p1", true, 0.9, 10.0));
    auto alerts = rm.generateAlerts();
    EXPECT_TRUE(hasAlert(alerts, RiskAlert::Type::DIVERGENCE));
}

TEST(test_v10_r7a, PortfolioStopLossUnaffectedByWarmupGate)
{
    SpreadRiskManager rm;
    // 组合级 STOP_LOSS 与 pair is_active 无关, 抽测防误伤。
    // 注: 告警表只在 updatePairState 内重建 (生产链路每 tick 同步喂两者), 故此处
    // 先更新 PnL 再经任意 pair state 触发 updateAlerts。
    rm.updatePortfolioPnL(-40000.0, -5000.0); // drawdown 45k > 50k*0.8 -> CRITICAL STOP_LOSS
    rm.updatePairState("p1", makeState("p1", true, 0.9, 0.0));
    auto alerts = rm.generateAlerts();
    ASSERT_FALSE(alerts.empty());
    bool found = false;
    for (const auto& a : alerts) {
        if (a.type == RiskAlert::Type::STOP_LOSS) {
            found = true;
            EXPECT_EQ(a.level, RiskAlert::Level::CRITICAL);
        }
    }
    EXPECT_TRUE(found);
}

// === V10-P1-1b: 分级节流窗口 ===

TEST(test_v10_r7a, AlertThrottleWindowByLevel)
{
    EXPECT_EQ(arbAlertThrottleWindowMs(RiskAlert::Level::WARNING), 60000u);
    EXPECT_EQ(arbAlertThrottleWindowMs(RiskAlert::Level::CRITICAL), 10000u);
    EXPECT_EQ(arbAlertThrottleWindowMs(RiskAlert::Level::EMERGENCY), 0u);
    EXPECT_EQ(arbAlertThrottleWindowMs(RiskAlert::Level::INFO), 0u);
}

// === V10-P2-15: RISK_REDUCE 口径独立 (枚举值回归保护) ===

TEST(test_v10_r7a, RiskReduceDistinctFromOtherSources)
{
    // 防退化: 强平/taker 减仓统一走 RISK_REDUCE, 不得回退到 CLOSEOUT 冒用
    EXPECT_NE(static_cast<int>(Source::RISK_REDUCE), static_cast<int>(Source::CLOSEOUT));
    EXPECT_NE(static_cast<int>(Source::RISK_REDUCE), static_cast<int>(Source::HEDGING));
    EXPECT_NE(static_cast<int>(Source::RISK_REDUCE), static_cast<int>(Source::ARBITRAGE));
}

// === V10-P2-18: loader finite 守卫 (NaN 穿透修复) ===

TEST(test_v10_r7a, ReadDoubleRejectsNonFinite)
{
    std::string yaml = writeTmpYaml(
        "root:\n"
        "  nanVal: .nan\n"
        "  infVal: .inf\n"
        "  negInfVal: -.inf\n"
        "  okVal: 3.5\n");
    wtp::WTSVariant* cfg = WTSCfgLoader::load_from_file(yaml.c_str());
    ASSERT_NE(cfg, nullptr);
    wtp::WTSVariant* root = cfg->get("root");
    ASSERT_NE(root, nullptr);
    // NaN/±Inf 一律回落默认值 (旧实现 `x<=0||x>max` 双向比较对 NaN 恒 false 全放行)
    EXPECT_DOUBLE_EQ(FutuConfig::readDouble(root, "nanVal", 7.0), 7.0);
    EXPECT_DOUBLE_EQ(FutuConfig::readDouble(root, "infVal", 7.0), 7.0);
    EXPECT_DOUBLE_EQ(FutuConfig::readDouble(root, "negInfVal", 7.0), 7.0);
    EXPECT_DOUBLE_EQ(FutuConfig::readDouble(root, "okVal", 7.0), 3.5);
    cfg->release();
}
