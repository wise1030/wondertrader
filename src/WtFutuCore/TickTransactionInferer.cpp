/*!
 * \file TickTransactionInferer.cpp
 * \brief Implementation of Tick-Level Transaction Inference
 */
#include "TickTransactionInferer.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/TimeUtils.hpp"
#include <cmath>
#include <algorithm>

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

namespace futu {

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------

TickTransactionInferer::TickTransactionInferer()
    : _large_volume(0)
    , _total_volume(0)
{
}

//------------------------------------------------------------------------------
// Reset
//------------------------------------------------------------------------------

void TickTransactionInferer::reset()
{
    _prev_state = TickState();
    _inference_history.clear();
    resetAccumulation();
}

void TickTransactionInferer::resetAccumulation()
{
    _accumulated.reset();
    _large_volume = 0;
    _total_volume = 0;
}

//------------------------------------------------------------------------------
// Main Inference Logic
//------------------------------------------------------------------------------

InferenceTransaction TickTransactionInferer::inferFromTick(wtp::WTSTickData* tick)
{
    if (!tick) {
        return InferredTransaction();
    }
    
    auto& tickStruct = tick->getTickStruct();
    
    return inferFromTick(
        tickStruct.bid_price[0],
        tickStruct.ask_price[0],
        tickStruct.bid_qty[0],
        tickStruct.ask_qty[0],
        tickStruct.price,
        tickStruct.total_volume,
        tickStruct.action_time
    );
}

InferenceTransaction TickTransactionInferer::inferFromTick(
    double bid_px, double ask_px,
    double bid_vol, double ask_vol,
    double last_price, double last_vol,
    uint64_t timestamp
)
{
    InferredTransaction result;
    result.price = last_price;
    result.timestamp = timestamp;
    
    // First tick - no previous state to compare
    if (!_prev_state.initialized) {
        _prev_state.bid_px = bid_px;
        _prev_state.ask_px = ask_px;
        _prev_state.bid_vol = bid_vol;
        _prev_state.ask_vol = ask_vol;
        _prev_state.last_price = last_price;
        _prev_state.timestamp = timestamp;
        _prev_state.initialized = true;
        result.method = InferenceMethod::UNKNOWN;
        result.confidence = 0;
        return result;
    }
    
    // Calculate changes
    double bid_vol_change = _prev_state.bid_vol - bid_vol;  // Positive = volume removed
    double ask_vol_change = _prev_state.ask_vol - ask_vol;  // Positive = volume removed
    double price_change = last_price - _prev_state.last_price;
    
    // Detect method and direction
    result.method = detectMethod(
        bid_px, ask_px,
        _prev_state.bid_px, _prev_state.ask_px,
        last_price, _prev_state.last_price
    );
    
    // Calculate inferred volume
    result.volume = calculateInferredVolume(
        bid_vol, _prev_state.bid_vol,
        ask_vol, _prev_state.ask_vol,
        last_vol  // Using total volume change would be better if available
    );
    
    // Calculate confidence
    result.confidence = calculateConfidence(
        result.method,
        bid_vol_change, ask_vol_change,
        price_change
    );
    
    // Determine direction based on method
    switch (result.method) {
        case InferenceMethod::ASK_DEPLETION:
            // Sell-side volume removed = aggressive buying
            result.is_buy_initiated = true;
            result.is_sell_initiated = false;
            break;
            
        case InferenceMethod::BID_DEPLETION:
            // Buy-side volume removed = aggressive selling
            result.is_buy_initiated = false;
            result.is_sell_initiated = true;
            break;
            
        case InferenceMethod::PRICE_UP:
        case InferenceMethod::SPREAD_CROSS_UP:
            // Price moved up = buy-initiated
            result.is_buy_initiated = true;
            result.is_sell_initiated = false;
            break;
            
        case InferenceMethod::PRICE_DOWN:
        case InferenceMethod::SPREAD_CROSS_DOWN:
            // Price moved down = sell-initiated
            result.is_buy_initiated = false;
            result.is_sell_initiated = true;
            break;
            
        case InferenceMethod::VOLUME_SURGE:
            // Large volume - need to infer from price direction
            if (price_change > 0) {
                result.is_buy_initiated = true;
                result.is_sell_initiated = false;
            } else if (price_change < 0) {
                result.is_buy_initiated = false;
                result.is_sell_initiated = true;
            }
            break;
            
        default:
            // UNKNOWN - cannot determine
            result.is_buy_initiated = false;
            result.is_sell_initiated = false;
            break;
    }
    
    // Update previous state
    _prev_state.bid_px = bid_px;
    _prev_state.ask_px = ask_px;
    _prev_state.bid_vol = bid_vol;
    _prev_state.ask_vol = ask_vol;
    _prev_state.last_price = last_price;
    _prev_state.timestamp = timestamp;
    
    // Update accumulation if we have a direction
    if (result.confidence >= _config.min_confidence) {
        updateAccumulation(result);
    }
    
    return result;
}

//------------------------------------------------------------------------------
// Method Detection
//------------------------------------------------------------------------------

InferenceMethod TickTransactionInferer::detectMethod(
    double bid_px, double ask_px,
    double prev_bid_px, double prev_ask_px,
    double last_price, double prev_last_price
)
{
    double price_change = last_price - prev_last_price;
    double tick_size = _config.tick_size;
    
    // Check for spread cross
    double prev_mid = (prev_bid_px + prev_ask_px) / 2.0;
    double prev_spread = prev_ask_px - prev_bid_px;
    
    // Price crossed to ask side
    if (prev_last_price <= prev_ask_px && last_price > prev_ask_px) {
        return InferenceMethod::SPREAD_CROSS_UP;
    }
    
    // Price crossed to bid side
    if (prev_last_price >= prev_bid_px && last_price < prev_bid_px) {
        return InferenceMethod::SPREAD_CROSS_DOWN;
    }
    
    // Significant price movement
    if (price_change > tick_size * 0.5) {
        return InferenceMethod::PRICE_UP;
    }
    if (price_change < -tick_size * 0.5) {
        return InferenceMethod::PRICE_DOWN;
    }
    
    // Bid price moved down (new bid level)
    if (bid_px < prev_bid_px - tick_size * 0.5) {
        return InferenceMethod::BID_DEPLETION;
    }
    
    // Ask price moved up (new ask level)
    if (ask_px > prev_ask_px + tick_size * 0.5) {
        return InferenceMethod::ASK_DEPLETION;
    }
    
    return InferenceMethod::UNKNOWN;
}

//------------------------------------------------------------------------------
// Confidence Calculation
//------------------------------------------------------------------------------

double TickTransactionInferer::calculateConfidence(
    InferenceMethod method,
    double bid_vol_change, double ask_vol_change,
    double price_change
)
{
    double confidence = 0.0;
    double tick_size = _config.tick_size;
    
    switch (method) {
        case InferenceMethod::ASK_DEPLETION:
            // Higher volume depletion = higher confidence
            if (ask_vol_change > 0) {
                confidence = std::min(1.0, ask_vol_change / 100.0);
            }
            break;
            
        case InferenceMethod::BID_DEPLETION:
            if (bid_vol_change > 0) {
                confidence = std::min(1.0, bid_vol_change / 100.0);
            }
            break;
            
        case InferenceMethod::PRICE_UP:
        case InferenceMethod::PRICE_DOWN:
            // Larger price move = higher confidence
            {
                double abs_move = std::abs(price_change) / tick_size;
                confidence = std::min(1.0, abs_move / 2.0);
            }
            break;
            
        case InferenceMethod::SPREAD_CROSS_UP:
        case InferenceMethod::SPREAD_CROSS_DOWN:
            // Spread cross is a strong signal
            confidence = 0.9;
            break;
            
        case InferenceMethod::VOLUME_SURGE:
            confidence = 0.6;
            break;
            
        default:
            confidence = 0.0;
            break;
    }
    
    // Boost confidence if both volume and price signals agree
    if (method == InferenceMethod::ASK_DEPLETION && price_change > 0) {
        confidence = std::min(1.0, confidence + 0.2);
    }
    if (method == InferenceMethod::BID_DEPLETION && price_change < 0) {
        confidence = std::min(1.0, confidence + 0.2);
    }
    
    return confidence;
}

//------------------------------------------------------------------------------
// Volume Calculation
//------------------------------------------------------------------------------

double TickTransactionInferer::calculateInferredVolume(
    double bid_vol, double prev_bid_vol,
    double ask_vol, double prev_ask_vol,
    double last_vol
)
{
    // Calculate volume depleted from each side
    double bid_depletion = std::max(0.0, prev_bid_vol - bid_vol);
    double ask_depletion = std::max(0.0, prev_ask_vol - ask_vol);
    
    // Use the larger depletion as the inferred volume
    double inferred = std::max(bid_depletion, ask_depletion);
    
    // If both sides have depletion, take the sum (crossing trades)
    if (bid_depletion > 0 && ask_depletion > 0) {
        inferred = bid_depletion + ask_depletion;
    }
    
    // If we have total volume change, use that as an upper bound
    // Note: This requires maintaining prev_total_vol
    // For now, we use the inferred value directly
    
    return inferred;
}

//------------------------------------------------------------------------------
// Accumulation Update
//------------------------------------------------------------------------------

void TickTransactionInferer::updateAccumulation(const InferredTransaction& trans)
{
    // Prune old records first
    pruneHistory(trans.timestamp);
    
    // Calculate signed volume
    double signed_vol = trans.volume * trans.confidence;
    if (trans.is_sell_initiated) {
        signed_vol = -signed_vol;
    } else if (!trans.is_buy_initiated) {
        // No direction, skip
        return;
    }
    
    // Record to history
    InferenceRecord record;
    record.signed_volume = signed_vol;
    record.confidence = trans.confidence;
    record.is_large = trans.volume >= _config.large_trade_threshold;
    record.timestamp = trans.timestamp;
    _inference_history.push(record);
    
    // Update accumulated stats
    if (trans.is_buy_initiated) {
        _accumulated.buy_volume += trans.volume * trans.confidence;
    } else {
        _accumulated.sell_volume += trans.volume * trans.confidence;
    }
    _accumulated.net_flow = _accumulated.buy_volume - _accumulated.sell_volume;
    _accumulated.tick_count++;
    
    // Update large trade tracking
    if (record.is_large) {
        _large_volume += trans.volume;
    }
    _total_volume += trans.volume;
}

//------------------------------------------------------------------------------
// History Pruning
//------------------------------------------------------------------------------

void TickTransactionInferer::pruneHistory(uint64_t current_time)
{
    uint64_t window_start = current_time - _config.imbalance_window_ms;
    
    // Remove old records
    while (!_inference_history.empty()) {
        const auto& oldest = _inference_history.front();
        if (oldest.timestamp < window_start) {
            // Subtract from accumulated stats
            double vol = std::abs(oldest.signed_volume);
            if (oldest.signed_volume > 0) {
                _accumulated.buy_volume -= vol;
            } else {
                _accumulated.sell_volume -= vol;
            }
            
            if (oldest.is_large) {
                _large_volume -= vol;
            }
            _total_volume -= vol;
            
            _inference_history.pop();
        } else {
            break;
        }
    }
    
    // Update imbalance ratio
    double total = _accumulated.buy_volume + _accumulated.sell_volume;
    if (total > 0) {
        _accumulated.imbalance_ratio = _accumulated.net_flow / total;
    } else {
        _accumulated.imbalance_ratio = 0;
    }
    
    _accumulated.window_start = window_start;
}

//------------------------------------------------------------------------------
// Analysis Results
//------------------------------------------------------------------------------

InferredTradeImbalance TickTransactionInferer::getInferredImbalance() const
{
    InferredTradeImbalance result;
    
    result.net_flow = _accumulated.net_flow;
    result.imbalance_ratio = _accumulated.imbalance_ratio;
    
    if (_total_volume > 0) {
        result.large_trade_ratio = _large_volume / _total_volume;
    }
    
    // Calculate average confidence
    if (!_inference_history.empty()) {
        double total_conf = 0;
        size_t count = 0;
        for (size_t i = 0; i < _inference_history.size(); ++i) {
            total_conf += _inference_history[i].confidence;
            count++;
        }
        result.confidence = total_conf / count;
    }
    
    result.timestamp = _prev_state.timestamp;
    
    return result;
}

AccumulatedFlowStats TickTransactionInferer::getFlowStats() const
{
    return _accumulated;
}

double TickTransactionInferer::getImbalanceRatio() const
{
    return _accumulated.imbalance_ratio;
}

} // namespace futu
