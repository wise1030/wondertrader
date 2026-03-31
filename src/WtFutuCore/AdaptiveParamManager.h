/*!
 * \file AdaptiveParamManager.h
 * \brief Adaptive Parameter Tuning for Market Making
 * 
 * Dynamically adjusts strategy parameters based on:
 *   - Recent PnL performance
 *   - Fill rate analysis
 *   - Market condition changes
 *   - Risk utilization
 * 
 * Uses a gradient-free optimization approach suitable for
 * real-time parameter adjustment in production.
 */
#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include "../Share/RingBuffer.hpp"
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"

namespace futu {

//==============================================================================
// Parameter Types
//==============================================================================

/// Tunable parameter types
enum class ParamType : uint8_t
{
    SPREAD_BASE,            ///< Base spread (ticks)
    SPREAD_VOL_SENSITIVITY, ///< Volatility sensitivity
    SPREAD_PHI,             ///< Inventory penalty coefficient
    ALPHA_SENSITIVITY,      ///< Alpha impact on price
    ALPHA_OFI_WEIGHT,       ///< OFI weight in alpha
    ALPHA_TRADE_WEIGHT,     ///< Trade imbalance weight
    ALPHA_LEADLAG_WEIGHT,   ///< Lead-lag weight
    INVENTORY_MAX,          ///< Max inventory
    CANCEL_MAX_AGE,         ///< Max order age for cancel
    TOXICITY_THRESHOLD      ///< VPIN threshold
};

/// Parameter bounds
struct ParamBounds
{
    double min_val;
    double max_val;
    double step;        ///< Minimum adjustment step
    double current;
    
    ParamBounds()
        : min_val(0), max_val(1), step(0.01), current(0.5)
    {}
    
    ParamBounds(double minV, double maxV, double stp, double cur)
        : min_val(minV), max_val(maxV), step(stp), current(cur)
    {}
    
    double clamp(double val) const
    {
        return std::max(min_val, std::min(max_val, val));
    }
};

//==============================================================================
// Performance Metrics
//==============================================================================

/// Performance sample for parameter tuning
struct PerformanceSample
{
    double realized_pnl;        ///< Realized PnL in window
    double unrealized_pnl;      ///< Unrealized PnL
    double fill_rate;           ///< Fill rate (fills / quotes)
    double spread_captured;     ///< Average spread captured
    double inventory_cost;      ///< Inventory carrying cost
    double adverse_selection;   ///< Estimated adverse selection loss
    double volatility;          ///< Market volatility in window
    uint32_t quote_count;       ///< Number of quotes placed
    uint32_t fill_count;        ///< Number of fills
    uint32_t cancel_count;      ///< Number of cancels
    uint64_t timestamp;
    
    PerformanceSample()
        : realized_pnl(0), unrealized_pnl(0), fill_rate(0)
        , spread_captured(0), inventory_cost(0), adverse_selection(0)
        , volatility(0), quote_count(0), fill_count(0)
        , cancel_count(0), timestamp(0)
    {}
    
    /// Calculate composite score
    double score() const
    {
        // Score = PnL - Costs - Risk Penalty
        double score = realized_pnl + unrealized_pnl * 0.5;
        score -= inventory_cost;
        score -= adverse_selection * 2.0;  // Heavy penalty for adverse selection
        
        // Bonus for good fill rate
        if (fill_rate > 0.2 && fill_rate < 0.5)
        {
            score += spread_captured * fill_rate;
        }
        
        return score;
    }
};

//==============================================================================
// Parameter Adjustment
//==============================================================================

/// Parameter adjustment result
struct ParamAdjustment
{
    ParamType type;
    double old_value;
    double new_value;
    double delta;
    std::string reason;
    
    ParamAdjustment()
        : type(ParamType::SPREAD_BASE)
        , old_value(0), new_value(0), delta(0)
    {}
};

//==============================================================================
// Adaptive Parameter Manager
//==============================================================================

/// Adaptive Parameter Manager Configuration
struct AdaptiveConfig
{
    uint32_t    window_size;        ///< Performance window size
    double      adjustment_freq;    ///< Adjustment frequency (seconds)
    double      learning_rate;      ///< Learning rate for adjustments
    double      momentum;           ///< Momentum for gradient estimation
    double      min_improvement;    ///< Minimum improvement to accept change
    bool        enabled;            ///< Enable/disable adaptation
    
    AdaptiveConfig()
        : window_size(100)
        , adjustment_freq(60.0)
        , learning_rate(0.1)
        , momentum(0.9)
        , min_improvement(0.01)
        , enabled(true)
    {}
};

/// Adaptive Parameter Manager
class AdaptiveParamManager
{
public:
    AdaptiveParamManager();
    ~AdaptiveParamManager() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const AdaptiveConfig& config) { _config = config; }
    const AdaptiveConfig& getConfig() const { return _config; }
    
    void setEnabled(bool enabled) { _config.enabled = enabled; }
    bool isEnabled() const { return _config.enabled; }
    
    //==========================================================================
    // Parameter Setup
    //==========================================================================
    
    /// Register a tunable parameter
    void registerParam(ParamType type, const ParamBounds& bounds);
    
    /// Get current parameter value
    double getParam(ParamType type) const;
    
    /// Set parameter value (manual override)
    void setParam(ParamType type, double value);
    
    //==========================================================================
    // Performance Tracking
    //==========================================================================
    
    /// Record a performance sample
    void recordPerformance(const PerformanceSample& sample);
    
    /// Get recent performance statistics
    PerformanceSample getRecentPerformance() const;
    
    //==========================================================================
    // Parameter Adjustment
    //==========================================================================
    
    /// Update parameters based on recent performance
    /// Returns list of adjustments made
    std::vector<ParamAdjustment> updateParameters();
    
    /// Force adjust a specific parameter
    ParamAdjustment forceAdjust(ParamType type, double delta);
    
    //==========================================================================
    // Market State Adaptation
    //==========================================================================
    
    /// Adapt parameters for high volatility
    void adaptForHighVolatility();
    
    /// Adapt parameters for low volatility
    void adaptForLowVolatility();
    
    /// Adapt parameters for trending market
    void adaptForTrending(double trendStrength);
    
    /// Adapt parameters for ranging market
    void adaptForRanging();
    
    //==========================================================================
    // Statistics
    //==========================================================================
    
    /// Get total number of adjustments
    uint32_t getTotalAdjustments() const { return _total_adjustments; }
    
    /// Get average improvement per adjustment
    double getAvgImprovement() const { return _avg_improvement; }
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void reset();
    
private:
    AdaptiveConfig _config;
    
    // Registered parameters (using wtp namespace)
    wtp::wt_hashmap<int, ParamBounds> _params;
    
    // Performance history (capacity must be power of 2)
    RingBuffer<PerformanceSample, 512> _perf_history;
    
    // Gradient estimates (for momentum-based optimization)
    wtp::wt_hashmap<int, double> _gradient_estimates;
    wtp::wt_hashmap<int, double> _velocity;
    
    // Last adjustment time
    uint64_t _last_adjustment_time;
    
    // Statistics
    uint32_t _total_adjustments;
    double _avg_improvement;
    double _cumulative_improvement;
    
    // Helper methods
    double estimateGradient(ParamType type) const;
    bool shouldAdjust(ParamType type) const;
    double computeAdjustment(ParamType type, double gradient);
};

} // namespace futu
