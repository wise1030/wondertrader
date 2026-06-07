/*!
 * \file SpreadCalculator.h
 * \brief Spread Calculation Engine for Cross-Term Arbitrage
 * 
 * Provides spread calculation and statistical analysis:
 *   - Multiple spread types (simple, weighted, log, ratio)
 *   - Rolling statistics (mean, std, Z-Score)
 *   - Correlation and beta calculation
 *   - Half-life estimation for mean reversion
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include "SpreadArbitrageTypes.h"
#include "../Share/RingBuffer.hpp"
#include "../Includes/FasterDefs.h"
#include <string>
#include <vector>
#include <memory>
#include <cmath>

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

namespace futu {

//==============================================================================
// Spread Calculator Configuration
//==============================================================================

struct SpreadCalculatorConfig
{
    uint32_t window_size;           ///< Rolling window for statistics
    uint32_t min_samples;           ///< Minimum samples for valid stats
    double ema_alpha;               ///< EMA smoothing factor
    bool use_robust_stats;          ///< Use robust statistics (median-based)
    
    SpreadCalculatorConfig()
        : window_size(200)
        , min_samples(30)
        , ema_alpha(0.1)
        , use_robust_stats(false)
    {}
};

//==============================================================================
// Spread Calculator
//==============================================================================

/// Calculates spread values and statistics for a single spread pair
class SpreadCalculator
{
public:
    SpreadCalculator();
    ~SpreadCalculator() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const SpreadCalculatorConfig& config) { _config = config; }
    const SpreadCalculatorConfig& getConfig() const { return _config; }
    
    void setSpreadType(SpreadType type) { _spread_type = type; }
    void setLegRatios(double leg1_ratio, double leg2_ratio);
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// Update with new tick data
    void onTick(const std::string& code, double price, double multiplier, uint64_t timestamp);
    
    /// Update with WTSTickData
    void onLeg1Tick(double price, uint64_t timestamp);
    void onLeg2Tick(double price, uint64_t timestamp);
    
    //==========================================================================
    // Spread Calculation
    //==========================================================================
    
    /// Calculate spread value from two prices
    double calculateSpread(double price1, double price2) const;
    
    /// Get current spread value
    double getCurrentSpread() const { return _current_spread; }
    
    /// Get current state
    SpreadState getState() const;
    
    //==========================================================================
    // Statistics
    //==========================================================================
    
    /// Get rolling mean
    double getMean() const { return _spread_mean; }
    
    /// Get rolling standard deviation
    double getStdDev() const { return _spread_std; }
    
    /// Get current Z-Score
    double getZScore() const { return _zscore; }
    
    /// Get correlation between legs
    double getCorrelation() const { return _correlation; }
    
    /// Get beta (hedge ratio) - smoothed by EMA for stability
    double getBeta() const { return _smoothed_beta; }
    
    /// Get raw beta (without EMA smoothing)
    double getRawBeta() const { return _beta; }
    
    /// Get half-life of mean reversion (in updates)
    double getHalfLife() const { return _half_life; }
    
    //==========================================================================
    // Advanced Analysis
    //==========================================================================
    
    /// Calculate Pearson correlation
    double calculateCorrelation() const;
    
    /// Calculate linear regression beta
    double calculateBeta() const;
    
    /// Estimate half-life using Ornstein-Uhlenbeck process
    double estimateHalfLife() const;
    
    /// Check if spread is mean-reverting (ADF test approximation)
    bool isMeanReverting() const;
    
    //==========================================================================
    // State Management
    //==========================================================================
    
    /// Reset all state
    void reset();
    
    /// Get sample count
    size_t getSampleCount() const { return _welford_n; }
    
    /// Check if has enough samples
    bool hasEnoughSamples() const { return _welford_n >= _config.min_samples; }
    
private:
    //==========================================================================
    // Internal Methods
    //==========================================================================
    
    void updateStatistics();
    void updateCorrelation();
    double calculateRobustStd(const RingBuffer<double, 256>& data) const;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    SpreadCalculatorConfig _config;
    SpreadType _spread_type;
    double _leg1_ratio;
    double _leg2_ratio;
    
    //==========================================================================
    // Price Data
    //==========================================================================
    
    double _leg1_price;
    double _leg2_price;
    double _leg1_multiplier;
    double _leg2_multiplier;
    uint64_t _last_leg1_update;
    uint64_t _last_leg2_update;
    bool _leg1_fresh;    // BUG-7: leg1有新数据标记，用于tick同步配对
    bool _leg2_fresh;    // BUG-7: leg2有新数据标记，用于tick同步配对
    
    //==========================================================================
    // Spread Data
    //==========================================================================
    
    double _current_spread;
    RingBuffer<double, 256> _spread_history;  // Power of 2 for RingBuffer
    
    //==========================================================================
    // Statistics
    //==========================================================================
    
    double _spread_mean;
    double _spread_std;
    double _zscore;
    double _ema_spread;
    
    // Correlation / Beta
    RingBuffer<double, 256> _leg1_history;
    RingBuffer<double, 256> _leg2_history;
    double _correlation;
    double _beta;
    double _smoothed_beta;      ///< EMA smoothed beta for stable hedge ratio
    mutable double _alpha;  // mutable for modification in const calculateBeta()
    double _half_life;
    
    //==========================================================================
    // Welford Online Algorithm State (for O(1) mean/variance updates)
    //==========================================================================
    
    double _welford_m;     ///< Running mean
    double _welford_s;     ///< Running sum of squared deviations
    uint32_t _welford_n;   ///< Sample count for Welford algorithm
    
    //==========================================================================
    // State
    //==========================================================================
    
    uint64_t _last_update;
    bool _initialized;
};

//==============================================================================
// Multi-Spread Calculator Manager
//==============================================================================

/// Manages multiple spread calculators
class SpreadCalculatorManager
{
public:
    SpreadCalculatorManager();
    ~SpreadCalculatorManager() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const SpreadCalculatorConfig& config) { _config = config; }
    
    /// Add a spread pair
    void addSpreadPair(const SpreadPairConfig& pair_config);
    
    /// Remove a spread pair
    void removeSpreadPair(const std::string& pair_id);
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// Update with tick for any contract
    void onTick(const std::string& code, double price, double multiplier, uint64_t timestamp);
    
    //==========================================================================
    // State Access
    //==========================================================================
    
    /// Get spread state for a pair
    SpreadState getSpreadState(const std::string& pair_id) const;
    
    /// Get all spread states
    std::vector<SpreadState> getAllStates() const;
    
    /// Get calculator for a pair
    SpreadCalculator* getCalculator(const std::string& pair_id);
    
    //==========================================================================
    // Lookup
    //==========================================================================
    
    /// Check if contract is part of any spread pair
    bool isSpreadContract(const std::string& code) const;
    
    /// Get all pairs containing a contract
    std::vector<std::string> getPairsForContract(const std::string& code) const;
    
    //==========================================================================
    // Management
    //==========================================================================
    
    void reset();
    size_t getPairCount() const { return _calculators.size(); }
    
private:
    SpreadCalculatorConfig _config;
    
    wtp::wt_hashmap<std::string, std::unique_ptr<SpreadCalculator>> _calculators;
    
    // Mapping from contract code to spread pair IDs
    wtp::wt_hashmap<std::string, std::vector<std::string>> _contract_to_pairs;
    
    // Pair configurations for lookup
    wtp::wt_hashmap<std::string, SpreadPairConfig> _pair_configs;
};

} // namespace futu
