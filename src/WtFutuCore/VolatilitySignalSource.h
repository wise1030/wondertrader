// src/WtFutuCore/VolatilitySignalSource.h
#pragma once

#include "ISignalSource.h"
#include "../Share/RingBuffer.hpp"
#include <cmath>

namespace futu {

class MarketDataContext;

/// Implementation of VolatilitySignalSource focusing on realized and composite vol
class RealizedVolSignalSource : public VolatilitySignalSource
{
public:
    RealizedVolSignalSource();
    virtual ~RealizedVolSignalSource() = default;
    
    // ISignalSource implementation
    virtual const std::string& name() const override { return _name; }
    virtual SignalType type() const override { return SignalType::VOLATILITY; }
    virtual void update(const MarketDataContext& book) override;
    virtual void reset() override;
    virtual bool enabled() const override { return _enabled; }
    virtual void setEnabled(bool enabled) override { _enabled = enabled; }
    virtual const SignalResult& result() const override { return _result; }
    
    // VolatilitySignalSource implementation
    virtual const VolatilitySignalResult& getVolatility() const override { return _result; }
    virtual double getVolPercentile() const override;
    
    // Configuration
    void setWindowSize(uint32_t windowSize);
    void setVpinWeight(double weight) { _vpin_weight = weight; }

private:
    std::string _name = "RealizedVol";
    bool _enabled = true;
    VolatilitySignalResult _result;
    
    // Incremental volatility calculation
    static constexpr size_t MAX_BUFFER = 256;
    RingBuffer<double, MAX_BUFFER> _returns;
    double _running_sum = 0;
    double _running_sum_sq = 0;
    uint32_t _window_size = 100;
    
    double _vpin_weight = 0.3;
    double _last_mid = 0;
    
    void updateVolatility();
    VolTier determineTier(double percentile);
};

} // namespace futu
