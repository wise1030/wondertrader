/*!
 * \file StatisticalArbStrategy.cpp
 * \brief Statistical Arbitrage Strategy Implementation
 */
#include "StatisticalArbStrategy.h"
#include <cmath>
#include <algorithm>

namespace futu {

StatisticalArbStrategy::StatisticalArbStrategy()
    : _last_update(0)
    , _prev_zscore(0)
    , _prev_correlation(0)
    , _adaptive_weight_zscore(0.30)
    , _adaptive_weight_momentum(0.20)
    , _adaptive_weight_volatility(0.15)
    , _adaptive_weight_correlation(0.20)
    , _adaptive_weight_mspread(0.15)
    , _last_signal_time(0)
    , _entry_signal(0)
{
}

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
    
    if (_zscore_history.size() < _config.min_samples)
    {
        feat.is_valid = false;
        return feat;
    }
    
    feat.zscore = calculateZScoreFeature(state);
    feat.zscore_momentum = calculateMomentumFeature(state);
    feat.volatility_ratio = calculateVolatilityFeature(state);
    feat.correlation_trend = calculateCorrelationFeature(state);
    feat.mspread_imbalance = calculateMSpreadFeature(state);
    feat.volume_imbalance = (state.total_volume > 0) ? std::max(-1.0, std::min(1.0, (state.buy_volume - state.sell_volume) / state.total_volume)) : 0;
    
    // Calculate composite signal using weights
    double w_z = _config.use_adaptive_weights ? _adaptive_weight_zscore : _config.weight_zscore;
    double w_m = _config.use_adaptive_weights ? _adaptive_weight_momentum : _config.weight_momentum;
    double w_v = _config.use_adaptive_weights ? _adaptive_weight_volatility : _config.weight_volatility;
    double w_c = _config.use_adaptive_weights ? _adaptive_weight_correlation : _config.weight_correlation;
    double w_s = _config.use_adaptive_weights ? _adaptive_weight_mspread : _config.weight_mspread;
    
    // Normalize weights
    double w_sum = w_z + w_m + w_v + w_c + w_s;
    if (w_sum > 0)
    {
        w_z /= w_sum;
        w_m /= w_sum;
        w_v /= w_sum;
        w_c /= w_sum;
        w_s /= w_sum;
    }
    
    // Composite signal
    feat.composite_signal = 
        w_z * feat.zscore +
        w_m * feat.zscore_momentum +
        w_v * (feat.volatility_ratio - 1.0) +
        w_c * feat.correlation_trend +
        w_s * feat.mspread_imbalance;
    
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
    double normalized = state.zscore / 3.0;  // 3 sigma covers most of distribution
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
    
    if (hist_vol < 1e-10)
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

double StatisticalArbStrategy::calculateMSpreadFeature(const SpreadState& state) const
{
    if (state.mid_price <= 0) return 0;
    
    double raw_spread = state.ask_price - state.bid_price;
    double relative_spread = raw_spread / state.mid_price;
    
    double volume_weight = 1.0;
    if (state.total_volume > 0 && state.average_trade_size > 0)
    {
        double trade_intensity = state.total_volume / state.average_trade_size;
        volume_weight = std::min(1.0 + trade_intensity * 0.01, 2.0);
    }
    
    double mspread = relative_spread * volume_weight;
    return std::max(-1.0, std::min(1.0, mspread * 100.0));
}

void StatisticalArbStrategy::updateAdaptiveWeights()
{
    // Adjust weights based on feature performance
    if (_performance.sample_count < 10)
        return;
    
    const double learning_rate = 0.1;
    const double min_weight = 0.05;
    const double max_weight = 0.50;
    
    // Increase weight for features that have performed well
    _adaptive_weight_zscore = std::min(max_weight,
        _adaptive_weight_zscore + learning_rate * _performance.zscore_return);
    _adaptive_weight_zscore = std::max(min_weight, _adaptive_weight_zscore);
    
    _adaptive_weight_momentum = std::min(max_weight,
        _adaptive_weight_momentum + learning_rate * _performance.momentum_return);
    _adaptive_weight_momentum = std::max(min_weight, _adaptive_weight_momentum);
    
    _adaptive_weight_volatility = std::min(max_weight,
        _adaptive_weight_volatility + learning_rate * _performance.volatility_return);
    _adaptive_weight_volatility = std::max(min_weight, _adaptive_weight_volatility);
    
    _adaptive_weight_correlation = std::min(max_weight,
        _adaptive_weight_correlation + learning_rate * _performance.correlation_return);
    _adaptive_weight_correlation = std::max(min_weight, _adaptive_weight_correlation);
    
    _adaptive_weight_mspread = std::min(max_weight,
        _adaptive_weight_mspread + learning_rate * _performance.mspread_return);
    _adaptive_weight_mspread = std::max(min_weight, _adaptive_weight_mspread);
}

void StatisticalArbStrategy::recordOutcome(double pnl, const StatisticalFeatures& features)
{
    // Update performance tracking for adaptive weights
    double scaled_pnl = pnl / (_config.base_qty + 1);
    
    _performance.zscore_return = 0.9 * _performance.zscore_return + 
                                  0.1 * scaled_pnl * features.zscore;
    _performance.momentum_return = 0.9 * _performance.momentum_return + 
                                    0.1 * scaled_pnl * features.zscore_momentum;
    _performance.volatility_return = 0.9 * _performance.volatility_return + 
                                      0.1 * scaled_pnl * (features.volatility_ratio - 1.0);
    _performance.correlation_return = 0.9 * _performance.correlation_return + 
                                       0.1 * scaled_pnl * features.correlation_trend;
    _performance.mspread_return = 0.9 * _performance.mspread_return + 
                                   0.1 * scaled_pnl * features.mspread_imbalance;
    
    _performance.sample_count++;
    
    if (_config.use_adaptive_weights)
        updateAdaptiveWeights();
}

SpreadSignal StatisticalArbStrategy::generateSignal(const SpreadState& state, uint64_t current_time)
{
    SpreadSignal signal;
    signal.pair_id = state.pair_id;
    signal.source = ArbitrageStrategy::STATISTICAL_ARB;
    signal.timestamp = current_time;
    
    if (!_features.is_valid)
    {
        signal.type = SpreadSignalType::NONE;
        return signal;
    }
    
    double sig = _features.composite_signal;
    double abs_sig = std::abs(sig);
    
    // No position - check for entry
    if (!state.hasPosition())
    {
        if (sig > _config.entry_threshold)
        {
            signal.type = SpreadSignalType::OPEN_SHORT_SPREAD;
            signal.confidence = calculateConfidence(_features);
            signal.suggested_size = calculatePositionSize(_features);
            signal.entry_zscore = state.zscore;
            signal.reason = "Statistical signal: composite above threshold";
            _entry_signal = sig;
        }
        else if (sig < -_config.entry_threshold)
        {
            signal.type = SpreadSignalType::OPEN_LONG_SPREAD;
            signal.confidence = calculateConfidence(_features);
            signal.suggested_size = calculatePositionSize(_features);
            signal.entry_zscore = state.zscore;
            signal.reason = "Statistical signal: composite below threshold";
            _entry_signal = sig;
        }
    }
    // Has position - check for exit
    else
    {
        // Stop loss
        if (abs_sig > _config.stop_loss_threshold)
        {
            signal.type = SpreadSignalType::STOP_LOSS;
            signal.confidence = 1.0;
            signal.suggested_size = std::abs(state.spread_position);
            signal.reason = "Statistical signal extreme, stop loss";
        }
        // Timeout
        else if (state.positionDuration(current_time) > _config.convergence_timeout)
        {
            signal.type = SpreadSignalType::TIMEOUT_EXIT;
            signal.confidence = 0.8;
            signal.suggested_size = std::abs(state.spread_position);
            signal.reason = "Timeout: statistical signal did not converge";
        }
        // Normal exit
        else if (state.spread_position > 0 && sig > -_config.exit_threshold)
        {
            signal.type = SpreadSignalType::CLOSE_LONG_SPREAD;
            signal.confidence = 0.9;
            signal.suggested_size = state.spread_position;
            signal.reason = "Statistical signal normalized, closing position";
        }
        else if (state.spread_position < 0 && sig < _config.exit_threshold)
        {
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
    _performance = FeaturePerformance();
    _adaptive_weight_zscore = 0.30;
    _adaptive_weight_momentum = 0.20;
    _adaptive_weight_volatility = 0.15;
    _adaptive_weight_correlation = 0.20;
    _adaptive_weight_mspread = 0.15;
    _last_signal_time = 0;
    _entry_signal = 0;
}

} // namespace futu
