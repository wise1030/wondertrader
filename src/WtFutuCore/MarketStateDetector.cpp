/*!
 * \file MarketStateDetector.cpp
 * \brief Market State Detection Implementation
 */
#include "MarketStateDetector.h"
#include "../Includes/WTSDataDef.hpp"
#include <algorithm>
#include <numeric>

namespace futu {

MarketStateDetector::MarketStateDetector()
    : _tick_size(0.2)
    , _current_state(MarketState::NORMAL)
    , _ticks_in_state(0)
    , _ticks_since_change(0)
    , _cache_dirty(true)
    , _trading_day(0)
    , _in_session(false)
{
    // deque doesn't have reserve, set max size via resize or just let it grow
}

void MarketStateDetector::onTick(wtp::WTSTickData* tick)
{
    if (!tick) return;
    
    double mid = (tick->bidprice(0) + tick->askprice(0)) / 2.0;
    if (mid <= 0) return;
    
    // Update price history
    _price_history.push_back(mid);
    if (_price_history.size() > _params.lookback_ticks)
        _price_history.pop_front();
    
    // Update spread history
    double spread = tick->askprice(0) - tick->bidprice(0);
    _spread_history.push_back(spread);
    if (_spread_history.size() > _params.lookback_ticks)
        _spread_history.pop_front();
    
    // Update volume history (sum of bid and ask at best)
    double volume = tick->bidqty(0) + tick->askqty(0);
    _volume_history.push_back(volume);
    if (_volume_history.size() > _params.lookback_ticks)
        _volume_history.pop_front();
    
    // Update state tracking
    _ticks_in_state++;
    _ticks_since_change++;
    _cache_dirty = true;
    _in_session = true;
    
    // Run detection
    DetectionResult result = detect();
    if (result.state != _current_state)
    {
        _current_state = result.state;
        _ticks_in_state = 0;
        _ticks_since_change = 0;
    }
}

void MarketStateDetector::onSessionBegin(uint32_t tradingDay)
{
    _trading_day = tradingDay;
    _in_session = true;
    reset();
}

void MarketStateDetector::onSessionEnd(uint32_t tradingDay)
{
    _in_session = false;
    _current_state = MarketState::CLOSED;
}

DetectionResult MarketStateDetector::detect() const
{
    if (!_cache_dirty)
        return _cached_result;
    
    DetectionResult result;
    result.state = MarketState::NORMAL;
    result.confidence = 0.5;
    
    // Check various conditions in priority order
    
    // 1. Check for circuit breaker / halt
    // (would need external signal, skip for now)
    
    // 2. Check for auction period
    if (isAuctionPeriod())
    {
        result.state = MarketState::AUCTION;
        result.confidence = 0.9;
        result.should_pause = true;
        _cached_result = result;
        _cache_dirty = false;
        return result;
    }
    
    // 3. Check for fast move
    double moveEst;
    if (isFastMove(moveEst))
    {
        result.state = MarketState::FAST_MOVE;
        result.confidence = 0.8;
        result.should_widen = true;
        result.should_hedge = true;
        result.momentum = getMomentum();
        _cached_result = result;
        _cache_dirty = false;
        return result;
    }
    
    // 4. Check for elevated volatility
    double volEst;
    if (isVolatilityElevated(volEst))
    {
        result.state = MarketState::ELEVATED_VOL;
        result.confidence = 0.7;
        result.should_widen = true;
        result.vol_estimate = volEst;
        _cached_result = result;
        _cache_dirty = false;
        return result;
    }
    
    // 5. Check for low liquidity
    double spreadEst;
    if (isLowLiquidity(spreadEst))
    {
        result.state = MarketState::LOW_LIQUIDITY;
        result.confidence = 0.6;
        result.should_widen = true;
        result.spread_estimate = spreadEst;
        _cached_result = result;
        _cache_dirty = false;
        return result;
    }
    
    // Normal market
    result.state = MarketState::NORMAL;
    result.confidence = 0.8;
    result.momentum = getMomentum();
    result.spread_estimate = getSpreadInTicks();
    
    _cached_result = result;
    _cache_dirty = false;
    return result;
}

bool MarketStateDetector::isVolatilityElevated(double& volEstimate) const
{
    if (_price_history.size() < 10)
        return false;
    
    // Calculate price returns
    std::vector<double> returns;
    for (size_t i = 1; i < _price_history.size(); i++)
    {
        double ret = (_price_history[i] - _price_history[i-1]) / _price_history[i-1];
        returns.push_back(ret);
    }
    
    // Calculate standard deviation
    double sum = 0, sum_sq = 0;
    for (double r : returns)
    {
        sum += r;
        sum_sq += r * r;
    }
    
    double n = static_cast<double>(returns.size());
    double mean = sum / n;
    double variance = (sum_sq / n) - (mean * mean);
    volEstimate = std::sqrt(std::max(0.0, variance));
    
    return volEstimate > _params.vol_threshold;
}

bool MarketStateDetector::isFastMove(double& moveEstimate) const
{
    if (_price_history.size() < 5)
        return false;
    
    // Calculate price move over lookback
    double start_price = _price_history.front();
    double end_price = _price_history.back();
    
    if (start_price <= 0) return false;
    
    moveEstimate = std::abs((end_price - start_price) / start_price);
    
    return moveEstimate > _params.move_threshold;
}

bool MarketStateDetector::isLowLiquidity(double& spreadEstimate) const
{
    if (_spread_history.empty())
        return false;
    
    // Calculate average spread
    double sum = 0;
    for (double s : _spread_history)
        sum += s;
    
    spreadEstimate = sum / _spread_history.size() / _tick_size;
    
    // Also check volume
    double avg_vol = 0;
    if (!_volume_history.empty())
    {
        for (double v : _volume_history)
            avg_vol += v;
        avg_vol /= _volume_history.size();
    }
    
    // Low liquidity = wide spread OR low volume
    bool wide_spread = spreadEstimate > _params.spread_threshold;
    bool low_volume = avg_vol < _params.volume_threshold;
    
    return wide_spread || low_volume;
}

bool MarketStateDetector::isAuctionPeriod() const
{
    // This would need market-specific logic
    // For now, return false (would be set by external signal)
    return false;
}

double MarketStateDetector::getMomentum() const
{
    if (_price_history.size() < 5)
        return 0;
    
    // Simple momentum: recent price vs earlier price
    size_t n = _price_history.size();
    double recent = 0, earlier = 0;
    
    for (size_t i = n/2; i < n; i++)
        recent += _price_history[i];
    recent /= (n - n/2);
    
    for (size_t i = 0; i < n/2; i++)
        earlier += _price_history[i];
    earlier /= (n/2);
    
    if (earlier <= 0) return 0;
    
    double momentum = (recent - earlier) / earlier;
    
    // Normalize to -1 to 1
    return std::max(-1.0, std::min(1.0, momentum * 100));
}

double MarketStateDetector::getSpreadInTicks() const
{
    if (_spread_history.empty())
        return 0;
    
    double sum = 0;
    for (double s : _spread_history)
        sum += s;
    
    return (sum / _spread_history.size()) / _tick_size;
}

void MarketStateDetector::reset()
{
    _price_history.clear();
    _spread_history.clear();
    _volume_history.clear();
    _current_state = MarketState::NORMAL;
    _ticks_in_state = 0;
    _ticks_since_change = 0;
    _cache_dirty = true;
}

} // namespace futu
