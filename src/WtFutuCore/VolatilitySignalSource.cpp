// src/WtFutuCore/VolatilitySignalSource.cpp
#include "VolatilitySignalSource.h"
#include "MarketDataContext.h"
#include "FutuConfig.h"
#include <algorithm>

namespace futu {

RealizedVolSignalSource::RealizedVolSignalSource()
{
    _result.type = SignalType::VOLATILITY;
}

void RealizedVolSignalSource::setWindowSize(uint32_t windowSize)
{
    _window_size = std::min(windowSize, static_cast<uint32_t>(MAX_BUFFER));
}

void RealizedVolSignalSource::update(const MarketDataContext& book)
{
    if (!_enabled) return;
    
    double mid = book.getMidPrice();
    if (mid <= 0) return;
    
    _result.timestamp = book.getTimestamp();
    
    // Incremental return calculation
    if (_last_mid > 0)
    {
        double ret = (mid - _last_mid) / _last_mid;
        
        // Handle full buffer
        if (_returns.size() >= _window_size)
        {
            double old_ret = _returns.front();
            _running_sum -= old_ret;
            _running_sum_sq -= old_ret * old_ret;
            _returns.pop();
        }
        
        // Add new return
        _returns.push(ret);
        _running_sum += ret;
        _running_sum_sq += ret * ret;
        
        updateVolatility();
    }
    
    _last_mid = mid;
    _result.valid = (_returns.size() >= std::min(_window_size / 2, 5u));
}

void RealizedVolSignalSource::updateVolatility()
{
    double n = static_cast<double>(_returns.size());
    if (n < 2) return;
    
    // Variance = E[X^2] - E[X]^2
    double mean = _running_sum / n;
    double variance = (_running_sum_sq / n) - (mean * mean);
    
    // Bessel's correction
    variance *= n / (n - 1.0);
    _result.realized_vol = std::sqrt(std::max(0.0, variance));
    
    // For now, composite vol = realized vol.
    // Future: blend with micro-structure signals (imbalance flux, etc.)
    _result.composite_vol = _result.realized_vol;
    
    // Update percentile and tier
    _result.vol_percentile = getVolPercentile();
    _result.vol_tier = determineTier(_result.vol_percentile);
}

double RealizedVolSignalSource::getVolPercentile() const
{
    double vol = _result.realized_vol;
    // FIX P2-12: 使用可配置的百分位分箱阈值替代硬编码
    // 原代码硬编码阈值对不同品种不适用(如原油vs豆粕波动率差异大)。
    // 现从_percentile_bins读取，可通过setPercentileBins()或配置文件调整。
    if (vol < _percentile_bins.vol_p10) return 10.0;
    if (vol < _percentile_bins.vol_p25) return 25.0;
    if (vol < _percentile_bins.vol_p50) return 50.0;
    if (vol < _percentile_bins.vol_p70) return 70.0;
    if (vol < _percentile_bins.vol_p85) return 85.0;
    return 95.0;
}

VolTier RealizedVolSignalSource::determineTier(double percentile)
{
    if (percentile < 20.0) return VolTier::LOW;
    if (percentile < 60.0) return VolTier::NORMAL;
    if (percentile < 85.0) return VolTier::ELEVATED;
    return VolTier::EXTREME;
}

void RealizedVolSignalSource::reset()
{
    _returns.clear();
    _running_sum = 0;
    _running_sum_sq = 0;
    _last_mid = 0;
    _result = VolatilitySignalResult();
    _result.type = SignalType::VOLATILITY;
}

//------------------------------------------------------------------------------

// FIX P2-12: PercentileBins::fromVariant — 从配置文件读取百分位阈值
// Note: defined outside the struct with full qualified name
auto
RealizedVolSignalSource::PercentileBins::fromVariant(wtp::WTSVariant* v)
    -> PercentileBins
{
    PercentileBins bins;
    if (!v) return bins;
    bins.vol_p10 = FutuConfig::readDouble(v, "volP10", 0.0003);
    bins.vol_p25 = FutuConfig::readDouble(v, "volP25", 0.0005);
    bins.vol_p50 = FutuConfig::readDouble(v, "volP50", 0.001);
    bins.vol_p70 = FutuConfig::readDouble(v, "volP70", 0.002);
    bins.vol_p85 = FutuConfig::readDouble(v, "volP85", 0.003);
    return bins;
}

} // namespace futu
