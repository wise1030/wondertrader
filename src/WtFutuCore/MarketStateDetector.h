/*!
 * \file MarketStateDetector.h
 * \brief Market State Detection for Market Making Protection
 * 
 * Detects adverse market conditions that require protective actions:
 *   - High volatility / fast moves
 *   - Low liquidity / wide spreads
 *   - Auction periods / market events
 *   - Directional price pressure
 *   - Circuit breakers / halts
 * 
 * Provides actionable signals for spread widening, quote pausing, or hedging.
 */
#pragma once

#include <string>
#include <vector>
#include <deque>
#include <cmath>
#include <cstdint>
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

namespace futu {

/// Market state classification
enum class MarketState
{
    NORMAL,             ///< Normal market conditions
    ELEVATED_VOL,       ///< Higher than normal volatility
    FAST_MOVE,          ///< Rapid directional price movement
    LOW_LIQUIDITY,      ///< Thin order book, wide spreads
    AUCTION,            ///< Pre-market / closing auction
    CIRCUIT_BREAKER,    ///< Trading halt or limit
    CLOSED              ///< Market closed
};

/// Market state names for logging
inline const char* marketStateName(MarketState state)
{
    switch (state)
    {
        case MarketState::NORMAL: return "NORMAL";
        case MarketState::ELEVATED_VOL: return "ELEVATED_VOL";
        case MarketState::FAST_MOVE: return "FAST_MOVE";
        case MarketState::LOW_LIQUIDITY: return "LOW_LIQUIDITY";
        case MarketState::AUCTION: return "AUCTION";
        case MarketState::CIRCUIT_BREAKER: return "CIRCUIT_BREAKER";
        case MarketState::CLOSED: return "CLOSED";
        default: return "UNKNOWN";
    }
}

/// Detection parameters
struct DetectionParams
{
    double      vol_threshold;      ///< Volatility threshold for elevated state
    double      move_threshold;     ///< Price move threshold for fast move (percent)
    double      spread_threshold;   ///< Spread threshold for low liquidity (ticks)
    double      volume_threshold;   ///< Minimum volume at best
    uint32_t    lookback_ticks;     ///< Lookback window for detection
    uint32_t    cooldown_ticks;     ///< Cooldown before returning to normal
    
    DetectionParams()
        : vol_threshold(0.003)
        , move_threshold(0.005)
        , spread_threshold(5.0)
        , volume_threshold(10.0)
        , lookback_ticks(50)
        , cooldown_ticks(20)
    {}
};

/// Detection result
struct DetectionResult
{
    MarketState state;              ///< Detected market state
    double      confidence;         ///< Confidence level (0-1)
    double      vol_estimate;       ///< Current volatility estimate
    double      spread_estimate;    ///< Current spread estimate
    double      momentum;           ///< Price momentum (positive = up, negative = down)
    bool        should_widen;       ///< Recommendation: widen spread
    bool        should_pause;       ///< Recommendation: pause quoting
    bool        should_hedge;       ///< Recommendation: hedge immediately
    
    DetectionResult()
        : state(MarketState::NORMAL)
        , confidence(0.5)
        , vol_estimate(0)
        , spread_estimate(0)
        , momentum(0)
        , should_widen(false)
        , should_pause(false)
        , should_hedge(false)
    {}
};

/// Market State Detector
class MarketStateDetector
{
public:
    MarketStateDetector();
    ~MarketStateDetector() {}
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setParams(const DetectionParams& params) { _params = params; }
    const DetectionParams& getParams() const { return _params; }
    
    void setContract(const std::string& code, double tickSize)
    {
        _code = code;
        _tick_size = tickSize;
    }
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// Process incoming tick
    void onTick(wtp::WTSTickData* tick);
    
    /// Process session event
    void onSessionBegin(uint32_t tradingDay);
    void onSessionEnd(uint32_t tradingDay);
    
    //==========================================================================
    // State Detection
    //==========================================================================
    
    /// Detect current market state
    DetectionResult detect() const;
    
    /// Get current state (cached)
    MarketState getCurrentState() const { return _current_state; }
    
    //==========================================================================
    // Individual Detectors
    //==========================================================================
    
    /// Check for elevated volatility
    bool isVolatilityElevated(double& volEstimate) const;
    
    /// Check for fast price move
    bool isFastMove(double& moveEstimate) const;
    
    /// Check for low liquidity
    bool isLowLiquidity(double& spreadEstimate) const;
    
    /// Check if in auction period
    bool isAuctionPeriod() const;
    
    //==========================================================================
    // Metrics
    //==========================================================================
    
    /// Get price momentum (-1 to 1)
    double getMomentum() const;
    
    /// Get current spread in ticks
    double getSpreadInTicks() const;
    
    /// Get time since last state change
    uint32_t getTicksSinceChange() const { return _ticks_since_change; }
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void reset();

private:
    std::string _code;
    double _tick_size;
    DetectionParams _params;
    
    // Price history
    std::deque<double> _price_history;
    std::deque<double> _spread_history;
    std::deque<double> _volume_history;
    
    // Current state
    MarketState _current_state;
    uint32_t _ticks_in_state;
    uint32_t _ticks_since_change;
    
    // Cached detection
    mutable DetectionResult _cached_result;
    mutable bool _cache_dirty;
    
    // Session tracking
    uint32_t _trading_day;
    bool _in_session;
};

} // namespace futu
