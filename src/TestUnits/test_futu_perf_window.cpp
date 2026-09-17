//==============================================================================
// F-core P2(2026-09-17): PerformanceMonitor 告警窗口化 / EPISODE 计数 / 日志限频
//
// 背景: 原实现按 16384 样本累计环计算 p99 并在每秒限频下重复打印, 09-17 实测 15448 行/日
//   (11:30 与 13:30 的 p99 完全相同 = 2806.2us, 午休不衰减, 重启才归零)。
// 方案 17.1: 判定样本改为"窗口环"(窗口 = 60s 汇总周期), 结窗累计 EPISODE 并清空窗口;
//   日志按通道限频, 但"是否 critical"的返回值不受限频影响 (调用方语义不变)。
//==============================================================================

#include <gtest/gtest.h>
#include <string>

#include "../WtFutuCore/PerformanceMonitor.h"

using namespace futu;

namespace
{
void feedTickToQuote(PerformanceMonitor& pm, uint64_t ns, int n)
{
	for (int i = 0; i < n; ++i)
		pm.recordTickToQuote(ns);
}
} // namespace

// 结窗: 窗口 p99 越 critical ⇒ crit_ep 计 1; KPI 报"最近已结窗"读数与样本数 (窗口随后清空)
TEST(FutuPerfWindow, CriticalEpisodeCountedAndWindowCleared)
{
	PerformanceMonitor pm;
	pm.setWarnThresholdNs(1000);     // 1us
	pm.setCriticalThresholdNs(2000); // 2us

	feedTickToQuote(pm, 5000, 200); // 5us > critical
	pm.evaluateAndResetWindow();

	const std::string kpi = pm.formatKpi();
	EXPECT_NE(kpi.find("crit_ep=1/0"), std::string::npos) << kpi;
	// 结窗后窗口已清空, KPI 必须报刚关闭窗口的读数 (5.0us) 而不是空窗口的 0.0us
	EXPECT_NE(kpi.find("t2q_p99=5.0us"), std::string::npos) << kpi;
	EXPECT_NE(kpi.find("n=200/0"), std::string::npos) << kpi;
}

// 分级: >warn 但 <critical ⇒ warn_ep; 低于阈值 / 样本 <100 均不计数
TEST(FutuPerfWindow, WarnEpisodeAndGuards)
{
	PerformanceMonitor pm;
	pm.setWarnThresholdNs(1000);
	pm.setCriticalThresholdNs(100000);

	feedTickToQuote(pm, 1500, 200); // 1.5us: warn 区间
	pm.evaluateAndResetWindow();
	EXPECT_NE(pm.formatKpi().find("warn_ep=1/0"), std::string::npos) << pm.formatKpi();

	feedTickToQuote(pm, 300, 200); // 低于 warn ⇒ 不计数
	pm.evaluateAndResetWindow();
	EXPECT_NE(pm.formatKpi().find("warn_ep=1/0"), std::string::npos) << pm.formatKpi();

	feedTickToQuote(pm, 9000, 50); // 样本不足 100 ⇒ 不计数 (启动期/稀疏窗口)
	pm.evaluateAndResetWindow();
	EXPECT_NE(pm.formatKpi().find("crit_ep=0/0"), std::string::npos) << pm.formatKpi();
}

// 限频只影响日志输出, 不影响 checkThresholds 的返回值 (调用方语义不变)
TEST(FutuPerfWindow, CriticalFlagIndependentOfLogThrottle)
{
	PerformanceMonitor pm;
	pm.setWarnThresholdNs(1000);
	pm.setCriticalThresholdNs(2000);
	pm.setAlertThrottleMs(600000); // 10min 日志限频

	feedTickToQuote(pm, 5000, 200);
	EXPECT_TRUE(pm.checkThresholds(1000000));
	EXPECT_TRUE(pm.checkThresholds(1001000)); // 日志被限频, 判定仍为 critical
	EXPECT_TRUE(pm.checkThresholds(1002000));
}

// 历史最大窗口 p99 记录 (用于巡检 KPI 观察真实劣化)
TEST(FutuPerfWindow, MaxP99Tracked)
{
	PerformanceMonitor pm;
	pm.setWarnThresholdNs(1000);
	pm.setCriticalThresholdNs(2000);

	feedTickToQuote(pm, 3000, 200);
	pm.evaluateAndResetWindow();
	feedTickToQuote(pm, 7000, 200);
	pm.evaluateAndResetWindow();

	const std::string kpi = pm.formatKpi();
	EXPECT_NE(kpi.find("max_p99=7.0/0.0us"), std::string::npos) << kpi;
	EXPECT_NE(kpi.find("crit_ep=2/0"), std::string::npos) << kpi;
}
