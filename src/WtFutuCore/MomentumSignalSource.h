/*!
 * \file MomentumSignalSource.h
 * \brief Momentum Signal Source
 * 
 * Calculates price momentum from recent price history:
 *   - Simple momentum: (recent_avg - earlier_avg) / earlier_avg
 *   - EMA momentum: EMA-based trend detection
 * 
 * Momentum is useful for:
 *   - Trend detection
 *   - Market state classification
 *   - Alpha signal enhancement
 */
#pragma once

#include "ISignalSource.h"
#include "MarketDataContext.h"
#include "../Share/RingBuffer.hpp"
#include <cmath>
#include <algorithm>

namespace futu {

//==============================================================================
// Momentum Signal Source
//==============================================================================

class MomentumSignalSource : public ISignalSource
{
public:
    struct Config
    {
        uint32_t window;        ///< Price history window
        double ema_alpha;       ///< EMA smoothing factor
        
        Config() 
            : window(50)
            , ema_alpha(0.1)
        {}
    };
    
    explicit MomentumSignalSource(const Config& cfg = Config())
        : _cfg(cfg)
        , _enabled(true)
        , _last_mid(0)
        , _ema_momentum(0)
    {
    }
    
    //==========================================================================
    // ISignalSource Interface
    //==========================================================================
    
    const std::string& name() const override 
    { 
        static std::string n = "Momentum"; 
        return n; 
    }
    
    SignalType type() const override 
    { 
        return SignalType::MOMENTUM; 
    }
    
    void update(const MarketDataContext& book) override
    {
        double mid = book.getMidPrice();
        uint64_t ts = book.getTimestamp();
        
        if (mid <= 0)
        {
            _result.valid = false;
            return;
        }
        
        // Store price history
        _price_history.push(mid);
        
        // Calculate momentum if enough history
        if (_price_history.size() >= 10)
        {
            calculateMomentum();
            _result.timestamp = ts;
            _result.valid = true;   // FIX P0-5: valid=true移到if内部，样本不足时不标记valid
        }
        else
        {
            _result.valid = false;  // 样本不足，不纳入加权计算
        }
        
        _last_mid = mid;
    }
    
    const SignalResult& result() const override { return _result; }
    
    double getAlphaValue() const override { return _result.alpha; }
    
    bool enabled() const override { return _enabled; }
    void setEnabled(bool e) override { _enabled = e; }
    
    void reset() override
    {
        _price_history.clear();
        _last_mid = 0;
        _ema_momentum = 0;
        _result = AlphaSignalResult();
    }
    
    /// Set configuration
    void setConfig(const Config& cfg) { _cfg = cfg; }
    
    //==========================================================================
    // Additional Accessors
    //==========================================================================
    
    /// Get momentum value [-1, 1]
    double getMomentum() const { return _result.alpha; }
    
    /// Get EMA momentum
    double getEMAMomentum() const { return _ema_momentum; }
    
private:
    Config _cfg;
    bool _enabled;
    AlphaSignalResult _result;
    
    RingBuffer<double, 128> _price_history;
    double _last_mid;
    double _ema_momentum;
    
    void calculateMomentum()
    {
        size_t n = _price_history.size();
        
        // FIX P1-2: 改用对数收益率替代百分比变化率
        // 百分比变化率 (P_t - P_{t-1})/P_{t-1} * 100 对高价合约信号被压制：
        //   同样1个tick变动，价格3000的合约变化率0.033%，价格30000的合约仅0.0033%。
        // 对数收益率 log(P_t/P_{t-1}) 消除品种价格差异，数学性质更优：
        //   - 可加性: log(P3/P1) = log(P3/P2) + log(P2/P1)
        //   - 对称性: 涨跌相同幅度，对数收益率绝对值相同
        // 乘以1000作为缩放因子(对数收益率通常很小，如0.0001级别)
        
        // Calculate log returns
        double log_return_sum = 0;
        size_t return_count = 0;
        
        for (size_t i = 1; i < n; ++i)
        {
            if (_price_history[i - 1] > 0 && _price_history[i] > 0)
            {
                log_return_sum += std::log(_price_history[i] / _price_history[i - 1]);
                return_count++;
            }
        }
        
        if (return_count > 0)
        {
            double raw_momentum = log_return_sum / return_count * 1000.0;
            
            // Scale and clamp to [-1, 1]
            double momentum = std::tanh(raw_momentum);
            
            _result.alpha = momentum;
            
            // Update EMA momentum
            _ema_momentum = _cfg.ema_alpha * momentum + (1 - _cfg.ema_alpha) * _ema_momentum;
        }
        
        _result.confidence = (n >= 20) ? 1.0 : static_cast<double>(n) / 20.0;
    }
};

} // namespace futu
