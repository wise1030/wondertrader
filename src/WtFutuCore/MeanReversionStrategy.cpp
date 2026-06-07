/*!
 * \file MeanReversionStrategy.cpp
 * \brief Mean Reversion Strategy Implementation
 */
#include "MeanReversionStrategy.h"
#include <cmath>
#include <algorithm>

namespace futu {

MeanReversionStrategy::MeanReversionStrategy()
    : _last_signal_time(0)
    , _last_signal_type(SpreadSignalType::NONE)
    , _entry_zscore(0)
{
}

SpreadSignal MeanReversionStrategy::generateSignal(const SpreadState& state, uint64_t current_time)
{
    SpreadSignal signal;
    signal.pair_id = state.pair_id;
    signal.source = ArbitrageStrategy::MEAN_REVERSION;
    signal.timestamp = current_time;
    
    // Check minimum samples
    if (!state.is_active)
    {
        signal.type = SpreadSignalType::NONE;
        return signal;
    }
    
    // Check half-life filter
    if (_config.use_half_life_filter && state.half_life > _config.max_half_life)
    {
        signal.type = SpreadSignalType::NONE;
        return signal;
    }
    
    // No position - check for entry
    if (!state.hasPosition())
    {
        double abs_z = std::abs(state.zscore);
        
        // Entry signal: Z-Score exceeds threshold
        if (state.zscore > _config.entry_z_threshold)
        {
            signal.type = SpreadSignalType::OPEN_SHORT_SPREAD;
            signal.confidence = calculateConfidence(state.zscore);
            signal.suggested_size = calculatePositionSize(state.zscore);
            signal.entry_zscore = state.zscore;
            signal.target_zscore = _config.exit_z_threshold;
            signal.reason = "Z-Score high, expecting mean reversion";
            _entry_zscore = state.zscore;
        }
        else if (state.zscore < -_config.entry_z_threshold)
        {
            signal.type = SpreadSignalType::OPEN_LONG_SPREAD;
            signal.confidence = calculateConfidence(state.zscore);
            signal.suggested_size = calculatePositionSize(state.zscore);
            signal.entry_zscore = state.zscore;
            signal.target_zscore = -_config.exit_z_threshold;
            signal.reason = "Z-Score low, expecting mean reversion";
            _entry_zscore = state.zscore;
        }
    }
    // Has position - check for exit
    else
    {
        // Check stop loss first
        if (std::abs(state.zscore) > _config.stop_loss_z)
        {
            signal.type = SpreadSignalType::STOP_LOSS;
            signal.confidence = 1.0;
            signal.suggested_size = std::abs(state.spread_position);
            signal.reason = "Stop loss triggered: Z-Score exceeded stop loss threshold";
        }
        // Check timeout
        else if (checkTimeout(state, current_time))
        {
            signal.type = SpreadSignalType::TIMEOUT_EXIT;
            signal.confidence = 0.8;
            signal.suggested_size = std::abs(state.spread_position);
            signal.reason = "Position timeout: spread did not converge";
        }
        // 均值回归退出：zscore 回归到0附近才退出
        // 修改：zscore > -exit_threshold * 0.3 才退出（更接近0，避免过早平仓）
        else if (state.spread_position > 0 && state.zscore > -_config.exit_z_threshold * 0.3)
        {
            signal.type = SpreadSignalType::CLOSE_LONG_SPREAD;
            signal.confidence = 0.9;
            signal.suggested_size = state.spread_position;
            signal.reason = "Z-Score reverted near zero, closing long spread";
        }
        else if (state.spread_position < 0 && state.zscore < _config.exit_z_threshold * 0.3)
        {
            signal.type = SpreadSignalType::CLOSE_SHORT_SPREAD;
            signal.confidence = 0.9;
            signal.suggested_size = std::abs(state.spread_position);
            signal.reason = "Z-Score reverted near zero, closing short spread";
        }
        // 加仓逻辑：仅在zscore远离止损区域时允许加仓
        // 安全间距：加仓上限 = stop_loss_z * add_safety_ratio
        // 例: stop_loss_z=4.0, add_safety_ratio=0.75 → 加仓区间为 [-3.0, -1.0]
        // 止损区间为 [-inf, -4.0]，两者之间有1.0的安全间距
        double add_safety_limit = _config.stop_loss_z * _config.add_safety_ratio;
        
        if (state.spread_position > 0 && 
                 state.zscore < -_config.entry_z_threshold * 0.5 &&
                 state.zscore > -add_safety_limit)
        {
            signal.type = SpreadSignalType::OPEN_LONG_SPREAD;
            signal.confidence = calculateConfidence(state.zscore) * 0.7;
            signal.suggested_size = calculatePositionSize(state.zscore) * 0.5;
            signal.reason = "Strong reversion, adding to position (safe zone)";
        }
        else if (state.spread_position < 0 && 
                 state.zscore > _config.entry_z_threshold * 0.5 &&
                 state.zscore < add_safety_limit)
        {
            signal.type = SpreadSignalType::OPEN_SHORT_SPREAD;
            signal.confidence = calculateConfidence(state.zscore) * 0.7;
            signal.suggested_size = calculatePositionSize(state.zscore) * 0.5;
            signal.reason = "Strong reversion, adding to position (safe zone)";
        }
    }
    
    _last_signal_time = current_time;
    if (signal.type != SpreadSignalType::NONE)
    {
        _last_signal_type = signal.type;
    }
    
    return signal;
}

double MeanReversionStrategy::calculatePositionSize(double zscore) const
{
    // Position size scales with Z-Score magnitude
    // Larger Z-Score = stronger signal = larger position
    
    double abs_z = std::abs(zscore);
    double ratio = abs_z / _config.entry_z_threshold;
    
    // Scale position with signal strength, cap at 2x base
    double size = _config.base_qty * (1.0 + _config.position_scale * std::min(ratio - 1.0, 1.0));
    
    // Apply position limit
    return std::min(size, _config.max_position);
}

double MeanReversionStrategy::calculateConfidence(double zscore) const
{
    double abs_z = std::abs(zscore);
    double ratio = abs_z / _config.entry_z_threshold;
    
    // Confidence ranges from 0.5 (at threshold) to 1.0 (at 2x threshold)
    return std::min(0.5 + (ratio - 1.0) * 0.5, 1.0);
}

bool MeanReversionStrategy::checkTimeout(const SpreadState& state, uint64_t current_time) const
{
    if (!state.hasPosition())
        return false;
    
    uint64_t duration = state.positionDuration(current_time);
    return duration > _config.convergence_timeout;
}

void MeanReversionStrategy::reset()
{
    _last_signal_time = 0;
    _last_signal_type = SpreadSignalType::NONE;
    _entry_zscore = 0;
}

} // namespace futu
