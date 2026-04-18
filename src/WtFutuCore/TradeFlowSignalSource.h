/*!
 * \file TradeFlowSignalSource.h
 * \brief Trade Flow Signal Source
 * 
 * Analyzes trade flow from order book data:
 *   - Net trade flow (buy - sell)
 *   - Trade imbalance ratio [-1, 1]
 *   - Large trade ratio
 *   - Average trade size
 * 
 * Uses data from MarketDataContext as single source.
 */
#pragma once

#include "ISignalSource.h"
#include "MarketDataContext.h"
#include "../Share/RingBuffer.hpp"
#include <cmath>
#include <algorithm>

namespace futu {

//==============================================================================
// Trade Flow Signal Source
//==============================================================================

class TradeFlowSignalSource : public ISignalSource
{
public:
    struct Config
    {
        uint32_t window;              ///< Trade history window
        double large_trade_threshold; ///< Large trade threshold
        
        Config() 
            : window(100)
            , large_trade_threshold(50.0)
        {}
    };
    
    explicit TradeFlowSignalSource(const Config& cfg = Config())
        : _cfg(cfg)
        , _enabled(true)
        , _net_flow(0)
        , _buy_volume(0)
        , _sell_volume(0)
        , _large_volume(0)
        , _total_volume(0)
        , _trade_count(0)
    {
    }
    
    //==========================================================================
    // ISignalSource Interface
    //==========================================================================
    
    const std::string& name() const override 
    { 
        static std::string n = "TradeFlow"; 
        return n; 
    }
    
    SignalType type() const override 
    { 
        return SignalType::TRADE_FLOW; 
    }
    
    void update(const MarketDataContext& book) override
    {
        // Get trade flow analysis from MarketDataContext
        auto flow = book.getTradeFlowAnalysis();
        
        // Update result
        _result.net_flow = flow.net_flow;
        _result.buy_volume = (_total_volume + _net_flow) / 2.0;
        _result.sell_volume = (_total_volume - _net_flow) / 2.0;
        
        // Normalize net flow
        if (_total_volume > 0)
        {
            _result.net_flow_normalized = _net_flow / _total_volume;
            _result.large_trade_ratio = _large_volume / _total_volume;
        }
        
        _result.avg_trade_size = _trade_count > 0 ? _total_volume / _trade_count : 0;
        
        // Clamp normalized flow
        _result.net_flow_normalized = std::clamp(_result.net_flow_normalized, -1.0, 1.0);
        
        _result.confidence = _trade_count >= 5 ? 1.0 : 0.5;
        _result.valid = true;
        _result.timestamp = book.getTimestamp();
    }
    
    const SignalResult& result() const override { return _result; }
    
    bool enabled() const override { return _enabled; }
    void setEnabled(bool e) override { _enabled = e; }
    
    void reset() override
    {
        _net_flow = 0;
        _buy_volume = 0;
        _sell_volume = 0;
        _large_volume = 0;
        _total_volume = 0;
        _trade_count = 0;
        _result = TradeFlowSignalResult();
    }
    
    /// Set configuration
    void setConfig(const Config& cfg) { _cfg = cfg; }
    
    //==========================================================================
    // Trade Recording (called when trade is detected)
    //==========================================================================
    
    /// Record a trade
    void onTrade(double qty, bool isBuy, uint64_t timestamp)
    {
        double signed_qty = isBuy ? qty : -qty;
        
        _net_flow += signed_qty;
        _total_volume += qty;
        _trade_count++;
        
        if (qty >= _cfg.large_trade_threshold)
        {
            _large_volume += qty;
        }
        
        // Store in history
        TradeSample sample;
        sample.qty = qty;
        sample.is_buy = isBuy;
        sample.timestamp = timestamp;
        _trade_history.push(sample);
        
        // Remove old samples (time-based window ~5 seconds)
        uint64_t cutoff = timestamp - 5000;
        while (_trade_history.size() > 0 && _trade_history.front().timestamp < cutoff)
        {
            const TradeSample& old = _trade_history.front();
            double old_signed = old.is_buy ? old.qty : -old.qty;
            _net_flow -= old_signed;
            _total_volume -= old.qty;
            if (old.qty >= _cfg.large_trade_threshold)
            {
                _large_volume -= old.qty;
            }
            _trade_count--;
            _trade_history.pop();
        }
    }
    
    //==========================================================================
    // Additional Accessors
    //==========================================================================
    
    double getNetFlow() const { return _result.net_flow; }
    double getNormalizedFlow() const { return _result.net_flow_normalized; }
    double getLargeTradeRatio() const { return _result.large_trade_ratio; }
    
private:
    Config _cfg;
    bool _enabled;
    TradeFlowSignalResult _result;
    
    // Trade tracking
    struct TradeSample
    {
        double qty;
        bool is_buy;
        uint64_t timestamp;
    };
    RingBuffer<TradeSample, 256> _trade_history;
    
    double _net_flow;
    double _buy_volume;
    double _sell_volume;
    double _large_volume;
    double _total_volume;
    uint32_t _trade_count;
};

} // namespace futu
