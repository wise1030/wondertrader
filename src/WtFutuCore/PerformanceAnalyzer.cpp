/*!
 * \file PerformanceAnalyzer.cpp
 * \brief Market Making Performance Analysis Implementation
 */
#include "PerformanceAnalyzer.h"
#include "../Share/TimeUtils.hpp"
#include "../WTSTools/WTSLogger.h"
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace futu {

PerformanceAnalyzer::PerformanceAnalyzer()
    : _quote_count(0)
    , _total_pnl(0)
    , _total_spread_captured(0)
    , _total_adverse_selection(0)
    , _total_volume(0)
    , _total_trades(0)
    , _winning_trades(0)
    , _peak_pnl(0)
    , _max_drawdown(0)
    , _toxicity_events(0)
    , _alpha_signals(0)
    , _alpha_correct(0)
    , _alpha_pnl(0)
    , _start_time(0)
    , _last_trade_time(0)
    , _trading_days(0)
{
    // Initialize condition performance
    for (int i = 0; i <= static_cast<int>(MarketCondition::NORMAL); ++i)
    {
        ConditionPerformance cp;
        cp.condition = static_cast<MarketCondition>(i);
        _condition_perf[cp.condition] = cp;
    }
}

void PerformanceAnalyzer::recordTrade(const TradeRecord& trade)
{
    _trades.push(trade);
    
    // Update totals
    _total_volume += trade.qty;
    _total_trades++;
    _last_trade_time = trade.timestamp;
    
    // Calculate PnL impact
    double immediate_pnl = trade.immediatePnL();
    _total_pnl += immediate_pnl;
    
    // Track spread captured
    double spread_cap = trade.spreadCaptured();
    _total_spread_captured += spread_cap;
    
    // Track adverse selection
    double adverse = calculateAdverseSelection(trade);
    _total_adverse_selection += adverse;
    
    // Track winning trades
    if (immediate_pnl > 0)
        _winning_trades++;
    
    // Track alpha performance
    if (std::abs(trade.alpha_at_trade) >= _config.strong_alpha_threshold)
    {
        _alpha_signals++;
        // Alpha is correct if trade direction matches alpha direction
        bool alpha_correct = (trade.alpha_at_trade > 0 && trade.is_buy) ||
                            (trade.alpha_at_trade < 0 && !trade.is_buy);
        if (alpha_correct)
        {
            _alpha_correct++;
            _alpha_pnl += immediate_pnl;
        }
    }
    
    // Update PnL history
    _pnl_history.push(immediate_pnl);
    
    // Update drawdown
    if (_total_pnl > _peak_pnl)
        _peak_pnl = _total_pnl;
    
    double drawdown = _peak_pnl - _total_pnl;
    if (drawdown > _max_drawdown)
        _max_drawdown = drawdown;
    
    // Update condition performance
    MarketCondition cond = determineMarketCondition(trade.timestamp);
    ConditionPerformance& cp = _condition_perf[cond];
    cp.pnl += immediate_pnl;
    cp.trade_count++;
    cp.volume += trade.qty;
    if (immediate_pnl > 0)
        cp.win_rate = (cp.win_rate * (cp.trade_count - 1) + 1.0) / cp.trade_count;
    else
        cp.win_rate = cp.win_rate * (cp.trade_count - 1) / cp.trade_count;
    cp.avg_spread_captured = (cp.avg_spread_captured * (cp.trade_count - 1) + spread_cap) 
                              / cp.trade_count;
}

void PerformanceAnalyzer::recordQuote(const std::string& code, 
                                       double bidPrice, double askPrice,
                                       double bidQty, double askQty, 
                                       uint64_t timestamp)
{
    _quote_count++;
}

void PerformanceAnalyzer::updatePosition(const std::string& code, 
                                          double position, double avgCost)
{
    PositionState& ps = _positions[code];
    ps.position = position;
    ps.avg_cost = avgCost;
}

void PerformanceAnalyzer::recordToxicityEvent()
{
    _toxicity_events++;
}

void PerformanceAnalyzer::recordAlphaSignal(double alpha, bool wasCorrect)
{
    _alpha_signals++;
    if (wasCorrect)
        _alpha_correct++;
}

PerformanceMetrics PerformanceAnalyzer::getMetrics() const
{
    PerformanceMetrics metrics;
    
    // Basic metrics
    metrics.total_pnl = _total_pnl;
    metrics.total_volume = _total_volume;
    metrics.total_trades = _total_trades;
    
    // Spread metrics
    if (_total_trades > 0)
    {
        metrics.avg_spread_captured = _total_spread_captured / _total_trades;
    }
    metrics.spread_capture_rate = metrics.avg_spread_captured;
    
    // Fill metrics
    if (_quote_count > 0)
    {
        metrics.fill_rate = static_cast<double>(_total_trades) / _quote_count;
    }
    
    // Win rate
    if (_total_trades > 0)
    {
        metrics.win_rate = static_cast<double>(_winning_trades) / _total_trades;
    }
    
    // Drawdown
    metrics.max_drawdown = _max_drawdown;
    
    // Sharpe ratio (simplified)
    if (_pnl_history.size() >= 10)
    {
        double mean = 0, variance = 0;
        for (size_t i = 0; i < _pnl_history.size(); ++i)
        {
            mean += _pnl_history[i];
        }
        mean /= _pnl_history.size();
        
        for (size_t i = 0; i < _pnl_history.size(); ++i)
        {
            double diff = _pnl_history[i] - mean;
            variance += diff * diff;
        }
        variance /= _pnl_history.size();
        
        double std_dev = std::sqrt(variance);
        if (std_dev > 0)
        {
            // Annualize assuming 250 trading days
            metrics.sharpe_ratio = mean / std_dev * std::sqrt(250);
        }
    }
    
    // Adverse selection
    if (_total_pnl != 0)
    {
        metrics.adverse_ratio = _total_adverse_selection / std::abs(_total_pnl);
    }
    metrics.toxicity_events = _toxicity_events;
    
    // Alpha performance
    if (_alpha_signals > 0)
    {
        metrics.alpha_accuracy = static_cast<double>(_alpha_correct) / _alpha_signals;
        metrics.alpha_pnl_per_trade = _alpha_pnl / _alpha_signals;
    }
    
    // Time metrics
    if (_start_time > 0 && _last_trade_time > _start_time)
    {
        metrics.trading_time_sec = (_last_trade_time - _start_time) / 1000;
    }
    metrics.trading_days = _trading_days;
    
    return metrics;
}

PnLAttribution PerformanceAnalyzer::getPnLAttribution() const
{
    PnLAttribution attr;
    
    // Spread PnL: positive spread capture
    attr.spread_pnl = _total_spread_captured > 0 ? _total_spread_captured : 0;
    
    // Adverse selection: negative spread capture + crossing costs
    attr.adverse_selection = _total_adverse_selection;
    
    // Alpha PnL: PnL from trades with strong alpha signals
    attr.alpha_pnl = _alpha_pnl;
    
    // Inventory PnL: estimated from position changes
    double inv_pnl = 0;
    for (const auto& kv : _positions)
    {
        // Simplified: estimate inventory PnL
        inv_pnl += kv.second.realized_pnl;
    }
    attr.inventory_pnl = inv_pnl;
    
    // Timing PnL: residual
    attr.timing_pnl = _total_pnl - attr.spread_pnl - attr.inventory_pnl 
                      - attr.alpha_pnl + attr.adverse_selection;
    
    return attr;
}

std::map<MarketCondition, ConditionPerformance> 
PerformanceAnalyzer::getPerformanceByCondition() const
{
    return _condition_perf;
}

double PerformanceAnalyzer::calculateAdverseSelection(const TradeRecord& trade) const
{
    // Adverse selection = price moved against us after trade
    // Estimate using spread and crossing status
    
    double adverse = 0;
    
    // If we crossed the spread, we paid half spread
    if (trade.is_crossing)
    {
        adverse = trade.spread_at_trade / 2 * trade.qty;
    }
    
    // Additional adverse selection based on spread captured
    double spread_cap = trade.spreadCaptured();
    if (spread_cap < 0)
    {
        // Negative spread capture indicates adverse selection
        adverse += std::abs(spread_cap) * trade.spread_at_trade * trade.qty;
    }
    
    return adverse;
}

MarketCondition PerformanceAnalyzer::determineMarketCondition(uint64_t timestamp) const
{
    // Simplified market condition determination
    // In production, this would use volatility, trend metrics, etc.
    
    // Default to NORMAL
    return MarketCondition::NORMAL;
}

std::string PerformanceAnalyzer::generateSummaryReport() const
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    
    auto metrics = getMetrics();
    auto attr = getPnLAttribution();
    
    oss << "\n========== Market Making Performance Report ==========\n\n";
    
    oss << "--- PnL Summary ---\n";
    oss << "  Total PnL:         " << metrics.total_pnl << "\n";
    oss << "  PnL per Million:   " << metrics.pnlPerMillion() << "\n";
    oss << "  Max Drawdown:      " << metrics.max_drawdown << "\n";
    oss << "  Sharpe Ratio:      " << metrics.sharpe_ratio << "\n";
    oss << "  Win Rate:          " << (metrics.win_rate * 100) << "%\n\n";
    
    oss << "--- PnL Attribution ---\n";
    oss << "  Spread Capture:    " << attr.spread_pnl << "\n";
    oss << "  Alpha PnL:         " << attr.alpha_pnl << "\n";
    oss << "  Inventory PnL:     " << attr.inventory_pnl << "\n";
    oss << "  Adverse Selection: " << attr.adverse_selection << "\n";
    oss << "  Timing PnL:        " << attr.timing_pnl << "\n\n";
    
    oss << "--- Trading Metrics ---\n";
    oss << "  Total Trades:      " << metrics.total_trades << "\n";
    oss << "  Total Volume:      " << metrics.total_volume << "\n";
    oss << "  Fill Rate:         " << (metrics.fill_rate * 100) << "%\n";
    oss << "  Avg Spread Cap:    " << metrics.avg_spread_captured << "\n\n";
    
    oss << "--- Alpha Performance ---\n";
    oss << "  Alpha Accuracy:    " << (metrics.alpha_accuracy * 100) << "%\n";
    oss << "  Alpha PnL/Trade:   " << metrics.alpha_pnl_per_trade << "\n\n";
    
    oss << "--- Risk Metrics ---\n";
    oss << "  Adverse Ratio:     " << (metrics.adverse_ratio * 100) << "%\n";
    oss << "  Toxicity Events:   " << metrics.toxicity_events << "\n";
    
    oss << "\n======================================================\n";
    
    return oss.str();
}

void PerformanceAnalyzer::reset()
{
    _trades.clear();
    _quote_count = 0;
    _positions.clear();
    _total_pnl = 0;
    _total_spread_captured = 0;
    _total_adverse_selection = 0;
    _total_volume = 0;
    _total_trades = 0;
    _winning_trades = 0;
    _peak_pnl = 0;
    _max_drawdown = 0;
    _toxicity_events = 0;
    _alpha_signals = 0;
    _alpha_correct = 0;
    _alpha_pnl = 0;
    _pnl_history.clear();
    _start_time = 0;
    _last_trade_time = 0;
    _trading_days = 0;
    
    for (auto& kv : _condition_perf)
    {
        kv.second = ConditionPerformance();
        kv.second.condition = kv.first;
    }
}

void PerformanceAnalyzer::resetDaily()
{
    // Keep trade history but reset daily counters
    _trading_days++;
    
    // Reset daily PnL tracking
    _peak_pnl = _total_pnl;  // Reset peak to current PnL
    _max_drawdown = 0;
}

} // namespace futu
