// src/WtFutuCore/VolatilitySignalSource.cpp
#include "VolatilitySignalSource.h"
#include "MarketDataContext.h"
#include "FutuConfig.h"
#include "../../WTSTools/WTSLogger.h"
#include <algorithm>

namespace futu
{

RealizedVolSignalSource::RealizedVolSignalSource()
{
    _result.type = SignalType::VOLATILITY;
}

void RealizedVolSignalSource::setWindowSize(uint32_t windowSize)
{
    _window_size = std::min(windowSize, static_cast<uint32_t>(MAX_BUFFER));
}

void RealizedVolSignalSource::update(const MarketDataContext& book)
{
    if (!_enabled)
        return;

    double mid = book.getMidPrice();
    if (mid <= 0)
        return;

    _result.timestamp = book.getTimestamp();
    if (_code.empty())
        _code = book.getCode();

    // Incremental return calculation
    if (_last_mid > 0) {
        double ret = (mid - _last_mid) / _last_mid;

        // Handle full buffer
        if (_returns.size() >= _window_size) {
            double old_ret = _returns.front();
            _running_sum -= old_ret;
            _running_sum_sq -= old_ret * old_ret;
            _returns.pop();
        }

        // Add new return
        _returns.push(ret);
        _running_sum += ret;
        _running_sum_sq += ret * ret;

        updateVolatility();
    }

    _last_mid = mid;
    _result.valid = (_returns.size() >= std::min(_window_size / 2, 5u));
}

void RealizedVolSignalSource::updateVolatility()
{
    double n = static_cast<double>(_returns.size());
    if (n < 2)
        return;

    // Variance = E[X^2] - E[X]^2
    double mean = _running_sum / n;
    double variance = (_running_sum_sq / n) - (mean * mean);

    // Bessel's correction
    variance *= n / (n - 1.0);
    _result.realized_vol = std::sqrt(std::max(0.0, variance));
    _result.composite_vol = _result.realized_vol;

    // Direct tier classification from vol thresholds (no percentile binning).
    // Only 2 boundaries matter: ELEVATED (widen) and EXTREME (pause).
    double vol = _result.realized_vol;
    if (vol >= _vol_extreme)
        _result.vol_tier = VolTier::EXTREME;
    else if (vol >= _vol_elevated)
        _result.vol_tier = VolTier::ELEVATED;
    else
        _result.vol_tier = VolTier::NORMAL;

    // Linear percentile mapping for SpreadOptimizer (sigma_sq) and ICWeightTracker (regime).
    // Maps [0, vol_extreme] -> [0, 85], caps at 100.
    _result.vol_percentile = std::min(100.0, vol / _vol_extreme * 85.0);

    // V8-S10: vol 分布统计埋点 (标定 elevated/extreme 阈值用, 默认关闭)
    if (_stats_log_interval > 0) {
        _stats_min = std::min(_stats_min, vol);
        _stats_max = std::max(_stats_max, vol);
        _stats_sum += vol;
        if (++_stats_count % _stats_log_interval == 0) {
            WTSLogger::info("[VOL_STATS] {} samples={} vol={:.6f} min={:.6f} mean={:.6f} max={:.6f}",
                            _code,
                            _stats_count,
                            vol,
                            _stats_min,
                            _stats_sum / _stats_count,
                            _stats_max);
        }
    }
}

void RealizedVolSignalSource::reset()
{
    _returns.clear();
    _running_sum = 0;
    _running_sum_sq = 0;
    _last_mid = 0;
    _result = VolatilitySignalResult();
    _result.type = SignalType::VOLATILITY;
}

} // namespace futu
