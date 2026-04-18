/*!
 * \file BookImbalanceSignalSource.h
 * \brief Book Imbalance Signal Source
 * 
 * Analyzes order book imbalance for directional signals:
 *   - Simple imbalance: (bid_vol - ask_vol) / (bid_vol + ask_vol)
 *   - Depth imbalance: Price-weighted imbalance
 *   - Pressure intensity: Signal strength
 * 
 * Uses data from MarketDataContext as single source.
 */
#pragma once

#include "ISignalSource.h"
#include "MarketDataContext.h"
#include <cmath>
#include <algorithm>

namespace futu {

//==============================================================================
// Book Imbalance Signal Source
//==============================================================================

class BookImbalanceSignalSource : public ISignalSource
{
public:
    struct Config
    {
        double dominant_threshold;    ///< Threshold for dominant flag (default 0.2)
        
        Config() 
            : dominant_threshold(0.2)
        {}
    };
    
    explicit BookImbalanceSignalSource(const Config& cfg = Config())
        : _cfg(cfg)
        , _enabled(true)
    {
    }
    
    //==========================================================================
    // ISignalSource Interface
    //==========================================================================
    
    const std::string& name() const override 
    { 
        static std::string n = "BookImbalance"; 
        return n; 
    }
    
    SignalType type() const override 
    { 
        return SignalType::BOOK_IMBALANCE; 
    }
    
    void update(const MarketDataContext& book) override
    {
        // Extract imbalance data from MarketDataContext
        _result.simple_imbalance = book.getImbalance();
        _result.depth_imbalance = book.getDepthImbalance();
        _result.bid_depth = book.getBidDepth();
        _result.ask_depth = book.getAskDepth();
        
        // Calculate pressure intensity
        _result.pressure_intensity = std::abs(_result.simple_imbalance);
        
        // Determine dominant side
        _result.bid_dominant = _result.simple_imbalance > _cfg.dominant_threshold;
        _result.ask_dominant = _result.simple_imbalance < -_cfg.dominant_threshold;
        
        // Set confidence based on intensity
        _result.confidence = _result.pressure_intensity;
        
        // Set timestamp
        _result.timestamp = book.getTimestamp();
        
        // Mark as valid
        _result.valid = true;
    }
    
    const SignalResult& result() const override { return _result; }
    
    bool enabled() const override { return _enabled; }
    void setEnabled(bool e) override { _enabled = e; }
    
    void reset() override
    {
        _result = BookImbalanceSignalResult();
    }
    
    //==========================================================================
    // Convenience Methods
    //==========================================================================
    
    /// Get simple imbalance [-1, 1]
    double getSimpleImbalance() const { return _result.simple_imbalance; }
    
    /// Get depth imbalance [-1, 1]
    double getDepthImbalance() const { return _result.depth_imbalance; }
    
    /// Get pressure intensity [0, 1]
    double getPressureIntensity() const { return _result.pressure_intensity; }
    
    /// Check if bid side is dominant
    bool isBidDominant() const { return _result.bid_dominant; }
    
    /// Check if ask side is dominant
    bool isAskDominant() const { return _result.ask_dominant; }
    
private:
    Config _cfg;
    bool _enabled;
    BookImbalanceSignalResult _result;
};

} // namespace futu
