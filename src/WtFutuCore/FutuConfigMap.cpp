/*!
 * \file FutuConfigMap.cpp
 * \brief 策略配置 → 协调器配置 唯一映射点实现 (F-core P1-4.1b)
 */
#include "FutuConfigMap.h"

#include "UftFutuMmStrategy.h"   // FutuMmConfig
#include "StrategyCoordinator.h" // CoordinatorConfig

namespace futu
{

size_t FutuConfigMap::applyStrategyOverrides(CoordinatorConfig& coord, const FutuMmConfig& cfg)
{
    size_t n = 0;

    // ---- closeout 时序/开关 (策略层 config.yaml 为最终覆盖源) ----
    coord.closeout_minutes_before = cfg.closeout.minutes_before;
    ++n;
    coord.close_time = cfg.closeout.close_time;
    ++n;
    coord.closeout_flatten_position = cfg.closeout.flatten_position;
    ++n;
    coord.night_close_time = cfg.closeout.night_close_time;
    ++n;
    coord.night_minutes_before = cfg.closeout.night_minutes_before;
    ++n;

    // ---- PerformanceMonitor (P2: alertThrottleMs 属本节, 新增字段务必在此登记) ----
    coord.perf_monitor_latency_threshold = cfg.perf.monitor_latency_threshold;
    ++n;
    coord.perf_enabled = cfg.perf.enabled;
    ++n;
    coord.perf_log_interval = cfg.perf.log_interval;
    ++n;
    coord.perf_warn_threshold_ns = cfg.perf.warn_threshold_ns;
    ++n;
    coord.perf_critical_threshold_ns = cfg.perf.critical_threshold_ns;
    ++n;
    coord.perf_alert_throttle_ms = cfg.perf.alert_throttle_ms;
    ++n;

    return n;
}

} // namespace futu
