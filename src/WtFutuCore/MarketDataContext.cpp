/*!
 * \file MarketDataContext.cpp
 * \brief Order Book Depth Analysis Implementation
 */
#include "MarketDataContext.h"
#include "../Includes/WTSDataDef.hpp"
#include <algorithm>
#include <numeric>

namespace futu {

//==============================================================================
// OrderBookStateTracker Implementation
//==============================================================================

OrderBookStateTracker::OrderBookStateTracker()
    : _tick_size(0.2)
    , _depth_levels(5)
{
    _snapshot.bids.reserve(10);
    _snapshot.asks.reserve(10);
}

void OrderBookStateTracker::setContract(const std::string& code, double tickSize, uint32_t depthLevels)
{
    _code = code;
    _tick_size = tickSize;
    _depth_levels = depthLevels;
    _snapshot.code = code;
}

void OrderBookStateTracker::onTick(wtp::WTSTickData* tick)
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

void OrderBookStateTracker::updateDerivedMetrics()
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
}

double OrderBookStateTracker::calculateImbalance() const
{
    if (_snapshot.bid_depth + _snapshot.ask_depth == 0)
        return 0;
    
    return (_snapshot.bid_depth - _snapshot.ask_depth) / 
           (_snapshot.bid_depth + _snapshot.ask_depth);
}

double OrderBookStateTracker::calculateDepthImbalance() const
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

void OrderBookStateTracker::reset()
{
    _snapshot = OrderBookSnapshot();
}

//==============================================================================
// TradeFlowTracker Implementation
//==============================================================================

TradeFlowTracker::TradeFlowTracker()
    : _large_trade_threshold(10.0)
    , _trade_sizes_idx(0)
    , _trade_sizes_sum(0.0)
    , _net_trade_flow(0)
    , _large_trade_volume(0)
    , _total_trade_volume(0)
    , _history_size(100)
{
    _trade_sizes.reserve(_history_size);
}

void TradeFlowTracker::setConfig(double tickSize, double largeTradeThreshold)
{
    _large_trade_threshold = largeTradeThreshold;
    
    TickInfererConfig infer_cfg;
    infer_cfg.tick_size = tickSize;
    infer_cfg.large_trade_threshold = largeTradeThreshold;
    infer_cfg.min_confidence = 0.3;
    _tick_inferer.setConfig(infer_cfg);
}

void TradeFlowTracker::onTickInference(wtp::WTSTickData* tick, double tickSize)
{
    auto& tickStruct = tick->getTickStruct();
    double bid1 = tickStruct.bid_prices[0];
    double ask1 = tickStruct.ask_prices[0];
    double last_price = tickStruct.price;
    double total_vol = tickStruct.total_volume;
    
    // 推断成交方向
    InferredTransaction inferred = _tick_inferer.inferFromTick(
        bid1, ask1,
        tickStruct.bid_qty[0], tickStruct.ask_qty[0],
        last_price, total_vol,
        tickStruct.action_time
    );
    
    // 默认推断置信度阈值为 0.3
    if (inferred.confidence >= 0.3 && inferred.volume > 0)
    {
        double signed_flow = inferred.is_buy_initiated ? inferred.volume : -inferred.volume;
        _net_trade_flow += signed_flow;
        _total_trade_volume += inferred.volume;
        
        // 跟踪大单
        if (inferred.volume > _large_trade_threshold)
        {
            _large_trade_volume += inferred.volume;
        }
        
        // 记录成交大小
        if (_trade_sizes.size() < _history_size) {
            _trade_sizes.push_back(inferred.volume);
            _trade_sizes_sum += inferred.volume;
        } else {
            _trade_sizes_sum = _trade_sizes_sum - _trade_sizes[_trade_sizes_idx] + inferred.volume;
            _trade_sizes[_trade_sizes_idx] = inferred.volume;
            _trade_sizes_idx = (_trade_sizes_idx + 1) % _history_size;
        }
    }
}

void TradeFlowTracker::onTransaction(wtp::WTSTransData* data)
{
    if (!data) return;
    
    // Update trade flow - access struct members via getTransStruct()
    const auto& trans = data->getTransStruct();
    double qty = static_cast<double>(trans.volume);
    
    // Determine trade direction from side field
    bool isBuy = (trans.side == BDT_Buy);
    
    double signed_qty = isBuy ? qty : -qty;
    _net_trade_flow += signed_qty;
    _total_trade_volume += qty;
    
    // Track large trades
    if (qty > _large_trade_threshold)
    {
        _large_trade_volume += qty;
    }
    
    // Track trade sizes with vector ring buffer
    if (_trade_sizes.size() < _history_size) {
        _trade_sizes.push_back(qty);
        _trade_sizes_sum += qty;
    } else {
        _trade_sizes_sum = _trade_sizes_sum - _trade_sizes[_trade_sizes_idx] + qty;
        _trade_sizes[_trade_sizes_idx] = qty;
        _trade_sizes_idx = (_trade_sizes_idx + 1) % _history_size;
    }
}

TradeFlowAnalysis TradeFlowTracker::getAnalysis() const
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
        flow.avg_trade_size = _trade_sizes_sum / _trade_sizes.size();
    }
    
    return flow;
}

void TradeFlowTracker::reset()
{
    _trade_sizes.clear();
    _trade_sizes_idx = 0;
    _trade_sizes_sum = 0.0;
    _net_trade_flow = 0;
    _large_trade_volume = 0;
    _total_trade_volume = 0;
    
    _tick_inferer.reset();
}

} // namespace futu
