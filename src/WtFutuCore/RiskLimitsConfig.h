/*!
 * \file RiskLimitsConfig.h
 * \brief 风控限速/仓位阈值配置的单一来源 (消除 UftFutuMmStrategy::Risk 与
 *        FutuRiskMonitor::RateLimits 的重复定义)
 *
 * 字段默认值以 FutuConfigLoader 的实际生效值为准 (历史上有两套默认值,
 * 装配器最终以 _config.risk 覆盖, 故此处收敛为有效默认).
 */
#pragma once

#include <cstdint>
#include "FutuConfig.h"

namespace futu
{

/// 频率/速率/仓位/delta 阈值 (风险监控与策略配置共用)
struct RiskRateLimits
{
    uint32_t max_orders_per_sec;
    uint32_t max_cancels_per_sec;
    uint32_t max_trades_per_sec;
    double max_delta_change_per_sec;
    uint32_t delta_rate_window_sec;
    uint32_t delta_rate_cooldown_ms;

    // 分级响应阈值
    double position_breach_pause_threshold;
    double delta_critical_mult;
    double delta_warning_mult;
    double position_warning_l1;
    double position_warning_l2;
    double position_hard_block_ratio;
    uint32_t widen_threshold;

    // 同侧连续成交熔断 (per-contract side fill breaker)
    uint32_t max_consecutive_same_side;
    uint32_t same_side_window_ms;
    uint32_t same_side_pause_ms;

    RiskRateLimits()
        : max_orders_per_sec(50), max_cancels_per_sec(30), max_trades_per_sec(20), max_delta_change_per_sec(3.0),
          delta_rate_window_sec(2), delta_rate_cooldown_ms(15000), position_breach_pause_threshold(1.2),
          delta_critical_mult(1.5), delta_warning_mult(0.8), position_warning_l1(0.8), position_warning_l2(0.9),
          position_hard_block_ratio(1.0), widen_threshold(1), max_consecutive_same_side(5), same_side_window_ms(3000),
          same_side_pause_ms(5000)
    {}

    static RiskRateLimits fromVariant(wtp::WTSVariant* v)
    {
        RiskRateLimits r;
        r.max_orders_per_sec = FutuConfig::readUInt32(v, "maxOrdersPerSec", 50);
        r.max_cancels_per_sec = FutuConfig::readUInt32(v, "maxCancelsPerSec", 30);
        r.max_trades_per_sec = FutuConfig::readUInt32(v, "maxTradesPerSec", 20);
        r.max_delta_change_per_sec = FutuConfig::readDouble(v, "maxDeltaChangePerSec", 3.0);
        r.delta_rate_window_sec = FutuConfig::readUInt32(v, "deltaRateWindowSec", 2);
        r.delta_rate_cooldown_ms = FutuConfig::readUInt32(v, "deltaRateCooldownMs", 15000);
        r.position_breach_pause_threshold = FutuConfig::readDouble(v, "positionBreachPauseThreshold", 1.2);
        r.delta_critical_mult = FutuConfig::readDouble(v, "deltaCriticalMult", 1.5);
        r.delta_warning_mult = FutuConfig::readDouble(v, "deltaWarningMult", 0.8);
        r.position_warning_l1 = FutuConfig::readDouble(v, "positionWarningL1", 0.8);
        r.position_warning_l2 = FutuConfig::readDouble(v, "positionWarningL2", 0.9);
        r.position_hard_block_ratio = FutuConfig::readDouble(v, "positionHardBlockRatio", 1.0);
        r.widen_threshold = FutuConfig::readUInt32(v, "widenThreshold", 1);
        r.max_consecutive_same_side = FutuConfig::readUInt32(v, "maxConsecutiveSameSide", 5);
        r.same_side_window_ms = FutuConfig::readUInt32(v, "sameSideWindowMs", 3000);
        r.same_side_pause_ms = FutuConfig::readUInt32(v, "sameSidePauseMs", 5000);
        return r;
    }
};

} // namespace futu
