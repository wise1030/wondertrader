/*!
 * \file TickTransactionInferer.h
 * \brief Tick-Level Transaction Inference for Markets Without L2 Transaction Data
 * 
 * Infers trade direction and volume from tick-level order book snapshots.
 * Designed for Chinese futures market where transaction-level data is not available.
 * 
 * Inference Methods:
 *   1. Bid/Ask Depletion: Volume decrease on one side indicates aggressive trades
 *   2. Price Movement: Price up = buy-initiated, Price down = sell-initiated
 *   3. Spread Cross: Last price crossing spread indicates aggressive direction
 */
#pragma once

#include <string>
#include <cstdint>
#include "../Includes/WTSMarcos.h"
#include "../Share/RingBuffer.hpp"

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

namespace futu {

//==============================================================================
// Inferred Transaction Data
//==============================================================================

/// Method used to infer transaction direction
enum class InferenceMethod : uint8_t
{
    BID_DEPLETION = 0,      ///< Buy-side volume decreased (sell-initiated)
    ASK_DEPLETION = 1,      ///< Sell-side volume decreased (buy-initiated)
    PRICE_UP = 2,           ///< Price moved up (buy-initiated)
    PRICE_DOWN = 3,         ///< Price moved down (sell-initiated)
    SPREAD_CROSS_UP = 4,    ///< Last price crossed to ask side (buy-initiated)
    SPREAD_CROSS_DOWN = 5,  ///< Last price crossed to bid side (sell-initiated)
    VOLUME_SURGE = 6,       ///< Large volume change detected
    UNKNOWN = 7             ///< Cannot determine direction
};

/// Inferred transaction result
struct InferredTransaction
{
    double      price;              ///< Transaction price (last price from tick)
    double      volume;             ///< Inferred volume
    bool        is_buy_initiated;   ///< True if buy-initiated (aggressive buy)
    bool        is_sell_initiated;  ///< True if sell-initiated (aggressive sell)
    double      confidence;         ///< Confidence of inference [0, 1]
    uint64_t    timestamp;          ///< Timestamp
    
    InferenceMethod method;         ///< Method used for inference
    
    InferredTransaction()
        : price(0), volume(0)
        , is_buy_initiated(false), is_sell_initiated(false)
        , confidence(0), timestamp(0)
        , method(InferenceMethod::UNKNOWN)
    {}
};

//==============================================================================
// Accumulated Statistics
//==============================================================================

/// Accumulated trade flow statistics
struct AccumulatedFlowStats
{
    double      buy_volume;         ///< Accumulated inferred buy volume
    double      sell_volume;        ///< Accumulated inferred sell volume
    double      net_flow;           ///< Net flow (buy - sell)
    double      imbalance_ratio;    ///< Normalized imbalance [-1, 1]
    uint64_t    window_start;       ///< Window start timestamp
    uint32_t    tick_count;         ///< Number of ticks in window
    
    AccumulatedFlowStats()
        : buy_volume(0), sell_volume(0), net_flow(0)
        , imbalance_ratio(0), window_start(0), tick_count(0)
    {}
    
    void reset()
    {
        buy_volume = 0;
        sell_volume = 0;
        net_flow = 0;
        imbalance_ratio = 0;
        window_start = 0;
        tick_count = 0;
    }
};

//==============================================================================
// Trade Imbalance Result (Compatible with MicroAlphaEngine)
//==============================================================================

/// Trade imbalance result for integration with MicroAlphaEngine
struct InferredTradeImbalance
{
    double      net_flow;           ///< Net trade flow (buy - sell volume)
    double      imbalance_ratio;    ///< Normalized imbalance [-1, 1]
    double      large_trade_ratio;  ///< Ratio of large trades
    double      confidence;         ///< Average confidence of inference
    uint64_t    timestamp;
    
    InferredTradeImbalance()
        : net_flow(0), imbalance_ratio(0)
        , large_trade_ratio(0), confidence(0), timestamp(0)
    {}
};

//==============================================================================
// Configuration
//==============================================================================

struct TickInfererConfig
{
    uint32_t    imbalance_window_ms;    ///< Time window for imbalance calculation (ms)
    double      min_confidence;         ///< Minimum confidence threshold
    bool        use_volume_weighting;   ///< Use confidence-weighted volume
    double      large_trade_threshold;  ///< Volume threshold for large trade
    double      tick_size;              ///< Contract tick size
    
    TickInfererConfig()
        : imbalance_window_ms(5000)
        , min_confidence(0.3)
        , use_volume_weighting(true)
        , large_trade_threshold(50.0)
        , tick_size(1.0)
    {}
};

//==============================================================================
// Tick Transaction Inferer
//==============================================================================

/// Tick-Level Transaction Inference Engine
class TickTransactionInferer
{
public:
    TickTransactionInferer();
    ~TickTransactionInferer() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const TickInfererConfig& config) { _config = config; }
    const TickInfererConfig& getConfig() const { return _config; }
    
    void setContract(const std::string& code) { _code = code; }
    const std::string& getContract() const { return _code; }
    
    void setTickSize(double tickSize) { _config.tick_size = tickSize; }
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// Infer transaction from tick data
    InferredTransaction inferFromTick(
        double bid_px, double ask_px,
        double bid_vol, double ask_vol,
        double last_price, double last_vol,
        uint64_t timestamp
    );
    
    /// Infer from WTSTickData
    InferredTransaction inferFromTick(wtp::WTSTickData* tick);
    
    //==========================================================================
    // Analysis
    //==========================================================================
    
    /// Get inferred trade imbalance (compatible with MicroAlphaEngine)
    InferredTradeImbalance getInferredImbalance() const;
    
    /// Get accumulated flow statistics
    AccumulatedFlowStats getFlowStats() const;
    
    /// Get current imbalance ratio
    double getImbalanceRatio() const;
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void reset();
    void resetAccumulation();
    
private:
    std::string _code;
    TickInfererConfig _config;
    
    // Previous tick state
    struct TickState {
        double bid_px;
        double ask_px;
        double bid_vol;
        double ask_vol;
        double last_price;
        uint64_t timestamp;
        bool initialized;
        
        TickState()
            : bid_px(0), ask_px(0), bid_vol(0), ask_vol(0)
            , last_price(0), timestamp(0), initialized(false)
        {}
    };
    TickState _prev_state;
    
    // Inference history for rolling window
    struct InferenceRecord {
        double signed_volume;   // Positive = buy, Negative = sell
        double confidence;
        bool is_large;
        uint64_t timestamp;
    };
    RingBuffer<InferenceRecord, 256> _inference_history;
    
    // Accumulated statistics
    AccumulatedFlowStats _accumulated;
    double _large_volume;
    double _total_volume;
    
    //==========================================================================
    // Internal Methods
    //==========================================================================
    
    /// Detect inference method from price/volume changes
    InferenceMethod detectMethod(
        double bid_px, double ask_px,
        double prev_bid_px, double prev_ask_px,
        double last_price, double prev_last_price
    );
    
    /// Calculate confidence based on method and data quality
    double calculateConfidence(
        InferenceMethod method,
        double bid_vol_change, double ask_vol_change,
        double price_change
    );
    
    /// Calculate inferred volume from changes
    double calculateInferredVolume(
        double bid_vol, double prev_bid_vol,
        double ask_vol, double prev_ask_vol,
        double last_vol
    );
    
    /// Update accumulated statistics
    void updateAccumulation(const InferredTransaction& trans);
    
    /// Prune old records from history
    void pruneHistory(uint64_t current_time);
};

} // namespace futu
