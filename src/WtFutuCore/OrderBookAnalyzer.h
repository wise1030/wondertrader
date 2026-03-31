/*!
 * \file OrderBookAnalyzer.h
 * \brief Order Book Depth Analysis for Market Making
 * 
 * Analyzes order book dynamics for:
 *   - Imbalance detection and direction prediction
 *   - Depth quality assessment
 *   - Liquidity estimation
 *   - Trade flow analysis
 * 
 * All calculations are designed for inline use within on_tick callback.
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
class WTSOrdQueData;
class WTSOrdDtlData;
class WTSTransData;
NS_WTP_END

namespace futu {

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
    
    // Bid side (index 0 = best)
    std::vector<BookLevel> bids;
    
    // Ask side (index 0 = best)
    std::vector<BookLevel> asks;
    
    // Derived metrics
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
    double net_flow;        ///< Net trade flow (buy - sell volume)
    double buy_pressure;    ///< Estimated buy pressure (0-1)
    double sell_pressure;   ///< Estimated sell pressure (0-1)
    double avg_trade_size;  ///< Average trade size
    double large_trade_ratio; ///< Ratio of large trades
    
    TradeFlowAnalysis()
        : net_flow(0), buy_pressure(0.5), sell_pressure(0.5)
        , avg_trade_size(0), large_trade_ratio(0)
    {}
};

/// Order book analysis result
struct BookAnalysisResult
{
    double imbalance_score;     ///< -1 to 1, direction prediction
    double liquidity_score;     ///< 0 to 1, liquidity quality
    double toxicity_score;      ///< 0 to 1, adverse selection risk
    double spread_estimate;     ///< Fair spread estimate
    bool   toxic_detected;      ///< Adverse selection warning
    bool   direction_clear;     ///< Clear directional signal
    
    BookAnalysisResult()
        : imbalance_score(0), liquidity_score(0.5), toxicity_score(0)
        , spread_estimate(0), toxic_detected(false), direction_clear(false)
    {}
};

/// Order Book Analyzer
class OrderBookAnalyzer
{
public:
    OrderBookAnalyzer();
    ~OrderBookAnalyzer() {}
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setContract(const std::string& code, double tickSize, uint32_t depthLevels = 5);
    void setLargeTradeThreshold(double threshold) { _large_trade_threshold = threshold; }
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// Update with tick data (snapshot)
    void onTick(wtp::WTSTickData* tick);
    
    /// Update with order queue (Level 2)
    void onOrderQueue(wtp::WTSOrdQueData* data);
    
    /// Update with order detail (Level 2)
    void onOrderDetail(wtp::WTSOrdDtlData* data);
    
    /// Update with transaction (Level 2)
    void onTransaction(wtp::WTSTransData* data);
    
    //==========================================================================
    // Analysis
    //==========================================================================
    
    /// Get current order book snapshot
    const OrderBookSnapshot& getSnapshot() const { return _snapshot; }
    
    /// Analyze order book for trading signals
    BookAnalysisResult analyze() const;
    
    /// Get trade flow analysis
    TradeFlowAnalysis getTradeFlowAnalysis() const;
    
    //==========================================================================
    // Individual Metrics
    //==========================================================================
    
    /// Calculate simple imbalance (-1 to 1)
    double calculateImbalance() const;
    
    /// Calculate depth-weighted imbalance
    double calculateDepthImbalance() const;
    
    /// Estimate fair spread from book
    double estimateSpread() const;
    
    /// Estimate adverse selection risk
    double estimateToxicity() const;
    
    //==========================================================================
    // History
    //==========================================================================
    
    /// Get imbalance history
    const std::deque<double>& getImbalanceHistory() const { return _imbalance_history; }
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void reset();
    
private:
    std::string _code;
    double _tick_size;
    uint32_t _depth_levels;
    double _large_trade_threshold;
    
    // Current snapshot
    OrderBookSnapshot _snapshot;
    
    // Trade flow tracking
    std::deque<double> _trade_sizes;
    double _net_trade_flow;
    double _large_trade_volume;
    double _total_trade_volume;
    
    // Imbalance history
    std::deque<double> _imbalance_history;
    uint32_t _history_size;
    
    void updateDerivedMetrics();
};

} // namespace futu
