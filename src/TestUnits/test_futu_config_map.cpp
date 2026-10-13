//==============================================================================
// F-core P1-4.1b(2026-09-19): FutuConfigMap 逐字段映射完整性
//   背景: 该映射原为 11 行内联赋值, 新增跨结构字段易漏 (alertThrottleMs 需 5 处编辑,
//   perf 阈值曾"注入后无逻辑读取")。本用例把"加了键忘了桥"变成可测失败。
//==============================================================================

#include <gtest/gtest.h>

#include "../WtFutuCore/FutuConfigMap.h"
#include "../WtFutuCore/StrategyCoordinator.h"
#include "../WtFutuCore/UftFutuMmStrategy.h"

using namespace futu;

TEST(FutuConfigMap, AllStrategyOverridesAreMapped)
{
	FutuMmConfig cfg;
	// 每个被映射字段给一个可区分的非默认值
	cfg.closeout.minutes_before = 7;
	cfg.closeout.close_time = 150100;
	cfg.closeout.flatten_position = false;
	cfg.closeout.night_close_time = 230;
	cfg.closeout.night_minutes_before = 9;
	cfg.closeout.max_retries = 11;
	cfg.closeout.retry_interval_ms = 7654;
	cfg.perf.monitor_latency_threshold = 123456;
	cfg.perf.enabled = false;
	cfg.perf.log_interval = 2222;
	cfg.perf.warn_threshold_ns = 333333;
	cfg.perf.critical_threshold_ns = 444444;
	cfg.perf.alert_throttle_ms = 55555;

	CoordinatorConfig coord;
	const size_t mapped = FutuConfigMap::applyStrategyOverrides(coord, cfg);
	EXPECT_EQ(mapped, 13u); // 映射表变更时必须同步本用例

	EXPECT_EQ(coord.closeout_minutes_before, cfg.closeout.minutes_before);
	EXPECT_EQ(coord.close_time, cfg.closeout.close_time);
	EXPECT_EQ(coord.closeout_flatten_position, cfg.closeout.flatten_position);
	EXPECT_EQ(coord.night_close_time, cfg.closeout.night_close_time);
	EXPECT_EQ(coord.night_minutes_before, cfg.closeout.night_minutes_before);
	EXPECT_EQ(coord.closeout_max_retries, cfg.closeout.max_retries);
	EXPECT_EQ(coord.closeout_retry_interval_ms, cfg.closeout.retry_interval_ms);
	EXPECT_EQ(coord.perf_monitor_latency_threshold, cfg.perf.monitor_latency_threshold);
	EXPECT_EQ(coord.perf_enabled, cfg.perf.enabled);
	EXPECT_EQ(coord.perf_log_interval, cfg.perf.log_interval);
	EXPECT_EQ(coord.perf_warn_threshold_ns, cfg.perf.warn_threshold_ns);
	EXPECT_EQ(coord.perf_critical_threshold_ns, cfg.perf.critical_threshold_ns);
	EXPECT_EQ(coord.perf_alert_throttle_ms, cfg.perf.alert_throttle_ms);
}

// 覆盖语义: 只改策略层给出的字段, 其余 coordinator 字段保持原值 (不做整结构重置)
TEST(FutuConfigMap, NonMappedFieldsKeepCoordinatorValues)
{
	CoordinatorConfig coord;
	coord.bilateral_stats_log_interval_sec = 300;
	coord.modules.zombie_latch_max_hold_ms = 120000;

	FutuMmConfig cfg;
	FutuConfigMap::applyStrategyOverrides(coord, cfg);

	EXPECT_EQ(coord.bilateral_stats_log_interval_sec, 300u);
	EXPECT_EQ(coord.modules.zombie_latch_max_hold_ms, 120000u);
}
