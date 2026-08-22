/*!
 * \file StatisticalArbStrategy.cpp
 * \brief Statistical Arbitrage Strategy Implementation
 */
#include "StatisticalArbStrategy.h"
#include <cmath>
#include <algorithm>

namespace futu
{

StatisticalArbStrategy::StatisticalArbStrategy()
    : _last_update(0), _prev_zscore(0), _prev_correlation(0), _last_signal_time(0), _entry_signal(0)
{}

void StatisticalArbStrategy::updateState(const SpreadState& state, uint64_t timestamp)
{
    _prev_zscore = _features.zscore;
    _prev_correlation = _features.correlation_trend;

    _zscore_history.push(state.zscore);

    // Estimate volatility from spread
    double volatility = state.spread_std;
    _volatility_history.push(volatility);

    _correlation_history.push(state.correlation);

    _last_update = timestamp;

    // Calculate new features
    _features = calculateFeatures(state);
    _signal_history.push(_features.composite_signal);
}

StatisticalFeatures StatisticalArbStrategy::calculateFeatures(const SpreadState& state)
{
    StatisticalFeatures feat;

    if (_zscore_history.size() < _config.min_samples) {
        feat.is_valid = false;
        return feat;
    }

    feat.zscore = calculateZScoreFeature(state);
    feat.zscore_momentum = calculateMomentumFeature(state);
    feat.volatility_ratio = calculateVolatilityFeature(state);
    feat.correlation_trend = calculateCorrelationFeature(state);

    // Calculate composite signal using weights
    // V8-A3: mspread/volume_imbalance 死因子已删 (输入从未填充, 见 .h 注释)
    // V8-R3: 自适应权重链 (recordOutcome/updateAdaptiveWeights) 零调用已删 --
    // adaptive 权重恒等于初始值 (与静态权重同值), 此处行为零变化
    double w_z = _config.weight_zscore;
    double w_m = _config.weight_momentum;
    double w_v = _config.weight_volatility;
    double w_c = _config.weight_correlation;

    // Normalize weights
    double w_sum = w_z + w_m + w_v + w_c;
    if (w_sum > 0) {
        w_z /= w_sum;
        w_m /= w_sum;
        w_v /= w_sum;
        w_c /= w_sum;
    }

    // Composite signal
    feat.composite_signal = w_z * feat.zscore + w_m * feat.zscore_momentum + w_v * (feat.volatility_ratio - 1.0) +
                            w_c * feat.correlation_trend;

    // Clamp to [-1, 1]
    feat.composite_signal = std::max(-1.0, std::min(1.0, feat.composite_signal));

    // Calculate confidence based on feature stability
    feat.feature_stability = std::max(0.0, std::min(1.0, 1.0 - std::abs(feat.zscore_momentum) * 0.5));
    feat.signal_confidence = std::abs(feat.composite_signal) * feat.feature_stability;

    feat.is_valid = true;
    return feat;
}

double StatisticalArbStrategy::calculateZScoreFeature(const SpreadState& state) const
{
    // Normalize Z-Score to [-1, 1] range
    double normalized = state.zscore / 3.0; // 3 sigma covers most of distribution
    return std::max(-1.0, std::min(1.0, normalized));
}

double StatisticalArbStrategy::calculateMomentumFeature(const SpreadState& state) const
{
    if (_zscore_history.size() < 10)
        return 0;

    // Calculate Z-Score momentum
    size_t n = _zscore_history.size();
    double recent = 0, older = 0;

    // Average of last 5 vs previous 5
    for (size_t i = n - 5; i < n; ++i)
        recent += _zscore_history[i];
    for (size_t i = n - 10; i < n - 5; ++i)
        older += _zscore_history[i];

    recent /= 5;
    older /= 5;

    double momentum = recent - older;
    return std::max(-1.0, std::min(1.0, momentum / 2.0));
}

double StatisticalArbStrategy::calculateVolatilityFeature(const SpreadState& state) const
{
    if (_volatility_history.size() < 10)
        return 1.0;

    // Calculate volatility ratio (recent vs historical)
    size_t n = _volatility_history.size();
    double recent_vol = 0, hist_vol = 0;

    for (size_t i = n - 10; i < n; ++i)
        recent_vol += _volatility_history[i];
    for (size_t i = 0; i < n - 10; ++i)
        hist_vol += _volatility_history[i];

    recent_vol /= 10;
    hist_vol /= (n - 10);

    // V8-A11: n==10 时 hist 段为空 (0/0=NaN), `NaN < 1e-10` 恒 false 会滑过守卫
    // -> composite 钳位成 +1.0 最强做空信号。!(x>=eps) 形式同时拦截 NaN。
    if (!(hist_vol >= 1e-10))
        return 1.0;

    return recent_vol / hist_vol;
}

double StatisticalArbStrategy::calculateCorrelationFeature(const SpreadState& state) const
{
    if (_correlation_history.size() < 10)
        return 0;

    // Calculate correlation trend
    size_t n = _correlation_history.size();
    double recent = 0, older = 0;

    for (size_t i = n - 5; i < n; ++i)
        recent += _correlation_history[i];
    for (size_t i = n - 10; i < n - 5; ++i)
        older += _correlation_history[i];

    recent /= 5;
    older /= 5;

    // Correlation trend (increasing correlation = positive)
    double trend = recent - older;
    return std::max(-1.0, std::min(1.0, trend * 5.0));
}

// V8-A3: calculateMSpreadFeature 已删除 -- SpreadState 微结构字段
// (mid/bid/ask/total_volume/average_trade_size) 全链路从未填充, 恒返回 0

SpreadSignal StatisticalArbStrategy::generateSignal(const SpreadState& state, uint64_t current_time)
{
    SpreadSignal signal;
    signal.pair_id = state.pair_id;
    signal.source = ArbitrageStrategy::STATISTICAL_ARB;
    signal.timestamp = current_time;

    if (!_features.is_valid) {
        signal.type = SpreadSignalType::NONE;
        return signal;
    }

    double sig = _features.composite_signal;
    double abs_sig = std::abs(sig);

    // No position - check for entry
    if (!state.hasPosition()) {
        if (sig > _config.entry_threshold) {
            signal.type = SpreadSignalType::OPEN_SHORT_SPREAD;
            signal.confidence = calculateConfidence(_features);
            signal.suggested_size = calculatePositionSize(_features);
            signal.entry_zscore = state.zscore;
            signal.reason = "Statistical signal: composite above threshold";
            _entry_signal = sig;
        } else if (sig < -_config.entry_threshold) {
            signal.type = SpreadSignalType::OPEN_LONG_SPREAD;
            signal.confidence = calculateConfidence(_features);
            signal.suggested_size = calculatePositionSize(_features);
            signal.entry_zscore = state.zscore;
            signal.reason = "Statistical signal: composite below threshold";
            _entry_signal = sig;
        }
    }
    // Has position - check for exit
    else {
        // Stop loss
        if (abs_sig > _config.stop_loss_threshold) {
            signal.type = SpreadSignalType::STOP_LOSS;
            signal.confidence = 1.0;
            signal.suggested_size = std::abs(state.spread_position);
            signal.reason = "Statistical signal extreme, stop loss";
        }
        // Timeout
        else if (state.positionDuration(current_time) > _config.convergence_timeout) {
            signal.type = SpreadSignalType::TIMEOUT_EXIT;
            signal.confidence = 0.8;
            signal.suggested_size = std::abs(state.spread_position);
            signal.reason = "Timeout: statistical signal did not converge";
        }
        // Normal exit
        else if (state.spread_position > 0 && sig > -_config.exit_threshold) {
            signal.type = SpreadSignalType::CLOSE_LONG_SPREAD;
            signal.confidence = 0.9;
            signal.suggested_size = state.spread_position;
            signal.reason = "Statistical signal normalized, closing position";
        } else if (state.spread_position < 0 && sig < _config.exit_threshold) {
            signal.type = SpreadSignalType::CLOSE_SHORT_SPREAD;
            signal.confidence = 0.9;
            signal.suggested_size = std::abs(state.spread_position);
            signal.reason = "Statistical signal normalized, closing position";
        }
    }

    _last_signal_time = current_time;
    return signal;
}

double StatisticalArbStrategy::calculatePositionSize(const StatisticalFeatures& features) const
{
    double signal_strength = std::abs(features.composite_signal);
    double size = _config.base_qty * (0.7 + 0.6 * signal_strength);
    return std::min(size, _config.max_position);
}

double StatisticalArbStrategy::calculateConfidence(const StatisticalFeatures& features) const
{
    return features.signal_confidence;
}

void StatisticalArbStrategy::reset()
{
    _zscore_history.clear();
    _volatility_history.clear();
    _correlation_history.clear();
    _signal_history.clear();
    _features = StatisticalFeatures();
    _prev_zscore = 0;
    _prev_correlation = 0;
    _last_signal_time = 0;
    _entry_signal = 0;
}

} // namespace futu
