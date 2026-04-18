/*!
 * \file PredictiveToxicity.cpp
 * \brief Predictive Toxicity Detection Implementation
 */

#include "PredictiveToxicity.h"
#include "../Includes/WTSDataDef.hpp"
#include <algorithm>
#include <cmath>

namespace futu {

PredictiveToxicity::PredictiveToxicity()
    : _has_alpha_data(false)
    , _cache_dirty(true)
{
}

//------------------------------------------------------------------------------
// Configuration
//------------------------------------------------------------------------------

void PredictiveToxicity::setBucketSize(double bucket_size)
{
    _bucket_size = bucket_size > 0 ? bucket_size : _cfg.vpin_bucket_size;
}

//------------------------------------------------------------------------------
// Data Input
//------------------------------------------------------------------------------

void PredictiveToxicity::updateAlpha(const AlphaResult& alpha, const TradeImbalanceResult& tradeImb)
{
    _latest_alpha = alpha;
    _latest_trade_imb = tradeImb;
    _has_alpha_data = true;
    _cache_dirty = true;
}

//------------------------------------------------------------------------------
// VPIN Analysis
//------------------------------------------------------------------------------

void PredictiveToxicity::onTrade(double price, double qty, bool isBuy, uint64_t timestamp)
{
    if (_bucket_size <= 0) return;

    if (isBuy) {
        _current_bucket.buy_volume += qty;
    } else {
        _current_bucket.sell_volume += qty;
    }
    _current_bucket.total_volume += qty;
    _current_bucket.end_time = timestamp;
    
    if (_current_bucket.start_time == 0) {
        _current_bucket.start_time = timestamp;
    }
    
    // Check if bucket is full
    if (_current_bucket.total_volume >= _bucket_size) {
        // Calculate order imbalance
        double imbalance = std::abs(_current_bucket.buy_volume - _current_bucket.sell_volume);
        _order_imbalances.push_back(imbalance);
        
        // Maintain fixed window
        if (_order_imbalances.size() > _cfg.vpin_window) {
            _order_imbalances.erase(_order_imbalances.begin());
        }
        
        // Calculate VPIN
        double sum_imbalance = 0;
        for (double imb : _order_imbalances) {
            sum_imbalance += imb;
        }
        
        if (!_order_imbalances.empty() && _bucket_size > 0) {
            _vpin = sum_imbalance / (_order_imbalances.size() * _bucket_size);
        }
        
        // Save and reset bucket
        _buckets.push_back(_current_bucket);
        if (_buckets.size() > _cfg.vpin_window) {
            _buckets.erase(_buckets.begin());
        }
        
        _current_bucket = VolumeBucket();
        _current_bucket.start_time = timestamp;
        
        _cache_dirty = true;
    }
}

void PredictiveToxicity::onTickVolume(const char* stdCode, const wtp::WTSTickData* tick)
{
    if (!tick) return;
    
    if (_bucket_size <= 0) {
        setBucketSize(_cfg.vpin_bucket_size);
        if (_bucket_size <= 0) return;
    }
    
    double qty = tick->volume();
    if (qty <= 0) return;
    
    double price = tick->price();
    uint64_t timestamp = tick->actiontime();
    
    // Infer trade direction
    bool isBuy = true;
    auto it = _last_ticks.find(stdCode);
    if (it != _last_ticks.end()) {
        const LastTickInfo& last = it->second;
        double last_mid = (last.bid_px + last.ask_px) / 2.0;
        double current_mid = (tick->bidprice(0) + tick->askprice(0)) / 2.0;
        
        if (price >= tick->askprice(0)) {
            isBuy = true;
        } else if (price <= tick->bidprice(0)) {
            isBuy = false;
        } else if (current_mid > last_mid) {
            isBuy = true;
        } else if (current_mid < last_mid) {
            isBuy = false;
        } else {
            onTrade(price, qty / 2.0, true, timestamp);
            onTrade(price, qty / 2.0, false, timestamp);
            _last_ticks[stdCode] = {tick->bidprice(0), tick->askprice(0), tick->totalvolume()};
            return;
        }
    }
    
    onTrade(price, qty, isBuy, timestamp);
    _last_ticks[stdCode] = {tick->bidprice(0), tick->askprice(0), tick->totalvolume()};
}

//------------------------------------------------------------------------------
// Cache Update
//------------------------------------------------------------------------------

void PredictiveToxicity::updateCache() const
{
    if (!_cache_dirty) return;
    
    _cached_result = PredictiveToxicityResult();
    
    // VPIN
    _cached_result.vpin = _vpin;
    
    if (_has_alpha_data)
    {
        // OFI toxicity
        _cached_result.ofi_toxicity = std::abs(_latest_alpha.ofi_component);
        
        // Trade imbalance toxicity
        _cached_result.trade_toxicity = std::abs(_latest_trade_imb.imbalance_ratio) * 
            (0.5 + 0.5 * _latest_trade_imb.large_trade_ratio);
        
        // Combined alpha toxicity
        _cached_result.alpha_toxicity = 
            _cfg.ofi_weight * _cached_result.ofi_toxicity +
            _cfg.trade_weight * _cached_result.trade_toxicity;
        
        // Check for extreme signals (> 0.9)
        if (_cached_result.ofi_toxicity > 0.9 || _cached_result.trade_toxicity > 0.9)
        {
            _cached_result.extreme_signal = std::max(_cached_result.ofi_toxicity, _cached_result.trade_toxicity);
        }
    }
    
    // Combined score: max of VPIN and alpha
    _cached_result.combined_score = std::max(_cached_result.vpin, _cached_result.alpha_toxicity);
    
    // Apply extreme signal boost
    if (_cached_result.extreme_signal > 0)
    {
        _cached_result.combined_score = std::max(_cached_result.combined_score, _cached_result.extreme_signal * 0.8);
    }
    
    // Is toxic?
    _cached_result.is_toxic = 
        _cached_result.combined_score > _cfg.alpha_threshold ||
        _cached_result.vpin > _cfg.vpin_threshold;
    
    // Toxic side
    if (_cached_result.is_toxic && _has_alpha_data)
    {
        if (_latest_alpha.ofi_component > 0 && _latest_trade_imb.imbalance_ratio > 0)
            _cached_result.toxic_side = 1;
        else if (_latest_alpha.ofi_component < 0 && _latest_trade_imb.imbalance_ratio < 0)
            _cached_result.toxic_side = -1;
    }
    
    _cache_dirty = false;
}

//------------------------------------------------------------------------------
// Analysis
//------------------------------------------------------------------------------

PredictiveToxicityResult PredictiveToxicity::analyze() const
{
    updateCache();
    return _cached_result;
}

double PredictiveToxicity::getToxicityScore() const
{
    updateCache();
    return _cached_result.combined_score;
}

//------------------------------------------------------------------------------
// Reset
//------------------------------------------------------------------------------

void PredictiveToxicity::reset()
{
    _has_alpha_data = false;
    _cache_dirty = true;
    _cached_result = PredictiveToxicityResult();
    
    _buckets.clear();
    _order_imbalances.clear();
    _current_bucket = VolumeBucket();
    _vpin = 0.0;
    _last_ticks.clear();
}

} // namespace futu
