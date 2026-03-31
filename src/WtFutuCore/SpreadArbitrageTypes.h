/*!
 * \file SpreadArbitrageTypes.h
 * \brief Cross-Term Spread Arbitrage Type Definitions
 * 
 * Defines core types for spread arbitrage strategies:
 *   - Spread types and calculations
 *   - Signal and position structures
 *   - Strategy enumerations
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace futu {

//==============================================================================
// Spread Type Enumeration
//==============================================================================

/// Spread calculation method
enum class SpreadType : uint8_t
{
    SIMPLE_DIFF,        ///< Simple difference: P1 - P2
    RATIO,              ///< Price ratio: P1 / P2
    LOG_DIFF,           ///< Log difference: log(P1) - log(P2)
    WEIGHTED,           ///< Weighted spread: w1*P1 - w2*P2 (considering multipliers)
    BASIS               ///< Basis: Futures - Spot (for futures-spot arbitrage)
};

//==============================================================================
// Arbitrage Strategy Enumeration
//==============================================================================

/// Strategy types for spread arbitrage
enum class ArbitrageStrategy : uint8_t
{
    MEAN_REVERSION,      ///< Strategy A: Mean reversion
    TREND_FOLLOWING,     ///< Strategy B: Trend following
    PAIRS_TRADING,       ///< Strategy C: Pairs trading (cointegration)
    STATISTICAL_ARB,     ///< Strategy D: Statistical arbitrage
    MARKET_MAKING,       ///< Strategy E: Market making enhancement
    HYBRID               ///< Strategy F: Hybrid approach
};

//==============================================================================
// Signal Type Enumeration
//==============================================================================

/// Spread trading signal type
enum class SpreadSignalType : uint8_t
{
    NONE,                   ///< No signal
    OPEN_LONG_SPREAD,       ///< Open long spread (buy leg1, sell leg2)
    OPEN_SHORT_SPREAD,      ///< Open short spread (sell leg1, buy leg2)
    CLOSE_LONG_SPREAD,      ///< Close long spread
    CLOSE_SHORT_SPREAD,     ///< Close short spread
    STOP_LOSS,              ///< Stop loss exit
    TIMEOUT_EXIT,           ///< Timeout forced exit
    REBALANCE,              ///< Rebalance position
    PAUSE_QUOTING,          ///< Pause one-sided quoting
    RESUME_QUOTING          ///< Resume normal quoting
};

//==============================================================================
// Spread Pair Configuration
//==============================================================================

/// Configuration for a spread pair
struct SpreadPairConfig
{
    std::string pair_id;            ///< Unique pair identifier
    std::string leg1_code;          ///< Near-term contract code
    std::string leg2_code;          ///< Far-term contract code
    
    SpreadType spread_type;         ///< Spread calculation method
    double leg1_multiplier;         ///< Leg1 contract multiplier
    double leg2_multiplier;         ///< Leg2 contract multiplier
    double leg1_ratio;              ///< Leg1 hedge ratio
    double leg2_ratio;              ///< Leg2 hedge ratio
    
    // Mean reversion parameters
    double entry_z_threshold;       ///< Entry Z-Score threshold
    double exit_z_threshold;        ///< Exit Z-Score threshold
    double stop_loss_z;             ///< Stop loss Z-Score
    
    // Trend following parameters
    uint32_t trend_ma_fast;         ///< Fast MA period
    uint32_t trend_ma_slow;         ///< Slow MA period
    double min_trend_strength;      ///< Minimum trend strength
    
    // Position limits
    double max_spread_position;     ///< Maximum spread position
    double max_single_leg;          ///< Maximum single leg position
    
    // Time parameters
    uint32_t lookback_window;       ///< Statistical window size
    uint32_t convergence_timeout;   ///< Convergence timeout (seconds)
    uint32_t expiry_threshold;      ///< Days to expiry warning
    
    // Strategy selection
    ArbitrageStrategy primary_strategy;   ///< Primary strategy
    bool use_hybrid;                ///< Use hybrid approach
    
    // Market making enhancement
    bool enhance_quoting;           ///< Enhance quoting with spread signal
    double pause_z_threshold;       ///< Z-Score to pause quoting
    double widen_z_threshold;       ///< Z-Score to widen spread
    
    SpreadPairConfig()
        : spread_type(SpreadType::WEIGHTED)
        , leg1_multiplier(300.0)
        , leg2_multiplier(300.0)
        , leg1_ratio(1.0)
        , leg2_ratio(1.0)
        , entry_z_threshold(2.0)
        , exit_z_threshold(0.5)
        , stop_loss_z(4.0)
        , trend_ma_fast(20)
        , trend_ma_slow(60)
        , min_trend_strength(0.001)
        , max_spread_position(20.0)
        , max_single_leg(30.0)
        , lookback_window(200)
        , convergence_timeout(3600)
        , expiry_threshold(7)
        , primary_strategy(ArbitrageStrategy::MEAN_REVERSION)
        , use_hybrid(false)
        , enhance_quoting(true)
        , pause_z_threshold(3.0)
        , widen_z_threshold(1.5)
    {}
};

//==============================================================================
// Spread State
//==============================================================================

/// Current state of a spread pair
struct SpreadState
{
    std::string pair_id;            ///< Pair identifier
    
    // Contract codes for expiry lookup
    std::string leg1_code;          ///< Leg1 contract code (e.g. "CFFEX.IF.2503")
    std::string leg2_code;          ///< Leg2 contract code (e.g. "CFFEX.IF.2506")
    
    // Price data
    double leg1_price;              ///< Current leg1 price
    double leg2_price;              ///< Current leg2 price
    double current_spread;          ///< Current spread value
    
    // Statistics
    double spread_mean;             ///< Rolling mean
    double spread_std;              ///< Rolling standard deviation
    double zscore;                  ///< Current Z-Score
    
    // Correlation & Cointegration
    double correlation;             ///< Pearson correlation
    double beta;                    ///< Hedge ratio (cointegration coefficient)
    double half_life;               ///< Mean reversion half-life (seconds)
    
    // Position
    double leg1_position;           ///< Current leg1 position
    double leg2_position;           ///< Current leg2 position
    double spread_position;         ///< Net spread position
    
    // PnL
    double unrealized_pnl;          ///< Unrealized PnL
    double realized_pnl;            ///< Realized PnL
    double entry_spread;            ///< Spread at entry
    
    // Timing
    uint64_t last_update;           ///< Last update timestamp
    uint64_t position_open_time;    ///< Position open timestamp
    
    // Flags
    bool is_active;                 ///< Is actively trading
    bool is_converging;             ///< Is spread converging
    
    SpreadState()
        : leg1_price(0), leg2_price(0)
        , current_spread(0)
        , spread_mean(0), spread_std(0), zscore(0)
        , correlation(0), beta(1.0), half_life(0)
        , leg1_position(0), leg2_position(0), spread_position(0)
        , unrealized_pnl(0), realized_pnl(0), entry_spread(0)
        , last_update(0), position_open_time(0)
        , is_active(false), is_converging(false)
    {}
    
    /// Check if has open position
    inline bool hasPosition() const { return std::abs(spread_position) > 0.001; }
    
    /// Get position duration in seconds
    inline uint64_t positionDuration(uint64_t now) const
    {
        return hasPosition() ? (now - position_open_time) / 1000 : 0;
    }
};

//==============================================================================
// Spread Signal
//==============================================================================

/// Trading signal for spread arbitrage
struct SpreadSignal
{
    SpreadSignalType type;          ///< Signal type
    std::string pair_id;            ///< Target spread pair
    
    double confidence;              ///< Signal confidence (0-1)
    double suggested_size;          ///< Suggested position size
    
    double entry_zscore;            ///< Z-Score at signal generation
    double target_zscore;           ///< Target Z-Score for exit
    
    // Leg information
    std::string leg1_code;          ///< Leg1 contract code
    std::string leg2_code;          ///< Leg2 contract code
    double leg1_price;              ///< Suggested leg1 price
    double leg2_price;              ///< Suggested leg2 price
    double leg1_qty;                ///< Suggested leg1 quantity
    double leg2_qty;                ///< Suggested leg2 quantity
    
    ArbitrageStrategy source;       ///< Strategy that generated signal
    std::string reason;             ///< Human-readable reason
    
    uint64_t timestamp;             ///< Signal timestamp
    
    SpreadSignal()
        : type(SpreadSignalType::NONE)
        , confidence(0), suggested_size(0)
        , entry_zscore(0), target_zscore(0)
        , leg1_price(0), leg2_price(0)
        , leg1_qty(0), leg2_qty(0)
        , source(ArbitrageStrategy::MEAN_REVERSION)
        , timestamp(0)
    {}
    
    /// Check if signal is actionable
    inline bool isActionable() const
    {
        return type != SpreadSignalType::NONE && confidence > 0.1;
    }
};

//==============================================================================
// Risk Metrics
//==============================================================================

/// Risk metrics for a spread position
struct SpreadRiskMetrics
{
    double var_99;                  ///< 99% Value at Risk
    double max_loss;                ///< Maximum potential loss
    double leg1_exposure;           ///< Leg1 exposure
    double leg2_exposure;           ///< Leg2 exposure
    double net_exposure;            ///< Net exposure after hedging
    double convergence_risk;        ///< Probability of non-convergence
    
    double correlation_risk;        ///< Risk from correlation breakdown
    double beta_instability;        ///< Risk from beta instability
    double liquidity_risk;          ///< Risk from low liquidity
    
    uint32_t days_to_expiry_leg1;   ///< Days to expiry for leg1
    uint32_t days_to_expiry_leg2;   ///< Days to expiry for leg2
    
    SpreadRiskMetrics()
        : var_99(0), max_loss(0)
        , leg1_exposure(0), leg2_exposure(0), net_exposure(0)
        , convergence_risk(0)
        , correlation_risk(0), beta_instability(0), liquidity_risk(0)
        , days_to_expiry_leg1(0), days_to_expiry_leg2(0)
    {}
};

//==============================================================================
// Quoting Adjustment
//==============================================================================

/// Adjustment to quoting parameters based on spread signal
struct QuotingAdjustment
{
    double bid_skew_adjustment;     ///< Adjustment to bid skew
    double ask_skew_adjustment;     ///< Adjustment to ask skew
    double spread_multiplier;       ///< Spread multiplier
    
    bool suppress_bid;              ///< Suppress bid quoting
    bool suppress_ask;              ///< Suppress ask quoting
    
    double confidence;              ///< Adjustment confidence
    
    QuotingAdjustment()
        : bid_skew_adjustment(0)
        , ask_skew_adjustment(0)
        , spread_multiplier(1.0)
        , suppress_bid(false)
        , suppress_ask(false)
        , confidence(0)
    {}
};

//==============================================================================
// Feature Vector for Statistical Strategies
//==============================================================================

/// Feature vector for machine learning / statistical analysis
struct SpreadFeatureVector
{
    double zscore;                  ///< Current Z-Score
    double zscore_change;           ///< Z-Score change rate
    double correlation;             ///< Current correlation
    double correlation_change;      ///< Correlation change rate
    double beta;                    ///< Current beta
    double beta_stability;          ///< Beta stability metric
    double volatility_ratio;        ///< Volatility ratio (leg1/leg2)
    double volume_imbalance;        ///< Volume imbalance
    double cost_of_carry;           ///< Cost of carry
    double momentum;                ///< Price momentum
    double mean_reversion_speed;    ///< Mean reversion speed
    
    SpreadFeatureVector()
        : zscore(0), zscore_change(0)
        , correlation(0), correlation_change(0)
        , beta(1), beta_stability(1)
        , volatility_ratio(1)
        , volume_imbalance(0)
        , cost_of_carry(0)
        , momentum(0)
        , mean_reversion_speed(0)
    {}
};

//==============================================================================
// Strategy Weights for Hybrid Mode
//==============================================================================

/// Weights for hybrid strategy signal combination
struct StrategyWeights
{
    double mean_reversion;          ///< Weight for mean reversion
    double trend_following;         ///< Weight for trend following
    double pairs_trading;           ///< Weight for pairs trading
    double statistical_arb;         ///< Weight for statistical arb
    
    StrategyWeights()
        : mean_reversion(0.4)
        , trend_following(0.2)
        , pairs_trading(0.2)
        , statistical_arb(0.2)
    {}
    
    /// Normalize weights to sum to 1
    void normalize()
    {
        double sum = mean_reversion + trend_following + pairs_trading + statistical_arb;
        if (sum > 0)
        {
            mean_reversion /= sum;
            trend_following /= sum;
            pairs_trading /= sum;
            statistical_arb /= sum;
        }
    }
};

} // namespace futu
