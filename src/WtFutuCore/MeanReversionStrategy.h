/*!
 * \file MeanReversionStrategy.h
 * \brief Mean Reversion Strategy for Spread Arbitrage
 * 
 * Strategy Logic:
 *   - Open when Z-Score exceeds threshold
 *   - Close when Z-Score reverts to mean
 *   - Stop loss on extreme deviation
 *   - Timeout exit on non-convergence
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include "SpreadArbitrageTypes.h"
#include "SpreadCalculator.h"
#include <memory>

namespace futu {

//==============================================================================
// Mean Reversion Configuration
//==============================================================================

struct MeanReversionConfig
{
    double entry_z_threshold;       ///< Z-Score threshold for entry
    double exit_z_threshold;        ///< Z-Score threshold for exit
    double stop_loss_z;             ///< Z-Score threshold for stop loss
    double max_position;            ///< Maximum position size
    
    uint32_t min_samples;           ///< Minimum samples required
    uint32_t convergence_timeout;   ///< Timeout for convergence (seconds)
    
    double base_qty;                ///< Base position size
    double position_scale;          ///< Position scaling factor
    
    bool use_half_life_filter;      ///< Filter by half-life
    double max_half_life;           ///< Maximum acceptable half-life
    
    double add_safety_ratio;        ///< 加仓安全比率，加仓上限 = stop_loss_z * ratio
                                    ///< 默认0.75，即加仓区间不超过止损阈值的75%
                                    ///< 避免在接近止损区域时加仓
    
    MeanReversionConfig()
        : entry_z_threshold(2.0)
        , exit_z_threshold(0.5)
        , stop_loss_z(4.0)
        , max_position(20.0)
        , min_samples(30)
        , convergence_timeout(3600)
        , base_qty(1.0)
        , position_scale(0.5)
        , use_half_life_filter(true)
        , max_half_life(500)
        , add_safety_ratio(0.75)
    {}
};

//==============================================================================
// Mean Reversion Strategy
//==============================================================================

class MeanReversionStrategy
{
public:
    MeanReversionStrategy();
    ~MeanReversionStrategy() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const MeanReversionConfig& config) { _config = config; }
    const MeanReversionConfig& getConfig() const { return _config; }
    
    //==========================================================================
    // Signal Generation
    //==========================================================================
    
    /// Generate trading signal based on current state
    SpreadSignal generateSignal(const SpreadState& state, uint64_t current_time);
    
    //==========================================================================
    // Position Size Calculation
    //==========================================================================
    
    /// Calculate position size based on Z-Score
    double calculatePositionSize(double zscore) const;
    
    //==========================================================================
    // State
    //==========================================================================
    
    /// Reset internal state
    void reset();
    
    /// Get strategy name
    static constexpr const char* getName() { return "MeanReversion"; }
    
private:
    //==========================================================================
    // Internal Methods
    //==========================================================================
    
    bool checkTimeout(const SpreadState& state, uint64_t current_time) const;
    double calculateConfidence(double zscore) const;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    MeanReversionConfig _config;
    
    //==========================================================================
    // Internal State
    //==========================================================================
    
    uint64_t _last_signal_time;
    SpreadSignalType _last_signal_type;
    double _entry_zscore;
};

} // namespace futu
