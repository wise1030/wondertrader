/*!
 * \file SyntheticSignalFusion.cpp
 * \brief Implementation of Multi-Source Signal Fusion
 */
#include "SyntheticSignalFusion.h"
#include <cmath>
#include <algorithm>

namespace futu {

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------

SyntheticSignalFusion::SyntheticSignalFusion()
    : _volatility(0.5)
    , _liquidity(0.5)
    , _last_vol_price(0)
    , _last_vol_timestamp(0)
    , _accumulated_buy_vol(0)
    , _accumulated_sell_vol(0)
    , _large_volume(0)
    , _total_volume(0)
    , _has_tick_inf(false)
    , _has_book_sig(false)
    , _has_calibration(false)
{
}

SyntheticSignalFusion::SyntheticSignalFusion(const FusionConfig& config)
    : _config(config)
    , _volatility(0.5)
    , _liquidity(0.5)
    , _last_vol_price(0)
    , _last_vol_timestamp(0)
    , _accumulated_buy_vol(0)
    , _accumulated_sell_vol(0)
    , _large_volume(0)
    , _total_volume(0)
    , _has_tick_inf(false)
    , _has_book_sig(false)
    , _has_calibration(false)
{
}

//------------------------------------------------------------------------------
// Reset
//------------------------------------------------------------------------------

void SyntheticSignalFusion::reset()
{
    _latest_tick_inf = InferredTransaction();
    _latest_book_sig = DepthImbalanceSignal();
    _latest_calibration = CalibrationResult();
    
    _fused_history.clear();
    _accumulated_buy_vol = 0;
    _accumulated_sell_vol = 0;
    _large_volume = 0;
    _total_volume = 0;
    
    _has_tick_inf = false;
    _has_book_sig = false;
    _has_calibration = false;
}

//------------------------------------------------------------------------------
// Signal Input
//------------------------------------------------------------------------------

void SyntheticSignalFusion::addTickInference(const InferredTransaction& tick_inf)
{
    _latest_tick_inf = tick_inf;
    _has_tick_inf = true;
}

void SyntheticSignalFusion::addBookSignal(const DepthImbalanceSignal& book_sig)
{
    _latest_book_sig = book_sig;
    _has_book_sig = true;
}

void SyntheticSignalFusion::addBookAnalysis(const BookAnalysisResult& analysis)
{
    _latest_book_sig.weighted_imbalance = analysis.imbalance_score;
    _latest_book_sig.pressure_intensity = analysis.liquidity_score;
    _latest_book_sig.bid_dominant = analysis.imbalance_score > 0.2;
    _latest_book_sig.ask_dominant = analysis.imbalance_score < -0.2;
    _latest_book_sig.confidence = analysis.direction_clear ? 0.8 : 0.4;
    _latest_book_sig.timestamp = 0;  // Caller should set if needed
    _has_book_sig = true;
}

void SyntheticSignalFusion::addSelfTradeCalibration(const CalibrationResult& calib)
{
    _latest_calibration = calib;
    _has_calibration = true;
}

//------------------------------------------------------------------------------
// Market State
//------------------------------------------------------------------------------

void SyntheticSignalFusion::updateVolatility(double volatility)
{
    _volatility = volatility;
}

void SyntheticSignalFusion::updateLiquidity(double liquidity)
{
    _liquidity = liquidity;
}

void SyntheticSignalFusion::onTick(const std::string& code, double mid_price, uint64_t timestamp)
{
    // Update volatility estimate from tick
    // Simple rolling volatility estimate based on price changes
    (void)code;  // Unused for now
    
    // Evaluate previous predictions
    if (_last_recorded_price > 0 && std::abs(mid_price - _last_recorded_price) >= 0.5) // Adjust minimum move threshold as needed, 0.5 is half a tick typically
    {
        bool actual_up = (mid_price > _last_recorded_price);
        
        if (std::abs(_last_tick_direction) > 0.1) {
            _tick_accuracy.addPrediction(_last_tick_direction > 0, actual_up);
        }
        if (std::abs(_last_book_direction) > 0.1) {
            _book_accuracy.addPrediction(_last_book_direction > 0, actual_up);
        }
        if (std::abs(_last_self_trade_direction) > 0.1) {
            // self_trade_direction is negative of bias, so > 0 means bullish
            _self_trade_accuracy.addPrediction(_last_self_trade_direction > 0, actual_up);
        }
        _last_recorded_price = mid_price;
    }
    else if (_last_recorded_price <= 0)
    {
        _last_recorded_price = mid_price;
    }
    
    // Store price history for volatility calculation
    if (_last_vol_price > 0 && _last_vol_timestamp > 0) {
        double price_change = std::abs(mid_price - _last_vol_price) / mid_price;
        _volatility = 0.9 * _volatility + 0.1 * price_change * 100;
        _volatility = std::min(1.0, std::max(0.0, _volatility));
    }
    
    _last_vol_price = mid_price;
    _last_vol_timestamp = timestamp;
}

//------------------------------------------------------------------------------
// Adaptive Weights Calculation
//------------------------------------------------------------------------------

AdaptiveWeights SyntheticSignalFusion::calculateAdaptiveWeights() const
{
    AdaptiveWeights w;
    
    // Start with base weights
    w.tick = _config.tick_inference_base_weight;
    w.book = _config.book_signal_base_weight;
    w.self_trade = _config.self_trade_base_weight;
    
    if (!_config.adaptive_weights) {
        return w;
    }
    
    // 1. High volatility: reduce tick inference weight (more noise)
    if (_volatility > 0.5) {
        double reduction = _config.volatility_sensitivity * (_volatility - 0.5) * 2.0;
        reduction = std::min(reduction, 0.5);  // Cap at 50% reduction
        
        double saved = w.tick * reduction;
        w.tick *= (1.0 - reduction);
        w.book += saved * 0.5;
        w.self_trade += saved * 0.5;
    }
    
    // 2. Low liquidity: increase book weight (order book is more informative)
    if (_liquidity < 0.3) {
        double boost = (0.3 - _liquidity) * _config.liquidity_sensitivity * 3.0;
        boost = std::min(boost, 0.5);  // Cap at 50% boost
        
        w.book *= (1.0 + boost);
        w.tick *= (1.0 - boost * 0.5);
        w.self_trade *= (1.0 - boost * 0.5);
    }
    
    // 3. Self-trade sample size adjustment
    if (_has_calibration && _latest_calibration.sample_size < _config.min_self_trade_samples) {
        double ratio = _latest_calibration.sample_size / _config.min_self_trade_samples;
        double saved = w.self_trade * (1.0 - ratio);
        w.self_trade *= ratio;
        w.tick += saved * 0.5;
        w.book += saved * 0.5;
    } else if (!_has_calibration) {
        // No calibration data, redistribute weight
        double saved = w.self_trade;
        w.self_trade = 0;
        w.tick += saved * 0.5;
        w.book += saved * 0.5;
    }
    
    // 4. Confidence-based adjustment
    if (_has_tick_inf && _has_book_sig) {
        // Adjust based on relative confidence
        double tick_conf = _latest_tick_inf.confidence;
        double book_conf = _latest_book_sig.confidence;
        double total_conf = tick_conf + book_conf;
        
        if (total_conf > 0) {
            double conf_ratio_tick = tick_conf / total_conf;
            double conf_ratio_book = book_conf / total_conf;
            
            // Blend original weights with confidence-weighted weights
            w.tick = 0.7 * w.tick + 0.3 * conf_ratio_tick * (w.tick + w.book);
            w.book = 0.7 * w.book + 0.3 * conf_ratio_book * (w.tick + w.book);
        }
    }
    
    // 5. Accuracy-based adjustment
    // Weight goes up if accuracy > 0.5, down if < 0.5
    double tick_acc_adj = (_tick_accuracy.accuracy - 0.5) * 0.2; // Max +/- 10%
    double book_acc_adj = (_book_accuracy.accuracy - 0.5) * 0.2;
    double self_trade_acc_adj = (_self_trade_accuracy.accuracy - 0.5) * 0.2;
    
    w.tick *= (1.0 + tick_acc_adj);
    w.book *= (1.0 + book_acc_adj);
    w.self_trade *= (1.0 + self_trade_acc_adj);
    
    // Normalize to sum to 1.0
    double total = w.tick + w.book + w.self_trade;
    if (total > 0) {
        w.tick /= total;
        w.book /= total;
        w.self_trade /= total;
    }
    
    return w;
}

//------------------------------------------------------------------------------
// Fuse
//------------------------------------------------------------------------------

SyntheticTransactionData SyntheticSignalFusion::fuse()
{
    SyntheticTransactionData result;
    
    // Get adaptive weights
    AdaptiveWeights w = calculateAdaptiveWeights();
    
    // Store weights in result
    result.tick_inference_weight = w.tick;
    result.book_signal_weight = w.book;
    result.self_trade_weight = w.self_trade;
    
    // Calculate direction signal from each source
    double direction = 0.0;
    double total_confidence = 0.0;
    
    // 1. Tick inference direction
    if (_has_tick_inf) {
        double tick_dir = 0.0;
        if (_latest_tick_inf.is_buy_initiated && !_latest_tick_inf.is_sell_initiated) {
            tick_dir = 1.0;
        } else if (_latest_tick_inf.is_sell_initiated && !_latest_tick_inf.is_buy_initiated) {
            tick_dir = -1.0;
        }
        
        _last_tick_direction = tick_dir;
        direction += w.tick * tick_dir * _latest_tick_inf.confidence;
        total_confidence += w.tick * _latest_tick_inf.confidence;
        
        result.price = _latest_tick_inf.price;
        result.volume = _latest_tick_inf.volume;
        result.timestamp = _latest_tick_inf.timestamp;
    }
    
    // 2. Order book signal
    if (_has_book_sig) {
        double book_dir = 0.0;
        if (_latest_book_sig.bid_dominant && !_latest_book_sig.ask_dominant) {
            book_dir = 1.0;
        } else if (_latest_book_sig.ask_dominant && !_latest_book_sig.bid_dominant) {
            book_dir = -1.0;
        }
        
        _last_book_direction = book_dir;
        direction += w.book * book_dir * _latest_book_sig.confidence;
        total_confidence += w.book * _latest_book_sig.confidence;
        
        if (result.timestamp == 0) {
            result.timestamp = _latest_book_sig.timestamp;
        }
    }
    
    // 3. Self-trade calibration (ground truth bias correction)
    if (_has_calibration && _latest_calibration.sample_size >= _config.min_self_trade_samples) {
        // Direction bias indicates which side is more toxic
        // Positive bias = buy side is more toxic (we lose on buys)
        // Negative bias = sell side is more toxic (we lose on sells)
        double self_dir = -_latest_calibration.direction_bias;  // Invert: avoid toxic side
        
        _last_self_trade_direction = self_dir;
        direction += w.self_trade * self_dir * _latest_calibration.confidence;
        total_confidence += w.self_trade * _latest_calibration.confidence;
        
        result.has_calibration = true;
    }
    
    // Determine final direction
    result.direction_signal = direction;
    
    // Determine buy/sell initiation
    if (direction > 0.1) {
        result.is_buy_initiated = true;
    } else if (direction < -0.1) {
        result.is_buy_initiated = false;  // Sell-initiated
    }
    
    // Calculate overall confidence
    if (total_confidence > 0) {
        result.confidence = std::abs(direction) * total_confidence;
    }
    result.confidence = std::min(1.0, result.confidence);
    
    // Determine if strong signal
    result.is_strong_signal = std::abs(direction) > _config.strong_signal_threshold 
                              && result.confidence > _config.min_confidence;
    
    // Update accumulated statistics
    if (result.confidence >= _config.min_confidence) {
        FusedSample sample;
        sample.signed_volume = result.volume * result.confidence;
        if (direction < 0) {
            sample.signed_volume = -sample.signed_volume;
        }
        sample.confidence = result.confidence;
        sample.timestamp = result.timestamp;
        
        _fused_history.push_back(sample);
        
        // Update accumulators
        if (result.is_buy_initiated) {
            _accumulated_buy_vol += result.volume * result.confidence;
        } else {
            _accumulated_sell_vol += result.volume * result.confidence;
        }
        
        _total_volume += result.volume;
        
        // Prune old history
        pruneHistory(result.timestamp);
    }
    
    return result;
}

//------------------------------------------------------------------------------
// Prune History
//------------------------------------------------------------------------------

void SyntheticSignalFusion::pruneHistory(uint64_t current_time)
{
    uint64_t window_start = current_time - _config.imbalance_window_ms;
    
    while (!_fused_history.empty() && _fused_history.front().timestamp < window_start) {
        const auto& oldest = _fused_history.front();
        
        if (oldest.signed_volume > 0) {
            _accumulated_buy_vol -= oldest.signed_volume;
        } else {
            _accumulated_sell_vol += oldest.signed_volume;  // Add because it was negative
        }
        
        _fused_history.pop_front();
    }
}

//------------------------------------------------------------------------------
// Fused Imbalance
//------------------------------------------------------------------------------

FusedTradeImbalance SyntheticSignalFusion::getFusedImbalance() const
{
    FusedTradeImbalance result;
    
    result.net_flow = _accumulated_buy_vol - _accumulated_sell_vol;
    
    double total = _accumulated_buy_vol + _accumulated_sell_vol;
    if (total > 0) {
        result.imbalance_ratio = result.net_flow / total;
    }
    
    if (_total_volume > 0) {
        result.large_trade_ratio = _large_volume / _total_volume;
    }
    
    // Calculate average confidence
    if (!_fused_history.empty()) {
        double total_conf = 0;
        for (const auto& sample : _fused_history) {
            total_conf += sample.confidence;
        }
        result.confidence = total_conf / _fused_history.size();
    }
    
    result.sample_count = static_cast<uint32_t>(_fused_history.size());
    
    // Calculate fusion quality
    result.fusion_quality = 0.0;
    if (_has_tick_inf) result.fusion_quality += 0.4;
    if (_has_book_sig) result.fusion_quality += 0.4;
    if (_has_calibration && _latest_calibration.sample_size >= _config.min_self_trade_samples) {
        result.fusion_quality += 0.2;
    }
    
    return result;
}

//------------------------------------------------------------------------------
// Current Weights
//------------------------------------------------------------------------------

AdaptiveWeights SyntheticSignalFusion::getCurrentWeights() const
{
    return calculateAdaptiveWeights();
}

} // namespace futu
