/*!
 * \file OrderBookAnalyzer.cpp
 * \brief Order Book Depth Analysis Implementation
 */
#include "OrderBookAnalyzer.h"
#include "../Includes/WTSDataDef.hpp"
#include <algorithm>
#include <numeric>

namespace futu {

OrderBookAnalyzer::OrderBookAnalyzer()
    : _tick_size(0.2)
    , _depth_levels(5)
    , _large_trade_threshold(10.0)
    , _net_trade_flow(0)
    , _large_trade_volume(0)
    , _total_trade_volume(0)
    , _history_size(100)
{
    _snapshot.bids.reserve(10);
    _snapshot.asks.reserve(10);
}

void OrderBookAnalyzer::setContract(const std::string& code, double tickSize, uint32_t depthLevels)
{
    _code = code;
    _tick_size = tickSize;
    _depth_levels = depthLevels;
    _snapshot.code = code;
}

void OrderBookAnalyzer::onTick(wtp::WTSTickData* tick)
{
    if (!tick) return;
    
    _snapshot.timestamp = tick->actiontime();
    _snapshot.bids.clear();
    _snapshot.asks.clear();
    
    // Extract bid levels
    for (uint32_t i = 0; i < _depth_levels && i < 10; i++)
    {
        BookLevel level;
        level.price = tick->bidprice(i);
        level.volume = tick->bidqty(i);
        if (level.price > 0 && level.volume > 0)
            _snapshot.bids.push_back(level);
    }
    
    // Extract ask levels
    for (uint32_t i = 0; i < _depth_levels && i < 10; i++)
    {
        BookLevel level;
        level.price = tick->askprice(i);
        level.volume = tick->askqty(i);
        if (level.price > 0 && level.volume > 0)
            _snapshot.asks.push_back(level);
    }
    
    updateDerivedMetrics();
}

void OrderBookAnalyzer::onOrderQueue(wtp::WTSOrdQueData* data)
{
    // Would update order counts at each level
    // Implementation depends on specific API
}

void OrderBookAnalyzer::onOrderDetail(wtp::WTSOrdDtlData* data)
{
    // Would update individual order information
    // Implementation depends on specific API
}

void OrderBookAnalyzer::onTransaction(wtp::WTSTransData* data)
{
    if (!data) return;
    
    // Update trade flow - access struct members via getTransStruct()
    const auto& trans = data->getTransStruct();
    double qty = static_cast<double>(trans.volume);
    double price = trans.price;
    
    // Determine trade direction from price vs mid
    double mid = _snapshot.mid_price;
    bool isBuy = (price >= mid);
    
    double signed_qty = isBuy ? qty : -qty;
    _net_trade_flow += signed_qty;
    _total_trade_volume += qty;
    
    // Track large trades
    if (qty > _large_trade_threshold)
    {
        _large_trade_volume += qty;
    }
    
    // Track trade sizes
    _trade_sizes.push_back(qty);
    if (_trade_sizes.size() > _history_size)
        _trade_sizes.pop_front();
}

void OrderBookAnalyzer::updateDerivedMetrics()
{
    // Calculate mid price
    if (!_snapshot.bids.empty() && !_snapshot.asks.empty())
    {
        _snapshot.mid_price = (_snapshot.bids[0].price + _snapshot.asks[0].price) / 2.0;
        _snapshot.spread = _snapshot.asks[0].price - _snapshot.bids[0].price;
    }
    
    // Calculate total depth
    _snapshot.bid_depth = 0;
    for (const auto& level : _snapshot.bids)
        _snapshot.bid_depth += level.volume;
    
    _snapshot.ask_depth = 0;
    for (const auto& level : _snapshot.asks)
        _snapshot.ask_depth += level.volume;
    
    // Calculate imbalance
    _snapshot.imbalance = calculateImbalance();
    _snapshot.depth_imbalance = calculateDepthImbalance();
    
    // Update history
    _imbalance_history.push_back(_snapshot.imbalance);
    if (_imbalance_history.size() > _history_size)
        _imbalance_history.pop_front();
}

double OrderBookAnalyzer::calculateImbalance() const
{
    if (_snapshot.bid_depth + _snapshot.ask_depth == 0)
        return 0;
    
    return (_snapshot.bid_depth - _snapshot.ask_depth) / 
           (_snapshot.bid_depth + _snapshot.ask_depth);
}

double OrderBookAnalyzer::calculateDepthImbalance() const
{
    // Weight by price distance from mid
    double weighted_bid = 0, weighted_ask = 0;
    
    for (const auto& level : _snapshot.bids)
    {
        double distance = (_snapshot.mid_price - level.price) / _tick_size;
        if (distance > 0)
            weighted_bid += level.volume / distance;
    }
    
    for (const auto& level : _snapshot.asks)
    {
        double distance = (level.price - _snapshot.mid_price) / _tick_size;
        if (distance > 0)
            weighted_ask += level.volume / distance;
    }
    
    if (weighted_bid + weighted_ask == 0)
        return 0;
    
    return (weighted_bid - weighted_ask) / (weighted_bid + weighted_ask);
}

double OrderBookAnalyzer::estimateSpread() const
{
    return _snapshot.spread / _tick_size;
}

double OrderBookAnalyzer::estimateToxicity() const
{
    // Toxicity = risk of being picked off
    // High toxicity when:
    // 1. Strong imbalance
    // 2. Large trades against our position
    // 3. Rapid price moves
    
    double imbalance_risk = std::abs(_snapshot.imbalance);
    double large_trade_risk = 0;
    
    if (_total_trade_volume > 0)
        large_trade_risk = _large_trade_volume / _total_trade_volume;
    
    // Combine metrics
    double toxicity = imbalance_risk * 0.6 + large_trade_risk * 0.4;
    
    return std::min(1.0, toxicity);
}

BookAnalysisResult OrderBookAnalyzer::analyze() const
{
    BookAnalysisResult result;
    
    // Imbalance score
    result.imbalance_score = _snapshot.depth_imbalance;
    
    // Liquidity score (based on depth)
    double avg_depth = (_snapshot.bid_depth + _snapshot.ask_depth) / 2.0;
    result.liquidity_score = std::min(1.0, avg_depth / 100.0);
    
    // Toxicity
    result.toxicity_score = estimateToxicity();
    result.toxic_detected = (result.toxicity_score > 0.7);
    
    // Spread estimate
    result.spread_estimate = estimateSpread();
    
    // Direction clarity
    result.direction_clear = (std::abs(_snapshot.imbalance) > 0.3);
    
    return result;
}

TradeFlowAnalysis OrderBookAnalyzer::getTradeFlowAnalysis() const
{
    TradeFlowAnalysis flow;
    
    flow.net_flow = _net_trade_flow;
    
    if (_total_trade_volume > 0)
    {
        flow.buy_pressure = (_total_trade_volume + _net_trade_flow) / (2 * _total_trade_volume);
        flow.sell_pressure = 1.0 - flow.buy_pressure;
        flow.large_trade_ratio = _large_trade_volume / _total_trade_volume;
    }
    
    if (!_trade_sizes.empty())
    {
        double sum = std::accumulate(_trade_sizes.begin(), _trade_sizes.end(), 0.0);
        flow.avg_trade_size = sum / _trade_sizes.size();
    }
    
    return flow;
}

void OrderBookAnalyzer::reset()
{
    _snapshot = OrderBookSnapshot();
    _trade_sizes.clear();
    _imbalance_history.clear();
    _net_trade_flow = 0;
    _large_trade_volume = 0;
    _total_trade_volume = 0;
}

} // namespace futu
