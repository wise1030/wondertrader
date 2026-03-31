/*!
 * \file StatisticalArbStrategy.h
 * \brief Statistical Arbitrage Strategy (Multi-factor)
 * 
 * Strategy Logic:
 *   - Multi-factor model for spread prediction
 *   - Feature-based signal generation
 *   - Machine learning signal combination
 *   - Risk-adjusted position sizing
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include "SpreadArbitrageTypes.h"
#include "SpreadCalculator.h"
#include "../Share/RingBuffer.hpp"
#include <memory>
#include <array>

namespace futu {

//==============================================================================
// Statistical Arbitrage Configuration
//==============================================================================

struct StatisticalArbConfig
{
    double entry_threshold;         ///< Composite signal threshold for entry
    double exit_threshold;          ///< Exit threshold
    double stop_loss_threshold;     ///< Stop loss threshold
    
    double max_position;            ///< Maximum position size
    double base_qty;                ///< Base position size
    
    uint32_t feature_window;        ///< Window for feature calculation
    uint32_t min_samples;           ///< Minimum samples required
    
    // Feature weights
    double weight_zscore;           ///< Z-Score feature weight
    double weight_momentum;         ///< Momentum feature weight
    double weight_volatility;       ///< Volatility feature weight
    double weight_correlation;      ///< Correlation feature weight
    double weight_mspread;          ///< Microstructure spread weight
    
    uint32_t convergence_timeout;   ///< Timeout for convergence (seconds)
    bool use_adaptive_weights;      ///< Adapt weights based on performance
    
    StatisticalArbConfig()
        : entry_threshold(0.7)
        , exit_threshold(0.3)
        , stop_loss_threshold(1.5)
        , max_position(15.0)
        , base_qty(1.0)
        , feature_window(100)
        , min_samples(50)
        , weight_zscore(0.30)
        , weight_momentum(0.20)
        , weight_volatility(0.15)
        , weight_correlation(0.20)
        , weight_mspread(0.15)
        , convergence_timeout(5400)
        , use_adaptive_weights(true)
    {}
};

//==============================================================================
// Statistical Features
//==============================================================================

struct StatisticalFeatures
{
    double zscore;                  ///< Normalized Z-Score
    double zscore_momentum;         ///< Z-Score change rate
    double volatility_ratio;        ///< Volatility ratio (leg1/leg2)
    double correlation_trend;       ///< Correlation trend
    double mspread_imbalance;       ///< Microstructure spread imbalance
    double volume_imbalance;        ///< Volume imbalance
    
    double composite_signal;        ///< Weighted composite signal
    double signal_confidence;       ///< Signal confidence
    
    // Feature quality metrics
    double feature_stability;       ///< Stability of features
    bool is_valid;                  ///< Are features valid
    
    StatisticalFeatures()
        : zscore(0), zscore_momentum(0)
        , volatility_ratio(1)
        , correlation_trend(0)
        , mspread_imbalance(0)
        , volume_imbalance(0)
        , composite_signal(0)
        , signal_confidence(0)
        , feature_stability(0)
        , is_valid(false)
    {}
};

//==============================================================================
// Feature Performance Tracker
//==============================================================================

struct FeaturePerformance
{
    double zscore_return;           ///< Average return from zscore signal
    double momentum_return;         ///< Average return from momentum signal
    double volatility_return;       ///< Average return from volatility signal
    double correlation_return;      ///< Average return from correlation signal
    double mspread_return;          ///< Average return from mspread signal
    
    uint32_t sample_count;          ///< Number of samples
    
    FeaturePerformance()
        : zscore_return(0)
        , momentum_return(0)
        , volatility_return(0)
        , correlation_return(0)
        , mspread_return(0)
        , sample_count(0)
    {}
};

//==============================================================================
// Statistical Arbitrage Strategy
//==============================================================================

class StatisticalArbStrategy
{
public:
    StatisticalArbStrategy();
    ~StatisticalArbStrategy() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const StatisticalArbConfig& config) { _config = config; }
    const StatisticalArbConfig& getConfig() const { return _config; }
    
    //==========================================================================
    // Data Update
    //==========================================================================
    
    /// Update with spread state
    void updateState(const SpreadState& state, uint64_t timestamp);
    
    //==========================================================================
    // Signal Generation
    //==========================================================================
    
    /// Generate trading signal
    SpreadSignal generateSignal(const SpreadState& state, uint64_t current_time);
    
    //==========================================================================
    // Feature Calculation
    //==========================================================================
    
    /// Calculate statistical features
    StatisticalFeatures calculateFeatures(const SpreadState& state);
    
    /// Get current features
    const StatisticalFeatures& getCurrentFeatures() const { return _features; }
    
    //==========================================================================
    // Performance Tracking
    //==========================================================================
    
    /// Record signal outcome for adaptive weights
    void recordOutcome(double pnl, const StatisticalFeatures& features);
    
    /// Get feature performance
    const FeaturePerformance& getFeaturePerformance() const { return _performance; }
    
    //==========================================================================
    // State
    //==========================================================================
    
    void reset();
    static constexpr const char* getName() { return "StatisticalArb"; }
    
private:
    //==========================================================================
    // Internal Methods
    //==========================================================================
    
    void updateFeatureHistory();
    double calculateZScoreFeature(const SpreadState& state) const;
    double calculateMomentumFeature(const SpreadState& state) const;
    double calculateVolatilityFeature(const SpreadState& state) const;
    double calculateCorrelationFeature(const SpreadState& state) const;
    double calculateMSpreadFeature(const SpreadState& state) const;
    
    void updateAdaptiveWeights();
    double calculatePositionSize(const StatisticalFeatures& features) const;
    double calculateConfidence(const StatisticalFeatures& features) const;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    StatisticalArbConfig _config;
    
    //==========================================================================
    // Feature History
    //==========================================================================
    
    RingBuffer<double, 128> _zscore_history;
    RingBuffer<double, 128> _volatility_history;
    RingBuffer<double, 128> _correlation_history;
    RingBuffer<double, 128> _signal_history;
    
    uint64_t _last_update;
    
    //==========================================================================
    // Current Features
    //==========================================================================
    
    StatisticalFeatures _features;
    double _prev_zscore;
    double _prev_correlation;
    
    //==========================================================================
    // Performance Tracking
    //==========================================================================
    
    FeaturePerformance _performance;
    
    //==========================================================================
    // Adaptive Weights
    //==========================================================================
    
    double _adaptive_weight_zscore;
    double _adaptive_weight_momentum;
    double _adaptive_weight_volatility;
    double _adaptive_weight_correlation;
    double _adaptive_weight_mspread;
    
    //==========================================================================
    // Signal State
    //==========================================================================
    
    uint64_t _last_signal_time;
    double _entry_signal;
};

} // namespace futu
