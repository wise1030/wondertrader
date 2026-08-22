// src/WtFutuCore/VolatilitySignalSource.h
#pragma once

#include "ISignalSource.h"
#include "../../Share/RingBuffer.hpp"
#include "../../Includes/WTSVariant.hpp"
#include <cmath>

namespace futu
{

class MarketDataContext;

/// Implementation of VolatilitySignalSource focusing on realized and composite vol
class RealizedVolSignalSource : public VolatilitySignalSource
{
public:
    RealizedVolSignalSource();
    virtual ~RealizedVolSignalSource() = default;

    // ISignalSource implementation
    virtual const std::string& name() const override { return _name; }
    virtual SignalType type() const override { return SignalType::VOLATILITY; }
    virtual void update(const MarketDataContext& book) override;
    virtual void reset() override;
    virtual bool enabled() const override { return _enabled; }
    virtual void setEnabled(bool enabled) override { _enabled = enabled; }
    virtual const SignalResult& result() const override { return _result; }

    // VolatilitySignalSource implementation
    virtual const VolatilitySignalResult& getVolatility() const override { return _result; }
    virtual double getVolPercentile() const override { return _result.vol_percentile; }

    // Configuration
    void setWindowSize(uint32_t windowSize);
    void setVpinWeight(double weight) { _vpin_weight = weight; }

    /// Set vol tier thresholds (direct realized_vol comparison, no percentile binning)
    void setVolThresholds(double elevated, double extreme)
    {
        _vol_elevated = elevated;
        _vol_extreme = extreme;
    }

    /// V8-S10: vol 分布统计埋点 (标定 elevated/extreme 阈值用)。
    /// interval>0 时每 interval 个有效 vol 样本输出一次 min/mean/max (info 级);
    /// 默认 0 = 关闭, 热路径零开销。报告实证 EC 常态 vol≈1.35e-4 而配置阈值
    /// 0.002/0.004 ≈ 15/30 倍常态 — ELEVATED/EXTREME 分档对 EC 不可达,
    /// 需按实测分布标定。
    void setStatsLogInterval(uint32_t interval) { _stats_log_interval = interval; }

private:
    std::string _name = "RealizedVol";
    bool _enabled = true;
    VolatilitySignalResult _result;

    // Incremental volatility calculation
    static constexpr size_t MAX_BUFFER = 256;
    RingBuffer<double, MAX_BUFFER> _returns;
    double _running_sum = 0;
    double _running_sum_sq = 0;
    uint32_t _window_size = 100;

    double _vpin_weight = 0.3;
    double _last_mid = 0;

    // Direct vol thresholds for tier classification
    // (V8-S10: 默认值同步 _ec_5d 实测标定 p95/p99.5; 旧 0.002/0.004 对 EC 不可达)
    double _vol_elevated = 0.0005; // >= this -> ELEVATED (widen spread)
    double _vol_extreme = 0.0017;  // >= this -> EXTREME  (pause quotes)

    // V8-S10: vol 分布统计 (标定用, 默认关闭)
    uint32_t _stats_log_interval = 0;
    uint32_t _stats_count = 0;
    double _stats_min = 1e30;
    double _stats_max = 0.0;
    double _stats_sum = 0.0;
    std::string _code;

    void updateVolatility();
};

} // namespace futu
