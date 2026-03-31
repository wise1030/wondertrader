/*!
 * \file MarketMakingEnhancer.cpp
 * \brief Market Making Enhancement Strategy Implementation
 */
#include "MarketMakingEnhancer.h"
#include <cmath>
#include <algorithm>

namespace futu {

MarketMakingEnhancer::MarketMakingEnhancer()
    : _last_decay_time(0)
{
}

QuotingAdjustment MarketMakingEnhancer::calculateAdjustment(
    const SpreadState& state, 
    const SpreadSignal& signal,
    uint64_t current_time)
{
    QuotingAdjustment adj;
    
    // Apply decay first
    applyDecay(current_time);
    
    // Check confidence threshold
    if (signal.confidence < _config.min_signal_confidence)
    {
        adj.confidence = 0;
        return adj;
    }
    
    double zscore = state.zscore;
    double abs_z = std::abs(zscore);
    
    // Calculate skew adjustment
    double skew_adj = calculateSkewAdjustment(zscore, signal.confidence);
    
    // Calculate spread multiplier
    double spread_mult = calculateSpreadMultiplier(zscore, signal.confidence);
    
    // Check for suppression
    adj.suppress_bid = shouldSuppressBid(zscore);
    adj.suppress_ask = shouldSuppressAsk(zscore);
    
    // Apply signal weight
    adj.bid_skew_adjustment = skew_adj * _config.signal_weight;
    adj.ask_skew_adjustment = -skew_adj * _config.signal_weight;
    adj.spread_multiplier = spread_mult;
    adj.confidence = signal.confidence;
    
    return adj;
}

QuotingState MarketMakingEnhancer::applySignal(
    const SpreadState& state,
    const SpreadSignal& signal,
    uint64_t current_time)
{
    QuotingAdjustment adj = calculateAdjustment(state, signal, current_time);
    
    // Update quoting state
    _quoting_state.bid_skew += adj.bid_skew_adjustment;
    _quoting_state.ask_skew += adj.ask_skew_adjustment;
    _quoting_state.spread_multiplier = adj.spread_multiplier;
    _quoting_state.suppress_bid = adj.suppress_bid;
    _quoting_state.suppress_ask = adj.suppress_ask;
    
    // Clamp values
    _quoting_state.bid_skew = std::max(-_config.max_skew_adjustment,
                                        std::min(_config.max_skew_adjustment, 
                                                 _quoting_state.bid_skew));
    _quoting_state.ask_skew = std::max(-_config.max_skew_adjustment,
                                        std::min(_config.max_skew_adjustment,
                                                 _quoting_state.ask_skew));
    _quoting_state.spread_multiplier = std::min(_quoting_state.spread_multiplier,
                                                 _config.max_spread_multiplier);
    
    _quoting_state.last_adjustment = adj.bid_skew_adjustment;
    _quoting_state.last_adjustment_time = current_time;
    
    _adjustment_history.push(adj.bid_skew_adjustment);
    
    return _quoting_state;
}

double MarketMakingEnhancer::calculateSkewAdjustment(double zscore, double confidence) const
{
    double abs_z = std::abs(zscore);
    
    // Only adjust if above threshold
    if (abs_z < _config.skew_z_threshold)
        return 0;
    
    // Linear scaling based on Z-Score
    double excess = abs_z - _config.skew_z_threshold;
    double max_excess = _config.pause_z_threshold - _config.skew_z_threshold;
    
    double adjustment = (excess / max_excess) * _config.max_skew_adjustment;
    adjustment *= confidence;
    
    // Sign based on Z-Score direction
    // Positive Z-Score = spread high = prefer to sell = negative skew (more aggressive on ask)
    // Negative Z-Score = spread low = prefer to buy = positive skew (more aggressive on bid)
    return (zscore > 0) ? -adjustment : adjustment;
}

double MarketMakingEnhancer::calculateSpreadMultiplier(double zscore, double confidence) const
{
    double abs_z = std::abs(zscore);
    
    // Only widen if above threshold
    if (abs_z < _config.widen_z_threshold)
        return 1.0;
    
    // Calculate widening factor
    double excess = abs_z - _config.widen_z_threshold;
    double max_excess = _config.pause_z_threshold - _config.widen_z_threshold;
    
    double widening = 1.0 + (excess / max_excess) * (_config.max_spread_multiplier - 1.0);
    widening = 1.0 + (widening - 1.0) * confidence;
    
    return std::min(widening, _config.max_spread_multiplier);
}

bool MarketMakingEnhancer::shouldSuppressBid(double zscore) const
{
    // Suppress bid when spread is very high (expect spread to decrease)
    return zscore > _config.pause_z_threshold;
}

bool MarketMakingEnhancer::shouldSuppressAsk(double zscore) const
{
    // Suppress ask when spread is very low (expect spread to increase)
    return zscore < -_config.pause_z_threshold;
}

void MarketMakingEnhancer::applyDecay(uint64_t current_time)
{
    if (_last_decay_time == 0)
    {
        _last_decay_time = current_time;
        return;
    }
    
    uint64_t elapsed = current_time - _last_decay_time;
    if (elapsed < _config.decay_interval_ms)
        return;
    
    // Apply decay
    uint32_t decay_steps = elapsed / _config.decay_interval_ms;
    double decay_factor = std::pow(_config.adjustment_decay, decay_steps);
    
    _quoting_state.bid_skew *= decay_factor;
    _quoting_state.ask_skew *= decay_factor;
    
    // Reset suppression after decay
    if (std::abs(_quoting_state.bid_skew) < 0.05)
    {
        _quoting_state.suppress_bid = false;
    }
    if (std::abs(_quoting_state.ask_skew) < 0.05)
    {
        _quoting_state.suppress_ask = false;
    }
    
    _last_decay_time = current_time;
}

void MarketMakingEnhancer::reset()
{
    _quoting_state = QuotingState();
    _adjustment_history.clear();
    _last_decay_time = 0;
}

} // namespace futu
