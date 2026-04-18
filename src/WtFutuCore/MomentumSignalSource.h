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
        return SignalType::CUSTOM; 
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
        }
        
        _last_mid = mid;
        _result.timestamp = ts;
        _result.valid = true;
    }
    
    const SignalResult& result() const override { return _result; }
    
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
        
        // Calculate average of recent half and earlier half
        double recent_sum = 0, earlier_sum = 0;
        size_t half = n / 2;
        
        for (size_t i = half; i < n; ++i)
        {
            recent_sum += _price_history[i];
        }
        double recent_avg = recent_sum / (n - half);
        
        for (size_t i = 0; i < half; ++i)
        {
            earlier_sum += _price_history[i];
        }
        double earlier_avg = earlier_sum / half;
        
        // Calculate momentum
        if (earlier_avg > 0)
        {
            double raw_momentum = (recent_avg - earlier_avg) / earlier_avg;
            
            // Scale and clamp to [-1, 1]
            double momentum = std::tanh(raw_momentum * 100.0);
            
            _result.alpha = momentum;
            
            // Update EMA momentum
            _ema_momentum = _cfg.ema_alpha * momentum + (1 - _cfg.ema_alpha) * _ema_momentum;
        }
        
        _result.confidence = (n >= 20) ? 1.0 : static_cast<double>(n) / 20.0;
    }
};

} // namespace futu
