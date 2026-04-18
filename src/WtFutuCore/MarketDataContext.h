/*!
 * \file MarketDataContext.h
 * \brief Order Book Depth Analysis & Market Data Snapshot for Market Making
 * 
 * Analyzes order book dynamics for:
 *   - Imbalance detection and direction prediction
 *   - Depth quality assessment
 *   - Liquidity estimation
 *   - Trade flow analysis
 * 
 * Architectural Note (Data Source Bifurcation):
 *   This file now cleanly separates Static OrderBook State and Dynamic TradeFlow.
 *   `MarketDataContext` acts as a Unified Market Data Context (Facade)
 *   providing O(1) inline access for Signal Sources.
 */
#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"
#include "TickTransactionInferer.h"

NS_WTP_BEGIN
class WTSTickData;
class WTSOrdQueData;
class WTSOrdDtlData;
class WTSTransData;
NS_WTP_END

namespace futu {

//==============================================================================
// Basic Data Structures
//==============================================================================

/// Order book level
struct BookLevel
{
    double price;
    double volume;
    double orders;  // Number of orders at this level
    
    BookLevel() : price(0), volume(0), orders(0) {}
};

/// Order book snapshot
struct OrderBookSnapshot
{
    std::string code;
    uint64_t timestamp;
    
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
    
    double mid_price;
    double spread;
    double imbalance;       // -1 (heavy ask) to +1 (heavy bid)
    double depth_imbalance; // Weighted by price distance
    double bid_depth;       // Total bid volume
    double ask_depth;       // Total ask volume
    
    OrderBookSnapshot()
        : timestamp(0), mid_price(0), spread(0)
        , imbalance(0), depth_imbalance(0)
        , bid_depth(0), ask_depth(0)
    {}
};

/// Trade flow analysis
struct TradeFlowAnalysis
{
    double net_flow;          ///< Net trade flow (buy - sell volume)
    double buy_pressure;      ///< Estimated buy pressure (0-1)
    double sell_pressure;     ///< Estimated sell pressure (0-1)
    double avg_trade_size;    ///< Average trade size
    double large_trade_ratio; ///< Ratio of large trades
    
    TradeFlowAnalysis()
        : net_flow(0), buy_pressure(0.5), sell_pressure(0.5)
        , avg_trade_size(0), large_trade_ratio(0)
    {}
};

/// Order book analysis result
struct BookAnalysisResult
{
    double imbalance_score;     
    double liquidity_score;     
    double toxicity_score;      
    double spread_estimate;     
    bool   toxic_detected;      
    bool   direction_clear;     
    
    double weighted_imbalance;  
    double pressure_intensity;  
    bool   bid_dominant;        
    bool   ask_dominant;        
    double confidence;          
    uint64_t timestamp;         
    
    BookAnalysisResult()
        : imbalance_score(0), liquidity_score(0.5), toxicity_score(0)
        , spread_estimate(0), toxic_detected(false), direction_clear(false)
        , weighted_imbalance(0), pressure_intensity(0)
        , bid_dominant(false), ask_dominant(false)
        , confidence(0), timestamp(0)
    {}
};

//==============================================================================
// Component 1: OrderBookStateTracker (Static OrderBook State)
//==============================================================================

class OrderBookStateTracker {
public:
    OrderBookStateTracker();
    
    void setContract(const std::string& code, double tickSize, uint32_t depthLevels = 5);
    void onTick(wtp::WTSTickData* tick);
    
    inline const std::string& getCode() const { return _code; }
    inline double getTickSize() const { return _tick_size; }
    inline const OrderBookSnapshot& getSnapshot() const { return _snapshot; }
    inline double estimateLiquidity() const {
        double avg_depth = (_snapshot.bid_depth + _snapshot.ask_depth) / 2.0;
        return std::min(1.0, avg_depth / 100.0);
    }
    
    void reset();
    
private:
    std::string _code;
    double _tick_size;
    uint32_t _depth_levels;
    OrderBookSnapshot _snapshot;
    
    void updateDerivedMetrics();
    double calculateImbalance() const;
    double calculateDepthImbalance() const;
};

//==============================================================================
// Component 2: TradeFlowTracker (Dynamic Transaction State)
//==============================================================================

class TradeFlowTracker {
public:
    TradeFlowTracker();
    
    void setConfig(double tickSize, double largeTradeThreshold);
    void onTickInference(wtp::WTSTickData* tick, double tickSize);
    void onTransaction(wtp::WTSTransData* data);
    
    TradeFlowAnalysis getAnalysis() const;
    void reset();
    
private:
    double _large_trade_threshold;
    
    std::vector<double> _trade_sizes;
    size_t _trade_sizes_idx;
    double _trade_sizes_sum;
    double _net_trade_flow;
    double _large_trade_volume;
    double _total_trade_volume;
    uint32_t _history_size;
    
    TickTransactionInferer _tick_inferer;
};

//==============================================================================
// Facade: MarketDataContext (Unified Market Data Context)
//==============================================================================

class MarketDataContext
{
public:
    MarketDataContext() {}
    ~MarketDataContext() {}
    
    void setContract(const std::string& code, double tickSize, uint32_t depthLevels = 5) {
        _state.setContract(code, tickSize, depthLevels);
    }
    
    void setLargeTradeThreshold(double threshold) { 
        _flow.setConfig(_state.getTickSize(), threshold); 
    }
    
    void onTick(wtp::WTSTickData* tick) {
        if (!tick) return;
        _state.onTick(tick);
        _flow.onTickInference(tick, _state.getTickSize());
    }
    
    void onOrderQueue(wtp::WTSOrdQueData* data) {
        (void)data; // implementation depends on API
    }
    
    void onOrderDetail(wtp::WTSOrdDtlData* data) {
        (void)data; // implementation depends on API
    }
    
    void onTransaction(wtp::WTSTransData* data) {
        _flow.onTransaction(data);
    }
    
    //==========================================================================
    // Inline Forwarding Accessors (Zero-overhead for ISignalSource)
    //==========================================================================
    
    inline const std::string& getCode() const { return _state.getCode(); }
    inline double getTickSize() const { return _state.getTickSize(); }
    inline uint64_t getTimestamp() const { return _state.getSnapshot().timestamp; }
    inline double getMidPrice() const { return _state.getSnapshot().mid_price; }
    inline double getSpread() const { return _state.getSnapshot().spread; }
    inline double getSpreadTicks() const { 
        return _state.getTickSize() > 0 ? _state.getSnapshot().spread / _state.getTickSize() : 0; 
    }
    inline double getBidPrice() const { 
        return _state.getSnapshot().bids.empty() ? 0 : _state.getSnapshot().bids[0].price; 
    }
    inline double getAskPrice() const { 
        return _state.getSnapshot().asks.empty() ? 0 : _state.getSnapshot().asks[0].price; 
    }
    inline double getBidVol() const { 
        return _state.getSnapshot().bids.empty() ? 0 : _state.getSnapshot().bids[0].volume; 
    }
    inline double getAskVol() const { 
        return _state.getSnapshot().asks.empty() ? 0 : _state.getSnapshot().asks[0].volume; 
    }
    inline double getBidDepth() const { return _state.getSnapshot().bid_depth; }
    inline double getAskDepth() const { return _state.getSnapshot().ask_depth; }
    inline double getImbalance() const { return _state.getSnapshot().imbalance; }
    inline double getDepthImbalance() const { return _state.getSnapshot().depth_imbalance; }
    inline const OrderBookSnapshot& getSnapshot() const { return _state.getSnapshot(); }
    inline TradeFlowAnalysis getTradeFlowAnalysis() const { return _flow.getAnalysis(); }
    inline double estimateLiquidity() const { return _state.estimateLiquidity(); }
    
    void reset() {
        _state.reset();
        _flow.reset();
    }
    
private:
    OrderBookStateTracker _state;
    TradeFlowTracker _flow;
};

} // namespace futu
