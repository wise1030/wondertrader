/*!
 * \file TrendFollowingStrategy.cpp
 * \brief Trend Following Strategy Implementation
 */
#include "TrendFollowingStrategy.h"
#include <cmath>
#include <algorithm>

namespace futu {

TrendFollowingStrategy::TrendFollowingStrategy()
    : _last_update(0)
    , _fast_ma(0)
    , _slow_ma(0)
    , _prev_fast_ma(0)
    , _prev_slow_ma(0)
    , _prev_trend_direction(0)
    , _bars_in_current_trend(0)
    , _last_signal_time(0)
{
}

void TrendFollowingStrategy::updateSpread(double spread, uint64_t timestamp)
{
    _prev_fast_ma = _fast_ma;
    _prev_slow_ma = _slow_ma;
    
    _spread_history.push(spread);
    _last_update = timestamp;
    
    updateTrendState();
}

void TrendFollowingStrategy::updateTrendState()
{
    size_t n = _spread_history.size();
    if (n < _config.slow_ma_period)
        return;
    
    _fast_ma = calculateMA(_spread_history, _config.fast_ma_period);
    _slow_ma = calculateMA(_spread_history, _config.slow_ma_period);
    
    _trend_state.fast_ma = _fast_ma;
    _trend_state.slow_ma = _slow_ma;
    _trend_state.ma_diff = _fast_ma - _slow_ma;
    
    if (_slow_ma > 1e-10)
    {
        _trend_state.ma_diff_pct = _trend_state.ma_diff / _slow_ma;
    }
    
    _trend_state.trend_strength = calculateTrendStrength();
    int new_direction = detectTrendDirection();
    
    // Track bars in trend
    if (new_direction == _prev_trend_direction && new_direction != 0)
    {
        _bars_in_current_trend++;
    }
    else
    {
        _bars_in_current_trend = 1;
    }
    
    _trend_state.trend_direction = new_direction;
    _trend_state.bars_in_trend = _bars_in_current_trend;
    _trend_state.is_strong_trend = std::abs(_trend_state.trend_strength) >= _config.min_trend_strength;
    
    // Detect reversal
    _trend_state.is_trend_reversal = false;
    if (_prev_fast_ma > 0 && _prev_slow_ma > 0)
    {
        // Crossover detection
        bool was_above = _prev_fast_ma > _prev_slow_ma;
        bool is_above = _fast_ma > _slow_ma;
        _trend_state.is_trend_reversal = (was_above != is_above);
    }
    
    _prev_trend_direction = new_direction;
}

double TrendFollowingStrategy::calculateMA(const RingBuffer<double, 128>& data, uint32_t period) const
{
    size_t n = data.size();
    if (n < period || period == 0)
        return 0;
    
    double sum = 0;
    for (size_t i = n - period; i < n; ++i)
    {
        sum += data[i];
    }
    return sum / period;
}

double TrendFollowingStrategy::calculateTrendStrength() const
{
    // Calculate trend strength as slope of spread
    size_t n = _spread_history.size();
    if (n < 5)
        return 0;
    
    // Simple linear regression over last 5 points
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
    size_t start = n - 5;
    
    for (size_t i = 0; i < 5; ++i)
    {
        double x = static_cast<double>(i);
        double y = _spread_history[start + i];
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }
    
    double denom = 5 * sum_xx - sum_x * sum_x;
    if (std::abs(denom) < 1e-10)
        return 0;
    
    double slope = (5 * sum_xy - sum_x * sum_y) / denom;
    
    // Normalize by price level
    if (_slow_ma > 1e-10)
    {
        return slope / _slow_ma;
    }
    return slope;
}

int TrendFollowingStrategy::detectTrendDirection() const
{
    if (_fast_ma > _slow_ma + _config.entry_threshold)
    {
        return 1;  // Uptrend
    }
    else if (_fast_ma < _slow_ma - _config.entry_threshold)
    {
        return -1;  // Downtrend
    }
    return 0;  // Neutral
}

bool TrendFollowingStrategy::isConfirmedTrend() const
{
    return _bars_in_current_trend >= _config.confirmation_bars;
}

SpreadSignal TrendFollowingStrategy::generateSignal(const SpreadState& state, uint64_t current_time)
{
    SpreadSignal signal;
    signal.pair_id = state.pair_id;
    signal.source = ArbitrageStrategy::TREND_FOLLOWING;
    signal.timestamp = current_time;
    
    size_t n = _spread_history.size();
    if (n < _config.slow_ma_period)
    {
        signal.type = SpreadSignalType::NONE;
        return signal;
    }
    
    // No position - check for entry
    if (!state.hasPosition())
    {
        // Check for trend following entry
        if (_trend_state.is_strong_trend && isConfirmedTrend())
        {
            if (_trend_state.trend_direction > 0)
            {
                // Uptrend - go long spread
                signal.type = SpreadSignalType::OPEN_LONG_SPREAD;
                signal.confidence = calculateConfidence(_trend_state);
                signal.suggested_size = calculatePositionSize(_trend_state);
                signal.reason = "Uptrend confirmed, following spread expansion";
            }
            else if (_trend_state.trend_direction < 0)
            {
                // Downtrend - go short spread
                signal.type = SpreadSignalType::OPEN_SHORT_SPREAD;
                signal.confidence = calculateConfidence(_trend_state);
                signal.suggested_size = calculatePositionSize(_trend_state);
                signal.reason = "Downtrend confirmed, following spread contraction";
            }
        }
    }
    // Has position - check for exit
    else
    {
        // Exit on trend reversal
        if (_trend_state.is_trend_reversal)
        {
            if (state.spread_position > 0)
            {
                signal.type = SpreadSignalType::CLOSE_LONG_SPREAD;
                signal.confidence = 0.9;
                signal.suggested_size = state.spread_position;
                signal.reason = "Trend reversal detected, closing long spread";
            }
            else if (state.spread_position < 0)
            {
                signal.type = SpreadSignalType::CLOSE_SHORT_SPREAD;
                signal.confidence = 0.9;
                signal.suggested_size = std::abs(state.spread_position);
                signal.reason = "Trend reversal detected, closing short spread";
            }
        }
        // Exit on trend exhaustion
        else if (_trend_state.bars_in_trend > 50)
        {
            // Trend may be exhausted
            if (state.spread_position > 0 && _trend_state.trend_direction <= 0)
            {
                signal.type = SpreadSignalType::CLOSE_LONG_SPREAD;
                signal.confidence = 0.7;
                signal.suggested_size = state.spread_position;
                signal.reason = "Trend exhaustion, closing position";
            }
            else if (state.spread_position < 0 && _trend_state.trend_direction >= 0)
            {
                signal.type = SpreadSignalType::CLOSE_SHORT_SPREAD;
                signal.confidence = 0.7;
                signal.suggested_size = std::abs(state.spread_position);
                signal.reason = "Trend exhaustion, closing position";
            }
        }
    }
    
    _last_signal_time = current_time;
    return signal;
}

double TrendFollowingStrategy::calculatePositionSize(const TrendState& trend) const
{
    // Position size scales with trend strength
    double strength_factor = std::abs(trend.trend_strength) / _config.min_trend_strength;
    strength_factor = std::min(strength_factor, 2.0);  // Cap at 2x
    
    double size = _config.base_qty * (0.5 + 0.5 * strength_factor);
    return std::min(size, _config.max_position);
}

double TrendFollowingStrategy::calculateConfidence(const TrendState& trend) const
{
    // Confidence based on trend strength and confirmation
    double strength_factor = std::min(std::abs(trend.trend_strength) / _config.min_trend_strength, 2.0);
    double confirm_factor = std::min(trend.bars_in_trend / static_cast<double>(_config.confirmation_bars), 2.0);
    
    return std::min(0.5 + strength_factor * 0.2 + confirm_factor * 0.15, 0.95);
}

void TrendFollowingStrategy::reset()
{
    _spread_history.clear();
    _fast_ma = 0;
    _slow_ma = 0;
    _prev_fast_ma = 0;
    _prev_slow_ma = 0;
    _prev_trend_direction = 0;
    _bars_in_current_trend = 0;
    _last_signal_time = 0;
    _trend_state = TrendState();
}

} // namespace futu
