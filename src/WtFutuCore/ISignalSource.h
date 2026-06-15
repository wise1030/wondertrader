/*!
 * \file ISignalSource.h
 * \brief Signal Source Interface for Market Making
 * 
 * Provides a plugin architecture for signal sources:
 *   - OFI (Order Flow Imbalance)
 *   - Volatility (Realized + Microstructure)
 *   - Trade Flow
 *   - Alpha
 *   - Market State
 * 
 * Design Principles:
 *   - Single data source: MarketDataContext
 *   - Plugin architecture: each signal source implements ISignalSource
 *   - Configuration driven: enable/disable signals via config
 *   - Unified Context: SignalContext as the single source of truth
 */
#pragma once

#include <string>
#include <cstdint>
#include <vector>

#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

namespace futu {

class MarketDataContext;
struct SignalContext;

//==============================================================================
// Enumerations
//==============================================================================

/// Signal source type enumeration
enum class SignalType : uint8_t
{
    OFI,            ///< Order Flow Imbalance
    VOLATILITY,     ///< Realized Volatility
    TRADE_FLOW,     ///< Trade Flow Analysis
    BOOK_IMBALANCE, ///< Book Imbalance (order book pressure)
    ALPHA,          ///< Composite Alpha Signal
    MARKET_STATE,   ///< Market State Detection
    TOXICITY,       ///< Toxicity Detection
    MOMENTUM,       ///< Momentum Signal
    LEAD_LAG,       ///< Lead-Lag Signal
    CUSTOM          ///< User-defined signal
};

/// Volatility tier for unified volatility-based decisions
enum class VolTier : uint8_t
{
    LOW,            ///< Low volatility (< 20th percentile)
    NORMAL,         ///< Normal volatility (20-60th percentile)
    ELEVATED,       ///< Elevated volatility (60-80th percentile)
    EXTREME         ///< Extreme volatility (> 80th percentile)
};

/// Market state classification
enum class MarketState : uint8_t
{
    NORMAL,         ///< Normal market conditions
    ELEVATED_VOL,   ///< High volatility detected
    FAST_MOVE,      ///< Fast price movement
    LOW_LIQUIDITY   ///< Low liquidity detected
};

//==============================================================================
// Signal Results (Data Structures)
//==============================================================================

/// Base class for signal results
struct SignalResult
{
    SignalType type;
    double confidence;      ///< Signal confidence [0, 1]
    uint64_t timestamp;
    bool valid;
    
    SignalResult(SignalType t) 
        : type(t), confidence(0.0), timestamp(0), valid(false) {}
    virtual ~SignalResult() = default;
};

/// OFI signal result
struct OFISignalResult : public SignalResult
{
    double ofi;                 ///< Normalized OFI [-1, 1]
    double bid_pressure;        ///< Buy pressure [0, 1]
    double ask_pressure;        ///< Sell pressure [0, 1]
    double cumulative_ofi;      ///< Cumulative OFI for trend
    
    OFISignalResult() 
        : SignalResult(SignalType::OFI)
        , ofi(0), bid_pressure(0.5), ask_pressure(0.5), cumulative_ofi(0) {}
};

/// Volatility signal result (Unified: price-based + microstructure)
struct VolatilitySignalResult : public SignalResult
{
    double volatility;          ///< Legacy field for compatibility (maps to realized_vol)
    double realized_vol;        ///< Price-based realized volatility (std dev)
    double composite_vol;       ///< Composite volatility (combined factors)
    VolTier vol_tier;           ///< Volatility tier for scaling
    double vol_percentile;      ///< Volatility percentile [0, 100]
    double vpin;                ///< Volume toxicity/volatility index
    
    VolatilitySignalResult() 
        : SignalResult(SignalType::VOLATILITY)
        , volatility(0), realized_vol(0), composite_vol(0), vol_tier(VolTier::NORMAL)
        , vol_percentile(50), vpin(0) {}
};

/// Trade flow signal result
struct TradeFlowSignalResult : public SignalResult
{
    double net_flow;            ///< Net trade flow (buy - sell)
    double net_flow_normalized; ///< Normalized net flow [-1, 1]
    double buy_volume;          ///< Total buy volume
    double sell_volume;         ///< Total sell volume
    double large_trade_ratio;   ///< Large trade ratio [0, 1]
    double avg_trade_size;      ///< Average trade size
    
    TradeFlowSignalResult() 
        : SignalResult(SignalType::TRADE_FLOW)
        , net_flow(0), net_flow_normalized(0)
        , buy_volume(0), sell_volume(0)
        , large_trade_ratio(0), avg_trade_size(0) {}
};

/// Book imbalance signal result (order book pressure)
struct BookImbalanceSignalResult : public SignalResult
{
    double simple_imbalance;     ///< Simple imbalance [-1, 1] (bid-ask vol)
    double depth_imbalance;      ///< Depth-weighted imbalance [-1, 1]
    double pressure_intensity;   ///< Pressure intensity [0, 1]
    bool bid_dominant;           ///< Bid side dominant
    bool ask_dominant;           ///< Ask side dominant
    double bid_depth;            ///< Total bid depth
    double ask_depth;            ///< Total ask depth
    
    BookImbalanceSignalResult() 
        : SignalResult(SignalType::BOOK_IMBALANCE)
        , simple_imbalance(0), depth_imbalance(0), pressure_intensity(0)
        , bid_dominant(false), ask_dominant(false)
        , bid_depth(0), ask_depth(0) {}
};

/// Alpha signal result
struct AlphaSignalResult : public SignalResult
{
    double alpha;                  ///< Composite alpha [-1, 1]
    double ofi_component;          ///< OFI contribution
    double trade_component;        ///< Trade flow contribution
    double book_imbalance_component; ///< Book imbalance contribution
    double momentum_component;     ///< Momentum contribution
    double lead_lag_component;     ///< Lead-lag contribution
    bool is_strong_signal;         ///< Strong signal flag
    
    AlphaSignalResult() 
        : SignalResult(SignalType::ALPHA)
        , alpha(0), ofi_component(0), trade_component(0)
        , book_imbalance_component(0), momentum_component(0), lead_lag_component(0)
        , is_strong_signal(false) {}
};

/// Market state signal result
struct MarketStateSignalResult : public SignalResult
{
    MarketState state;
    bool should_widen;          ///< Recommend widening spread
    bool should_hedge;          ///< Recommend immediate hedging
    bool should_pause;          ///< Recommend pausing quotes
    double vol_estimate;        ///< Volatility estimate
    double spread_estimate;     ///< Spread estimate (ticks)
    
    MarketStateSignalResult() 
        : SignalResult(SignalType::MARKET_STATE)
        , state(MarketState::NORMAL)
        , should_widen(false), should_hedge(false), should_pause(false)
        , vol_estimate(0), spread_estimate(0) {}
};

/// Toxicity signal result
struct ToxicitySignalResult : public SignalResult
{
    double toxicity_score;      ///< Toxicity score [0, 1]
    bool toxic_detected;        ///< Toxic flow detected
    double vpin;                ///< VPIN value
    int toxic_side;             ///< Toxic side (1=buy toxic, -1=sell toxic, 0=both/none)
    
    ToxicitySignalResult() 
        : SignalResult(SignalType::TOXICITY)
        , toxicity_score(0), toxic_detected(false), vpin(0), toxic_side(0) {}
};

//==============================================================================
// Signal Context (Unified State)
//==============================================================================

/// Signal context containing all signal results (Single Source of Truth)
struct SignalContext
{
    //========== Basic Market Data ==========
    std::string code;
    uint64_t timestamp = 0;
    double mid_price = 0;
    double spread = 0;
    double spread_ticks = 0;
    double tick_size = 0.1;
    double bid_price = 0;
    double ask_price = 0;
    double bid_vol = 0;
    double ask_vol = 0;
    
    //========== Order Book Signals (Restored) ==========
    double imbalance = 0;           ///< Simple imbalance [-1, 1]
    double depth_imbalance = 0;     ///< Depth-weighted imbalance [-1, 1]
    double bid_depth = 0;
    double ask_depth = 0;
    double liquidity_score = 0;     ///< [0, 1]
    
    //========== Aggregated Signal Results ==========
    OFISignalResult ofi;
    VolatilitySignalResult volatility;
    TradeFlowSignalResult trade_flow;
    BookImbalanceSignalResult book_imbalance;
    AlphaSignalResult alpha;
    MarketStateSignalResult market_state;
    ToxicitySignalResult toxicity;
    
    //========== Convenience Methods ==========
    
    /// Check if quoting should be paused (risk or toxicity)
    bool shouldPause() const {
        return market_state.should_pause || toxicity.toxic_detected;
    }
    
    /// Check if spread should be widened (volatility or state)
    bool shouldWiden() const { 
        return market_state.should_widen || volatility.vol_tier >= VolTier::ELEVATED; 
    }
    
    /// Check if immediate hedging is needed
    bool shouldHedge() const { 
        return market_state.should_hedge; 
    }
    
    /// Check for strong alpha signal
    bool isStrongSignal() const { 
        return alpha.is_strong_signal; 
    }
    
    /// Get composite volatility for scaling (P0-2.3)
    double getEffectiveVol() const {
        return volatility.composite_vol > 0 ? volatility.composite_vol : volatility.realized_vol;
    }

    void reset() {
        timestamp = 0;
        mid_price = spread = spread_ticks = 0;
        bid_price = ask_price = bid_vol = ask_vol = 0;
        imbalance = depth_imbalance = bid_depth = ask_depth = liquidity_score = 0;
        ofi = OFISignalResult();
        volatility = VolatilitySignalResult();
        trade_flow = TradeFlowSignalResult();
        book_imbalance = BookImbalanceSignalResult();  // 修复reset()遗漏book_imbalance
        alpha = AlphaSignalResult();
        market_state = MarketStateSignalResult();
        toxicity = ToxicitySignalResult();
    }
};

//==============================================================================
// Interfaces
//==============================================================================

/// Interface for signal sources
class ISignalSource
{
public:
    virtual ~ISignalSource() = default;
    
    /// Signal source name
    virtual const std::string& name() const = 0;
    
    /// Signal type
    virtual SignalType type() const = 0;
    
    /// Update signal from MarketDataContext (primary data source)
    virtual void update(const MarketDataContext& book) = 0;
    
    /// Update signal from full context (for secondary signals)
    virtual void updateWithContext(const SignalContext& ctx) { (void)ctx; }
    
    /// Get current signal result
    virtual const SignalResult& result() const = 0;
    
    /// Get alpha value directly (avoids dynamic_cast in hot path)
    virtual double getAlphaValue() const { return 0.0; }
    
    /// Status management
    virtual bool enabled() const = 0;
    virtual void setEnabled(bool enabled) = 0;
    virtual void reset() = 0;
};

/// Specialized interface for volatility calculation
class VolatilitySignalSource : public ISignalSource
{
public:
    virtual const VolatilitySignalResult& getVolatility() const = 0;
    virtual double getVolPercentile() const = 0;
};

} // namespace futu
