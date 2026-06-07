/*!
 * \file TrendFollowingStrategy.h
 * \brief Trend Following Strategy for Spread Arbitrage
 * 
 * Strategy Logic:
 *   - Detect trend using MA crossover
 *   - Follow trend on momentum confirmation
 *   - Exit on trend reversal signal
 *   - Position size scales with trend strength
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
// Trend Following Configuration
//==============================================================================

struct TrendFollowingConfig
{
    uint32_t fast_ma_period;        ///< Fast MA period
    uint32_t slow_ma_period;        ///< Slow MA period
    uint32_t signal_period;         ///< Signal line period
    
    double min_trend_strength;      ///< Minimum trend strength for entry
    double max_adx;                 ///< Maximum ADX for range filter
    
    double entry_threshold;         ///< MA crossover threshold
    double exit_threshold;          ///< Exit threshold (MA re-cross)
    
    double max_position;            ///< Maximum position size
    double base_qty;                ///< Base position size
    
    double stop_loss_pct;           ///< Stop loss percentage, default 2%
    uint32_t max_trend_bars;        ///< Max bars in trend before exhaustion exit
    
    uint32_t confirmation_bars;     ///< Bars to confirm trend
    bool use_volume_filter;         ///< Filter by volume
    
    TrendFollowingConfig()
        : fast_ma_period(20)
        , slow_ma_period(60)
        , signal_period(10)
        , min_trend_strength(0.001)
        , max_adx(50)
        , entry_threshold(0.0)
        , exit_threshold(0.0)
        , max_position(15.0)
        , base_qty(1.0)
        , stop_loss_pct(0.02)
        , max_trend_bars(50)
        , confirmation_bars(3)
        , use_volume_filter(false)
    {}
};

//==============================================================================
// Trend State
//==============================================================================

struct TrendState
{
    double fast_ma;                 ///< Current fast MA value
    double slow_ma;                 ///< Current slow MA value
    double ma_diff;                 ///< Fast - Slow difference
    double ma_diff_pct;             ///< MA difference percentage
    double trend_strength;          ///< Trend strength (slope)
    double momentum;                ///< Momentum indicator
    
    int trend_direction;            ///< 1 = uptrend, -1 = downtrend, 0 = neutral
    int bars_in_trend;              ///< Number of bars in current trend
    
    double entry_price;             ///< Entry price for stop loss calculation
    
    bool is_strong_trend;           ///< Is trend strong enough for entry
    bool is_trend_reversal;         ///< Is trend reversing
    
    TrendState()
        : fast_ma(0), slow_ma(0)
        , ma_diff(0), ma_diff_pct(0)
        , trend_strength(0), momentum(0)
        , trend_direction(0)
        , bars_in_trend(0)
        , entry_price(0)
        , is_strong_trend(false)
        , is_trend_reversal(false)
    {}
};

//==============================================================================
// Trend Following Strategy
//==============================================================================

class TrendFollowingStrategy
{
public:
    TrendFollowingStrategy();
    ~TrendFollowingStrategy() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const TrendFollowingConfig& config) { _config = config; }
    const TrendFollowingConfig& getConfig() const { return _config; }
    
    //==========================================================================
    // Data Update
    //==========================================================================
    
    /// Update with new spread value
    void updateSpread(double spread, uint64_t timestamp);
    
    //==========================================================================
    // Signal Generation
    //==========================================================================
    
    /// Generate trading signal based on current state
    SpreadSignal generateSignal(const SpreadState& state, uint64_t current_time);
    
    //==========================================================================
    // Analysis
    //==========================================================================
    
    /// Get current trend state
    TrendState getTrendState() const { return _trend_state; }
    
    /// Get trend direction (-1, 0, 1)
    int getTrendDirection() const { return _trend_state.trend_direction; }
    
    //==========================================================================
    // State
    //==========================================================================
    
    void reset();
    static constexpr const char* getName() { return "TrendFollowing"; }
    
private:
    //==========================================================================
    // Internal Methods
    //==========================================================================
    
    void updateTrendState();
    double calculateMA(const RingBuffer<double, 128>& data, uint32_t period) const;
    double calculateTrendStrength() const;
    int detectTrendDirection() const;
    bool isConfirmedTrend() const;
    double calculatePositionSize(const TrendState& trend) const;
    double calculateConfidence(const TrendState& trend) const;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    TrendFollowingConfig _config;
    
    //==========================================================================
    // Data History
    //==========================================================================
    
    RingBuffer<double, 128> _spread_history;  // Power of 2
    uint64_t _last_update;
    
    //==========================================================================
    // Moving Averages
    //==========================================================================
    
    double _fast_ma;
    double _slow_ma;
    double _prev_fast_ma;
    double _prev_slow_ma;
    
    //==========================================================================
    // Trend State
    //==========================================================================
    
    TrendState _trend_state;
    int _prev_trend_direction;
    uint32_t _bars_in_current_trend;
    uint64_t _last_signal_time;
};

} // namespace futu
