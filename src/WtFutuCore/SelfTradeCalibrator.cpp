/*!
 * \file SelfTradeCalibrator.cpp
 * \brief Implementation of Self-Trade Calibration (Optimized with RingBuffer)
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
    _contract_states.clear();
}

void SelfTradeCalibrator::resetContract(const std::string& code)
{
    _contract_states.erase(code);
}

void SelfTradeCalibrator::resetCalibration(const std::string& code)
{
    auto it = _contract_states.find(code);
    if (it != _contract_states.end())
    {
        // Clear fill history and reset cache
        it->second.fill_history.clear();
        it->second.cached_result = CalibrationResult();
        it->second.cache_dirty = true;
    }
}

void SelfTradeCalibrator::decayCalibration(const std::string& code, uint64_t current_time, uint64_t decay_window_ms)
{
    auto it = _contract_states.find(code);
    if (it == _contract_states.end())
        return;
    
    auto& state = it->second;
    
    // If no fills, nothing to decay
    if (state.fill_history.empty())
        return;
    
    // 时间戳格式: date * 1000000 + time * 100 + secs
    // 其中 secs 格式为 SSMMM (秒*1000 + 毫秒)
    // 对于同一天内的比较，可以直接比较时间戳差值
    // decay_window_ms 是毫秒，需要转换
    
    // 计算截止时间
    // 注意：这个简单的减法只在同一天内有效
    // 对于跨天的情况，fill 会被自然淘汰（因为 RingBuffer 容量有限）
    uint64_t cutoff_time = 0;
    if (current_time > decay_window_ms)
    {
        cutoff_time = current_time - decay_window_ms;
    }
    
    // 创建新的缓冲区，只保留最近的 fills
    RingBuffer<SelfFillRecord, 128> recent_fills;
    uint32_t removed_count = 0;
    
    for (const auto& fill : state.fill_history)
    {
        if (fill.fill_time >= cutoff_time)
        {
            recent_fills.push(fill);
        }
        else
        {
            removed_count++;
        }
    }
    
    // 如果有 fills 被移除，更新状态
    if (removed_count > 0)
    {
        state.fill_history = recent_fills;
        state.cache_dirty = true;
        
        // 如果所有 fills 都被移除，重置缓存
        if (state.fill_history.empty())
        {
            state.cached_result = CalibrationResult();
            state.cache_dirty = false;
        }
    }
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
    
    // Get or create contract state
    auto& state = _contract_states[code];
    
    // Add to history (RingBuffer auto-manages size)
    state.fill_history.push(record);
    
    // Mark cache as dirty
    state.cache_dirty = true;
}

void SelfTradeCalibrator::onTick(const std::string& code, double mid_price, uint64_t timestamp)
{
    // Get or create contract state
    auto& state = _contract_states[code];
    
    // Update current market state
    state.mid_price = mid_price;
    state.timestamp = timestamp;
    
    // Analyze existing fills for toxicity
    analyzeFills(code);
    
    // Prune old fills (time-based)
    pruneHistory(code, timestamp);
}

//------------------------------------------------------------------------------
// Analysis
//------------------------------------------------------------------------------

void SelfTradeCalibrator::analyzeFills(const std::string& code) const
{
    auto it = _contract_states.find(code);
    if (it == _contract_states.end()) {
        return;
    }
    
    auto& state = it->second;
    
    // Skip if cache is clean
    if (!state.cache_dirty && state.cached_result.sample_size > 0) {
        return;
    }
    
    // Check if we have fills
    if (state.fill_history.empty()) {
        state.cached_result = CalibrationResult();
        state.cache_dirty = false;
        return;
    }
    
    double current_mid = state.mid_price;
    uint64_t current_time = state.timestamp;
    
    // Calculate statistics by iterating through RingBuffer
    CalibrationResult result;
    double total_adverse_move = 0;
    uint32_t adverse_count = 0;
    uint32_t buy_count = 0;
    uint32_t sell_count = 0;
    uint32_t buy_adverse = 0;
    uint32_t sell_adverse = 0;
    uint32_t total_count = 0;
    
    // Iterate through RingBuffer using range-based for loop
    for (const auto& fill : state.fill_history) {
        // Check if enough time has passed for analysis
        uint64_t elapsed = current_time - fill.fill_time;
        if (elapsed < _config.toxicity_window_ms / 2) {
            continue;  // Not enough time for meaningful analysis
        }
        
        total_count++;
        
        // Check for adverse selection
        bool is_adverse = checkAdverse(fill, current_mid);
        
        if (is_adverse) {
            adverse_count++;
            double move = current_mid - fill.mid_at_fill;
            double move_ticks = move / _config.tick_size;
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
    
    result.sample_size = static_cast<double>(total_count);
    
    // Calculate adverse ratios
    if (buy_count > 0) {
        result.buy_adverse_ratio = static_cast<double>(buy_adverse) / buy_count;
    }
    if (sell_count > 0) {
        result.sell_adverse_ratio = static_cast<double>(sell_adverse) / sell_count;
    }
    
    // Calculate overall toxicity
    if (total_count > 0) {
        result.toxicity_level = static_cast<double>(adverse_count) / total_count;
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
    state.cached_result = result;
    state.cache_dirty = false;
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
    auto it = _contract_states.find(code);
    if (it == _contract_states.end()) {
        return;
    }
    
    auto& state = it->second;
    uint64_t expiry = current_time - (_config.toxicity_window_ms * 10);  // Keep longer for analysis
    
    // Note: RingBuffer doesn't support selective removal from front
    // We rely on its fixed-size nature to auto-prune oldest entries
    // For time-based pruning, we would need a different approach
    // For now, the RingBuffer's fixed size (128) serves as the pruning mechanism
}

//------------------------------------------------------------------------------
// Results
//------------------------------------------------------------------------------

CalibrationResult SelfTradeCalibrator::getCalibration(const std::string& code) const
{
    auto it = _contract_states.find(code);
    if (it == _contract_states.end()) {
        return CalibrationResult();
    }
    
    auto& state = it->second;
    
    // Re-analyze if cache is dirty or missing
    if (state.cache_dirty || state.cached_result.sample_size == 0) {
        analyzeFills(code);
    }
    
    return state.cached_result;
}

SelfTradeToxicityMetrics SelfTradeCalibrator::getToxicityMetrics(const std::string& code) const
{
    SelfTradeToxicityMetrics metrics;
    
    auto calib = getCalibration(code);
    metrics.realized_toxicity = calib.toxicity_level;
    metrics.toxicity_score = calib.toxicity_level;
    metrics.is_toxic = calib.high_toxicity;
    metrics.toxic_side = -calib.recommended_side;  // Invert: if recommend buy, avoid sell
    
    auto it = _contract_states.find(code);
    if (it != _contract_states.end()) {
        auto& state = it->second;
        metrics.total_fills = static_cast<uint32_t>(state.fill_history.size());
        
        uint32_t adverse = 0;
        double total_move = 0;
        
        for (const auto& fill : state.fill_history) {
            if (checkAdverse(fill, state.mid_price)) {
                adverse++;
                double move = state.mid_price - fill.mid_at_fill;
                total_move += std::abs(move / _config.tick_size);
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

const RingBuffer<SelfFillRecord, 128>* SelfTradeCalibrator::getFillHistory(const std::string& code) const
{
    auto it = _contract_states.find(code);
    if (it != _contract_states.end()) {
        return &it->second.fill_history;
    }
    return nullptr;
}

uint32_t SelfTradeCalibrator::getSampleCount(const std::string& code) const
{
    auto it = _contract_states.find(code);
    if (it != _contract_states.end()) {
        return static_cast<uint32_t>(it->second.fill_history.size());
    }
    return 0;
}

} // namespace futu