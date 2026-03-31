/*!
 * \file SelfTradeCalibrator.cpp
 * \brief Implementation of Self-Trade Calibration
 */
#include "SelfTradeCalibrator.h"
#include <cmath>
#include <algorithm>

namespace futu {

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------

SelfTradeCalibrator::SelfTradeCalibrator()
{
}

//------------------------------------------------------------------------------
// Reset
//------------------------------------------------------------------------------

void SelfTradeCalibrator::reset()
{
    _fill_history.clear();
    _current_market.clear();
    _cached_calibration.clear();
    _cache_dirty.clear();
}

void SelfTradeCalibrator::resetContract(const std::string& code)
{
    _fill_history.erase(code);
    _current_market.erase(code);
    _cached_calibration.erase(code);
    _cache_dirty.erase(code);
}

//------------------------------------------------------------------------------
// Data Input
//------------------------------------------------------------------------------

void SelfTradeCalibrator::recordFill(
    const std::string& code,
    double price, double qty, bool is_buy,
    double mid_price, double spread,
    uint64_t timestamp
)
{
    SelfFillRecord record;
    record.code = code;
    record.fill_time = timestamp;
    record.fill_price = price;
    record.fill_qty = qty;
    record.is_buy = is_buy;
    record.mid_at_fill = mid_price;
    record.spread_at_fill = spread;
    record.analysis_time = timestamp;
    
    // Add to history
    auto& history = _fill_history[code];
    history.push_back(record);
    
    // Limit history size
    while (history.size() > _config.lookback_trades) {
        history.pop_front();
    }
    
    // Mark cache as dirty
    _cache_dirty[code] = true;
}

void SelfTradeCalibrator::onTick(const std::string& code, double mid_price, uint64_t timestamp)
{
    // Update current market state
    auto& state = _current_market[code];
    state.mid_price = mid_price;
    state.timestamp = timestamp;
    
    // Analyze existing fills for toxicity
    analyzeFills(code);
    
    // Prune old fills
    pruneHistory(code, timestamp);
}

//------------------------------------------------------------------------------
// Analysis
//------------------------------------------------------------------------------

void SelfTradeCalibrator::analyzeFills(const std::string& code) const
{
    auto it = _fill_history.find(code);
    if (it == _fill_history.end() || it->second.empty()) {
        return;
    }
    
    auto market_it = _current_market.find(code);
    if (market_it == _current_market.end()) {
        return;
    }
    
    const auto& history = it->second;
    double current_mid = market_it->second.mid_price;
    uint64_t current_time = market_it->second.timestamp;
    
    // Calculate statistics
    CalibrationResult result;
    double total_adverse_move = 0;
    uint32_t adverse_count = 0;
    uint32_t buy_count = 0;
    uint32_t sell_count = 0;
    uint32_t buy_adverse = 0;
    uint32_t sell_adverse = 0;
    
    for (const auto& fill : history) {
        // Check if enough time has passed for analysis
        uint64_t elapsed = current_time - fill.fill_time;
        if (elapsed < _config.toxicity_window_ms / 2) {
            continue;  // Not enough time for meaningful analysis
        }
        
        // Calculate price move
        double move = current_mid - fill.mid_at_fill;
        double move_ticks = move / _config.tick_size;
        
        // Check for adverse selection
        bool is_adverse = checkAdverse(fill, current_mid);
        
        if (is_adverse) {
            adverse_count++;
            total_adverse_move += std::abs(move_ticks);
            
            if (fill.is_buy) {
                buy_adverse++;
            } else {
                sell_adverse++;
            }
        }
        
        if (fill.is_buy) {
            buy_count++;
        } else {
            sell_count++;
        }
    }
    
    result.sample_size = static_cast<double>(history.size());
    
    // Calculate adverse ratios
    if (buy_count > 0) {
        result.buy_adverse_ratio = static_cast<double>(buy_adverse) / buy_count;
    }
    if (sell_count > 0) {
        result.sell_adverse_ratio = static_cast<double>(sell_adverse) / sell_count;
    }
    
    // Calculate overall toxicity
    uint32_t total_count = buy_count + sell_count;
    if (total_count > 0) {
        result.toxicity_level = static_cast<double>(adverse_count) / total_count;
        result.realized_toxicity = result.toxicity_level;
    }
    
    // Calculate direction bias
    // Positive = we're getting adverse on buys (market moving against us after buying)
    // Negative = we're getting adverse on sells (market moving against us after selling)
    result.direction_bias = result.buy_adverse_ratio - result.sell_adverse_ratio;
    
    // Determine high toxicity
    result.high_toxicity = result.toxicity_level > _config.adverse_threshold;
    
    // Determine recommended side
    if (result.buy_adverse_ratio > _config.adverse_threshold && 
        result.sell_adverse_ratio <= _config.adverse_threshold) {
        // Buys are toxic, recommend sell side
        result.recommended_side = -1;
    } else if (result.sell_adverse_ratio > _config.adverse_threshold && 
               result.buy_adverse_ratio <= _config.adverse_threshold) {
        // Sells are toxic, recommend buy side
        result.recommended_side = 1;
    } else {
        result.recommended_side = 0;
    }
    
    // Calculate confidence
    if (result.sample_size >= _config.min_samples) {
        result.confidence = std::min(1.0, result.sample_size / (_config.min_samples * 2.0));
    } else {
        result.confidence = result.sample_size / _config.min_samples;
    }
    
    // Cache result
    _cached_calibration[code] = result;
    _cache_dirty[code] = false;
}

bool SelfTradeCalibrator::checkAdverse(const SelfFillRecord& fill, double current_mid) const
{
    double move = current_mid - fill.mid_at_fill;
    double move_ticks = move / _config.tick_size;
    
    // For buys: adverse if price went down after we bought
    // For sells: adverse if price went up after we sold
    if (fill.is_buy) {
        return move_ticks < -_config.move_threshold_ticks;
    } else {
        return move_ticks > _config.move_threshold_ticks;
    }
}

double SelfTradeCalibrator::calculateRealizedToxicity(const SelfFillRecord& fill, double current_mid) const
{
    double move = current_mid - fill.mid_at_fill;
    double move_ticks = move / _config.tick_size;
    
    // For buys: toxicity is proportional to negative move
    // For sells: toxicity is proportional to positive move
    if (fill.is_buy) {
        return std::max(0.0, -move_ticks / 5.0);  // Normalize to ~[0, 1]
    } else {
        return std::max(0.0, move_ticks / 5.0);
    }
}

void SelfTradeCalibrator::pruneHistory(const std::string& code, uint64_t current_time)
{
    auto it = _fill_history.find(code);
    if (it == _fill_history.end()) {
        return;
    }
    
    auto& history = it->second;
    uint64_t expiry = current_time - (_config.toxicity_window_ms * 10);  // Keep longer for analysis
    
    while (!history.empty() && history.front().fill_time < expiry) {
        history.pop_front();
    }
}

//------------------------------------------------------------------------------
// Results
//------------------------------------------------------------------------------

CalibrationResult SelfTradeCalibrator::getCalibration(const std::string& code) const
{
    auto cache_it = _cached_calibration.find(code);
    auto dirty_it = _cache_dirty.find(code);
    
    if (cache_it != _cached_calibration.end() && 
        dirty_it != _cache_dirty.end() && !dirty_it->second) {
        return cache_it->second;
    }
    
    // Re-analyze if cache is dirty or missing
    analyzeFills(code);
    
    cache_it = _cached_calibration.find(code);
    if (cache_it != _cached_calibration.end()) {
        return cache_it->second;
    }
    
    return CalibrationResult();
}

SelfTradeToxicityMetrics SelfTradeCalibrator::getToxicityMetrics(const std::string& code) const
{
    SelfTradeToxicityMetrics metrics;
    
    auto calib = getCalibration(code);
    metrics.realized_toxicity = calib.toxicity_level;
    metrics.toxicity_score = calib.toxicity_level;
    metrics.is_toxic = calib.high_toxicity;
    metrics.toxic_side = -calib.recommended_side;  // Invert: if recommend buy, avoid sell
    
    auto it = _fill_history.find(code);
    if (it != _fill_history.end()) {
        metrics.total_fills = static_cast<uint32_t>(it->second.size());
        
        uint32_t adverse = 0;
        double total_move = 0;
        
        for (const auto& fill : it->second) {
            auto market_it = _current_market.find(code);
            if (market_it != _current_market.end()) {
                if (checkAdverse(fill, market_it->second.mid_price)) {
                    adverse++;
                    double move = market_it->second.mid_price - fill.mid_at_fill;
                    total_move += std::abs(move / _config.tick_size);
                }
            }
        }
        
        metrics.adverse_fills = adverse;
        if (adverse > 0) {
            metrics.avg_adverse_move = total_move / adverse;
        }
    }
    
    return metrics;
}

double SelfTradeCalibrator::getToxicityScore(const std::string& code) const
{
    return getCalibration(code).toxicity_level;
}

bool SelfTradeCalibrator::isHighToxicity(const std::string& code) const
{
    return getCalibration(code).high_toxicity;
}

//------------------------------------------------------------------------------
// Statistics
//------------------------------------------------------------------------------

const std::deque<SelfFillRecord>* SelfTradeCalibrator::getFillHistory(const std::string& code) const
{
    auto it = _fill_history.find(code);
    if (it != _fill_history.end()) {
        return &it->second;
    }
    return nullptr;
}

uint32_t SelfTradeCalibrator::getSampleCount(const std::string& code) const
{
    auto it = _fill_history.find(code);
    if (it != _fill_history.end()) {
        return static_cast<uint32_t>(it->second.size());
    }
    return 0;
}

} // namespace futu
