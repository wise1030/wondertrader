/*!
 * \file MarketMakingEnhancer.h
 * \brief Market Making Enhancement Strategy
 * 
 * Strategy Logic:
 *   - Adjust quoting based on spread signals
 *   - Widen spread when spread signal is strong
 *   - Pause one-sided quoting on extreme deviation
 *   - Dynamic skew adjustment based on position
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include "SpreadArbitrageTypes.h"
#include "SpreadCalculator.h"
#include "../Share/RingBuffer.hpp"
#include <memory>

namespace futu {

//==============================================================================
// Market Making Enhancer Configuration
//==============================================================================

struct MmEnhancerConfig
{
    // Quoting adjustment parameters
    double pause_z_threshold;       ///< Z-Score to pause quoting
    double widen_z_threshold;       ///< Z-Score to widen spread
    double skew_z_threshold;        ///< Z-Score to adjust skew
    
    double max_skew_adjustment;     ///< Maximum skew adjustment
    double max_spread_multiplier;   ///< Maximum spread multiplier
    
    // Position-based adjustment
    double position_skew_factor;    ///< Skew adjustment per position unit
    double max_position_skew;       ///< Maximum position-based skew
    
    // Confidence thresholds
    double min_signal_confidence;   ///< Minimum confidence to adjust
    double signal_weight;           ///< Weight of spread signal in adjustment
    
    // Decay parameters
    double adjustment_decay;        ///< Decay rate for adjustments
    uint32_t decay_interval_ms;     ///< Decay interval in milliseconds
    
    MmEnhancerConfig()
        : pause_z_threshold(3.0)
        , widen_z_threshold(1.5)
        , skew_z_threshold(1.0)
        , max_skew_adjustment(0.3)
        , max_spread_multiplier(2.0)
        , position_skew_factor(0.02)
        , max_position_skew(0.2)
        , min_signal_confidence(0.3)
        , signal_weight(0.6)
        , adjustment_decay(0.95)
        , decay_interval_ms(1000)
    {}
};

//==============================================================================
// Quoting State
//==============================================================================

struct QuotingState
{
    double bid_skew;                ///< Current bid skew
    double ask_skew;                ///< Current ask skew
    double spread_multiplier;       ///< Current spread multiplier
    
    bool suppress_bid;              ///< Suppress bid quoting
    bool suppress_ask;              ///< Suppress ask quoting
    
    double last_adjustment;         ///< Last adjustment value
    uint64_t last_adjustment_time;  ///< Time of last adjustment
    
    QuotingState()
        : bid_skew(0)
        , ask_skew(0)
        , spread_multiplier(1.0)
        , suppress_bid(false)
        , suppress_ask(false)
        , last_adjustment(0)
        , last_adjustment_time(0)
    {}
};

//==============================================================================
// Market Making Enhancer
//==============================================================================

class MarketMakingEnhancer
{
public:
    MarketMakingEnhancer();
    ~MarketMakingEnhancer() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const MmEnhancerConfig& config) { _config = config; }
    const MmEnhancerConfig& getConfig() const { return _config; }
    
    //==========================================================================
    // Quoting Adjustment
    //==========================================================================
    
    /// Calculate quoting adjustment based on spread state
    QuotingAdjustment calculateAdjustment(const SpreadState& state, 
                                          const SpreadSignal& signal,
                                          uint64_t current_time);
    
    /// Apply signal to current quoting state
    QuotingState applySignal(const SpreadState& state,
                             const SpreadSignal& signal,
                             uint64_t current_time);
    
    //==========================================================================
    // State Access
    //==========================================================================
    
    /// Get current quoting state
    const QuotingState& getQuotingState() const { return _quoting_state; }
    
    //==========================================================================
    // Management
    //==========================================================================
    
    void reset();
    static constexpr const char* getName() { return "MarketMakingEnhancer"; }
    
private:
    //==========================================================================
    // Internal Methods
    //==========================================================================
    
    double calculateSkewAdjustment(double zscore, double confidence) const;
    double calculateSpreadMultiplier(double zscore, double confidence) const;
    bool shouldSuppressBid(double zscore) const;
    bool shouldSuppressAsk(double zscore) const;
    void applyDecay(uint64_t current_time);
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    MmEnhancerConfig _config;
    
    //==========================================================================
    // Quoting State
    //==========================================================================
    
    QuotingState _quoting_state;
    
    //==========================================================================
    // History
    //==========================================================================
    
    RingBuffer<double, 64> _adjustment_history;
    uint64_t _last_decay_time;
};

} // namespace futu
