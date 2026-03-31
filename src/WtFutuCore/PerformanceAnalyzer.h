/*!
 * \file PerformanceAnalyzer.h
 * \brief Market Making Performance Analysis
 * 
 * Comprehensive PnL attribution and performance metrics:
 *   - Spread capture analysis
 *   - Inventory PnL decomposition
 *   - Adverse selection measurement
 *   - Fill rate analysis by market condition
 *   - Risk-adjusted returns
 */
#pragma once

#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <cstdint>
#include "../Share/RingBuffer.hpp"
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"

namespace futu {

//==============================================================================
// Trade Record
//==============================================================================

/// Individual trade record for analysis
struct TradeRecord
{
    std::string code;
    uint32_t    order_id;
    double      price;
    double      qty;
    double      mid_at_trade;       ///< Mid price when trade occurred
    double      spread_at_trade;    ///< Spread when trade occurred
    bool        is_buy;             ///< True = bought, False = sold
    bool        is_crossing;        ///< Did we cross the spread?
    uint64_t    timestamp;
    double      alpha_at_trade;     ///< Alpha signal at trade time
    double      volatility;         ///< Volatility at trade time
    
    TradeRecord()
        : order_id(0), price(0), qty(0)
        , mid_at_trade(0), spread_at_trade(0)
        , is_buy(false), is_crossing(false)
        , timestamp(0), alpha_at_trade(0), volatility(0)
    {}
    
    /// Calculate immediate PnL impact
    double immediatePnL() const
    {
        if (is_buy)
            return (mid_at_trade - price) * qty;
        else
            return (price - mid_at_trade) * qty;
    }
    
    /// Calculate spread captured
    double spreadCaptured() const
    {
        if (spread_at_trade <= 0) return 0;
        
        if (is_buy)
            return (mid_at_trade - price) / spread_at_trade;
        else
            return (price - mid_at_trade) / spread_at_trade;
    }
};

//==============================================================================
// PnL Attribution
//==============================================================================

/// PnL attribution breakdown
struct PnLAttribution
{
    double spread_pnl;          ///< PnL from spread capture
    double inventory_pnl;       ///< PnL from inventory position
    double adverse_selection;   ///< Loss from adverse selection
    double alpha_pnl;           ///< PnL attributed to alpha signals
    double timing_pnl;          ///< PnL from execution timing
    
    PnLAttribution()
        : spread_pnl(0), inventory_pnl(0)
        , adverse_selection(0), alpha_pnl(0), timing_pnl(0)
    {}
    
    double total() const
    {
        return spread_pnl + inventory_pnl + alpha_pnl + timing_pnl - adverse_selection;
    }
};

//==============================================================================
// Performance Metrics
//==============================================================================

/// Comprehensive performance metrics
struct PerformanceMetrics
{
    // Basic metrics
    double total_pnl;           ///< Total realized PnL
    double unrealized_pnl;      ///< Current unrealized PnL
    double total_volume;        ///< Total traded volume
    uint32_t total_trades;      ///< Number of trades
    
    // Spread metrics
    double avg_spread_captured; ///< Average spread captured per trade
    double spread_capture_rate; ///< Ratio of captured spread to market spread
    
    // Fill metrics
    double fill_rate;           ///< Fills / Quotes
    double cross_rate;          ///< Ratio of crossing trades
    double avg_fill_latency_us; ///< Average fill latency (microseconds)
    
    // Risk metrics
    double max_drawdown;        ///< Maximum drawdown
    double sharpe_ratio;        ///< Sharpe ratio (annualized)
    double sortino_ratio;       ///< Sortino ratio
    double win_rate;            ///< Profitable trades / Total trades
    
    // Adverse selection
    double adverse_ratio;       ///< Adverse selection / Total PnL
    double toxicity_events;     ///< Number of toxicity triggers
    
    // Alpha performance
    double alpha_accuracy;      ///< Alpha signal accuracy
    double alpha_pnl_per_trade; ///< PnL per trade with strong alpha
    
    // Inventory
    double avg_inventory;       ///< Average inventory held
    double inventory_turnover;  ///< Inventory turnover rate
    double max_inventory_held;  ///< Maximum inventory held
    
    // Time
    uint64_t trading_time_sec;  ///< Total trading time
    uint32_t trading_days;      ///< Number of trading days
    
    PerformanceMetrics()
        : total_pnl(0), unrealized_pnl(0), total_volume(0), total_trades(0)
        , avg_spread_captured(0), spread_capture_rate(0)
        , fill_rate(0), cross_rate(0), avg_fill_latency_us(0)
        , max_drawdown(0), sharpe_ratio(0), sortino_ratio(0), win_rate(0)
        , adverse_ratio(0), toxicity_events(0)
        , alpha_accuracy(0), alpha_pnl_per_trade(0)
        , avg_inventory(0), inventory_turnover(0), max_inventory_held(0)
        , trading_time_sec(0), trading_days(0)
    {}
    
    /// Calculate PnL per million traded
    double pnlPerMillion() const
    {
        if (total_volume <= 0) return 0;
        return total_pnl / total_volume * 1e6;
    }
    
    /// Calculate PnL per day
    double pnlPerDay() const
    {
        if (trading_days == 0) return 0;
        return total_pnl / trading_days;
    }
};

//==============================================================================
// Market Condition Buckets
//==============================================================================

/// Market condition types
enum class MarketCondition : uint8_t
{
    TRENDING_UP,        ///< Strong upward trend
    TRENDING_DOWN,      ///< Strong downward trend
    RANGING,            ///< Sideways/ranging market
    HIGH_VOLATILITY,    ///< High volatility period
    LOW_VOLATILITY,     ///< Low volatility period
    AUCTION,            ///< Auction period
    NEWS_EVENT,         ///< News/event driven
    NORMAL              ///< Normal conditions
};

/// Performance by market condition
struct ConditionPerformance
{
    MarketCondition condition;
    double pnl;
    uint32_t trade_count;
    double volume;
    double win_rate;
    double avg_spread_captured;
    
    ConditionPerformance()
        : condition(MarketCondition::NORMAL)
        , pnl(0), trade_count(0), volume(0)
        , win_rate(0), avg_spread_captured(0)
    {}
};

//==============================================================================
// Performance Analyzer
//==============================================================================

/// Performance Analyzer Configuration
struct AnalyzerConfig
{
    uint32_t history_size;      ///< Trade history size
    double adverse_threshold;   ///< Threshold for adverse selection detection
    double strong_alpha_threshold; ///< Threshold for strong alpha signal
    
    AnalyzerConfig()
        : history_size(10000)
        , adverse_threshold(0.5)
        , strong_alpha_threshold(0.7)
    {}
};

/// Market Making Performance Analyzer
class PerformanceAnalyzer
{
public:
    PerformanceAnalyzer();
    ~PerformanceAnalyzer() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const AnalyzerConfig& config) { _config = config; }
    const AnalyzerConfig& getConfig() const { return _config; }
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// Record a trade
    void recordTrade(const TradeRecord& trade);
    
    /// Record a quote (for fill rate calculation)
    void recordQuote(const std::string& code, double bidPrice, double askPrice,
                     double bidQty, double askQty, uint64_t timestamp);
    
    /// Update current position
    void updatePosition(const std::string& code, double position, double avgCost);
    
    /// Record toxicity event
    void recordToxicityEvent();
    
    /// Record alpha signal outcome
    void recordAlphaSignal(double alpha, bool wasCorrect);
    
    //==========================================================================
    // Analysis
    //==========================================================================
    
    /// Get overall performance metrics
    PerformanceMetrics getMetrics() const;
    
    /// Get PnL attribution
    PnLAttribution getPnLAttribution() const;
    
    /// Get performance by market condition
    std::map<MarketCondition, ConditionPerformance> getPerformanceByCondition() const;
    
    /// Get recent trades
    const RingBuffer<TradeRecord, 16384>& getRecentTrades() const { return _trades; }
    
    //==========================================================================
    // Analysis Helpers
    //==========================================================================
    
    /// Calculate adverse selection cost for a trade
    double calculateAdverseSelection(const TradeRecord& trade) const;
    
    /// Determine market condition at a timestamp
    MarketCondition determineMarketCondition(uint64_t timestamp) const;
    
    //==========================================================================
    // Reporting
    //==========================================================================
    
    /// Generate summary report
    std::string generateSummaryReport() const;
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void reset();
    void resetDaily();
    
private:
    AnalyzerConfig _config;
    
    // Trade history
    RingBuffer<TradeRecord, 16384> _trades;
    
    // Quote count for fill rate
    uint32_t _quote_count;
    
    // Position tracking
    struct PositionState {
        double position;
        double avg_cost;
        double realized_pnl;
    };
    wtp::wt_hashmap<std::string, PositionState> _positions;
    
    // Running totals
    double _total_pnl;
    double _total_spread_captured;
    double _total_adverse_selection;
    double _total_volume;
    uint32_t _total_trades;
    uint32_t _winning_trades;
    
    // Drawdown tracking
    double _peak_pnl;
    double _max_drawdown;
    
    // Toxicity events
    uint32_t _toxicity_events;
    
    // Alpha tracking
    uint32_t _alpha_signals;
    uint32_t _alpha_correct;
    double _alpha_pnl;
    
    // PnL history for Sharpe calculation
    RingBuffer<double, 1024> _pnl_history;
    
    // Market condition performance
    std::map<MarketCondition, ConditionPerformance> _condition_perf;
    
    // Time tracking
    uint64_t _start_time;
    uint64_t _last_trade_time;
    uint32_t _trading_days;
};

//==============================================================================
// Utility Functions
//==============================================================================

/// Convert market condition to string
inline const char* marketConditionName(MarketCondition cond)
{
    switch (cond)
    {
        case MarketCondition::TRENDING_UP: return "TRENDING_UP";
        case MarketCondition::TRENDING_DOWN: return "TRENDING_DOWN";
        case MarketCondition::RANGING: return "RANGING";
        case MarketCondition::HIGH_VOLATILITY: return "HIGH_VOLATILITY";
        case MarketCondition::LOW_VOLATILITY: return "LOW_VOLATILITY";
        case MarketCondition::AUCTION: return "AUCTION";
        case MarketCondition::NEWS_EVENT: return "NEWS_EVENT";
        case MarketCondition::NORMAL: return "NORMAL";
        default: return "UNKNOWN";
    }
}

} // namespace futu
