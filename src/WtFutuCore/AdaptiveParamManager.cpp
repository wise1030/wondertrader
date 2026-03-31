/*!
 * \file AdaptiveParamManager.cpp
 * \brief Adaptive Parameter Tuning Implementation
 * 
 * Uses a gradient-free optimization approach with momentum
 * for real-time parameter adjustment.
 */
#include "AdaptiveParamManager.h"
#include "../Share/TimeUtils.hpp"
#include "../WTSTools/WTSLogger.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace futu {

AdaptiveParamManager::AdaptiveParamManager()
    : _last_adjustment_time(0)
    , _total_adjustments(0)
    , _avg_improvement(0)
    , _cumulative_improvement(0)
{
}

void AdaptiveParamManager::registerParam(ParamType type, const ParamBounds& bounds)
{
    int key = static_cast<int>(type);
    _params[key] = bounds;
    _gradient_estimates[key] = 0;
    _velocity[key] = 0;
}

double AdaptiveParamManager::getParam(ParamType type) const
{
    int key = static_cast<int>(type);
    auto it = _params.find(key);
    if (it != _params.end())
        return it->second.current;
    return 0;
}

void AdaptiveParamManager::setParam(ParamType type, double value)
{
    int key = static_cast<int>(type);
    auto it = _params.find(key);
    if (it != _params.end())
    {
        it->second.current = it->second.clamp(value);
    }
}

void AdaptiveParamManager::recordPerformance(const PerformanceSample& sample)
{
    _perf_history.push(sample);
}

PerformanceSample AdaptiveParamManager::getRecentPerformance() const
{
    PerformanceSample recent;
    
    if (_perf_history.empty())
        return recent;
    
    // Aggregate recent samples
    double total_pnl = 0;
    double total_fill_rate = 0;
    double total_spread = 0;
    double total_vol = 0;
    uint32_t total_quotes = 0;
    uint32_t total_fills = 0;
    uint32_t count = 0;
    
    size_t start = _perf_history.size() > _config.window_size 
                   ? _perf_history.size() - _config.window_size 
                   : 0;
    
    for (size_t i = start; i < _perf_history.size(); ++i)
    {
        const auto& s = _perf_history[i];
        total_pnl += s.realized_pnl + s.unrealized_pnl * 0.5;
        total_fill_rate += s.fill_rate;
        total_spread += s.spread_captured;
        total_vol += s.volatility;
        total_quotes += s.quote_count;
        total_fills += s.fill_count;
        count++;
    }
    
    if (count > 0)
    {
        recent.realized_pnl = total_pnl;
        recent.fill_rate = total_fill_rate / count;
        recent.spread_captured = total_spread / count;
        recent.volatility = total_vol / count;
        recent.quote_count = total_quotes;
        recent.fill_count = total_fills;
    }
    
    return recent;
}

double AdaptiveParamManager::estimateGradient(ParamType type) const
{
    // Estimate gradient using finite differences on recent performance
    // This is a simplified approach - in production, use more sophisticated methods
    
    if (_perf_history.size() < 20)
        return 0;
    
    int key = static_cast<int>(type);
    auto param_it = _params.find(key);
    if (param_it == _params.end())
        return 0;
    
    double current_val = param_it->second.current;
    
    // Compare performance when parameter was higher vs lower
    double high_pnl = 0, low_pnl = 0;
    int high_count = 0, low_count = 0;
    
    double median = (param_it->second.min_val + param_it->second.max_val) / 2;
    
    for (size_t i = _perf_history.size() - 20; i < _perf_history.size(); ++i)
    {
        const auto& s = _perf_history[i];
        double score = s.score();
        
        // Use parameter history if available (simplified: use current as proxy)
        if (current_val > median)
        {
            high_pnl += score;
            high_count++;
        }
        else
        {
            low_pnl += score;
            low_count++;
        }
    }
    
    if (high_count == 0 || low_count == 0)
        return 0;
    
    double avg_high = high_pnl / high_count;
    double avg_low = low_pnl / low_count;
    
    // Gradient points in direction of improvement
    double diff = param_it->second.max_val - param_it->second.min_val;
    if (diff == 0) return 0;
    
    return (avg_high - avg_low) / diff;
}

bool AdaptiveParamManager::shouldAdjust(ParamType type) const
{
    // Check if enough time has passed since last adjustment
    uint64_t now = TimeUtils::getLocalTimeNow();
    if (now - _last_adjustment_time < _config.adjustment_freq * 1000)
        return false;
    
    // Check if we have enough performance data
    if (_perf_history.size() < 10)
        return false;
    
    return true;
}

double AdaptiveParamManager::computeAdjustment(ParamType type, double gradient)
{
    int key = static_cast<int>(type);
    
    // Update velocity with momentum
    double old_velocity = _velocity[key];
    double new_velocity = _config.momentum * old_velocity + _config.learning_rate * gradient;
    _velocity[key] = new_velocity;
    
    // Compute adjustment
    return new_velocity;
}

std::vector<ParamAdjustment> AdaptiveParamManager::updateParameters()
{
    std::vector<ParamAdjustment> adjustments;
    
    if (!_config.enabled)
        return adjustments;
    
    // Check if adjustment is needed
    uint64_t now = TimeUtils::getLocalTimeNow();
    
    for (auto& kv : _params)
    {
        ParamType type = static_cast<ParamType>(kv.first);
        ParamBounds& bounds = kv.second;
        
        // Estimate gradient
        double gradient = estimateGradient(type);
        
        // Apply gradient with momentum
        double adjustment = computeAdjustment(type, gradient);
        
        // Only apply if adjustment is significant
        if (std::abs(adjustment) < bounds.step * 0.1)
            continue;
        
        // Compute new value
        double old_val = bounds.current;
        double new_val = bounds.clamp(old_val + adjustment);
        
        // Create adjustment record
        ParamAdjustment adj;
        adj.type = type;
        adj.old_value = old_val;
        adj.new_value = new_val;
        adj.delta = new_val - old_val;
        
        // Determine reason
        if (gradient > 0)
            adj.reason = "Improving performance";
        else
            adj.reason = "Reducing losses";
        
        // Apply adjustment
        bounds.current = new_val;
        adjustments.push_back(adj);
        
        _total_adjustments++;
    }
    
    if (!adjustments.empty())
    {
        _last_adjustment_time = now;
        
        WTSLogger::debug("[ADAPTIVE] Made {} parameter adjustments", adjustments.size());
    }
    
    return adjustments;
}

ParamAdjustment AdaptiveParamManager::forceAdjust(ParamType type, double delta)
{
    ParamAdjustment adj;
    adj.type = type;
    
    int key = static_cast<int>(type);
    auto it = _params.find(key);
    if (it != _params.end())
    {
        ParamBounds& bounds = it->second;
        adj.old_value = bounds.current;
        adj.new_value = bounds.clamp(bounds.current + delta);
        adj.delta = adj.new_value - adj.old_value;
        adj.reason = "Manual adjustment";
        
        bounds.current = adj.new_value;
        _total_adjustments++;
    }
    
    return adj;
}

void AdaptiveParamManager::adaptForHighVolatility()
{
    // Widen spread in high volatility
    forceAdjust(ParamType::SPREAD_BASE, 0.5);
    forceAdjust(ParamType::SPREAD_VOL_SENSITIVITY, 0.2);
    
    // Reduce inventory limits
    forceAdjust(ParamType::INVENTORY_MAX, -10.0);
    
    // Lower toxicity threshold
    forceAdjust(ParamType::TOXICITY_THRESHOLD, -0.1);
    
    WTSLogger::info("[ADAPTIVE] Adapted for HIGH VOLATILITY");
}

void AdaptiveParamManager::adaptForLowVolatility()
{
    // Tighten spread in low volatility
    forceAdjust(ParamType::SPREAD_BASE, -0.3);
    
    // Increase inventory limits
    forceAdjust(ParamType::INVENTORY_MAX, 5.0);
    
    WTSLogger::info("[ADAPTIVE] Adapted for LOW VOLATILITY");
}

void AdaptiveParamManager::adaptForTrending(double trendStrength)
{
    // Increase alpha sensitivity in trending markets
    forceAdjust(ParamType::ALPHA_SENSITIVITY, 0.1 * trendStrength);
    
    // Widen spread to avoid adverse selection
    forceAdjust(ParamType::SPREAD_BASE, 0.3 * trendStrength);
    
    // Lower toxicity threshold
    forceAdjust(ParamType::TOXICITY_THRESHOLD, -0.05 * trendStrength);
    
    WTSLogger::info("[ADAPTIVE] Adapted for TRENDING market (strength={:.2f})", trendStrength);
}

void AdaptiveParamManager::adaptForRanging()
{
    // Decrease alpha sensitivity (less predictive power)
    forceAdjust(ParamType::ALPHA_SENSITIVITY, -0.1);
    
    // Tighten spread (capture more spread in ranging market)
    forceAdjust(ParamType::SPREAD_BASE, -0.2);
    
    // Increase inventory limits
    forceAdjust(ParamType::INVENTORY_MAX, 5.0);
    
    WTSLogger::info("[ADAPTIVE] Adapted for RANGING market");
}

void AdaptiveParamManager::reset()
{
    _perf_history.clear();
    _gradient_estimates.clear();
    _velocity.clear();
    _last_adjustment_time = 0;
    _total_adjustments = 0;
    _avg_improvement = 0;
    _cumulative_improvement = 0;
}

} // namespace futu
