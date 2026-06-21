/*!
 * \file LeadLagSignalSource.h
 * \brief Lead-Lag Signal Source
 * 
 * Detects predictive signals from lead contracts:
 *   - Cross-contract correlation
 *   - Time-lagged price movements
 * 
 * Used for pairs trading and cross-market arbitrage.
 */
#pragma once

#include "ISignalSource.h"
#include "MarketDataContext.h"
#include "../Share/RingBuffer.hpp"
#include <cmath>
#include <algorithm>
#include <unordered_map>

namespace futu {

//==============================================================================
// Lead Contract Info
//==============================================================================

struct LeadContractInfo
{
    std::string code;
    double correlation;     // Correlation with current contract
    double last_mid;
    double mid_change;
    uint64_t last_timestamp;
    
    LeadContractInfo() : correlation(0), last_mid(0), mid_change(0), last_timestamp(0) {}
};

//==============================================================================
// Lead-Lag Signal Source
//==============================================================================

class LeadLagSignalSource : public ISignalSource
{
public:
    struct Config
    {
        uint32_t window;       ///< Mid-change smoothing window size
        double weight;
        uint32_t lag_ms;
        double scale_factor;   ///< Signal scale factor (tanh input multiplier)
                               ///< bps scaling. EC mid~3700, 1 tick(0.5)=1.35bps
                               ///< scale=3000: tanh(1.35×0.3)=0.37 (moderate, 不饱和)
                               ///< scale=10000: tanh(1.35×0.3)=0.87 (偏饱和)
        
        Config() 
            : window(50)
            , weight(0.3)
            , lag_ms(50)
            , scale_factor(3000.0)
        {}
    };
    
    explicit LeadLagSignalSource(const Config& cfg = Config())
        : _cfg(cfg)
        , _enabled(true)
        , _cumulative_signal(0)
    {
    }
    
    //==========================================================================
    // ISignalSource Interface
    //==========================================================================
    
    const std::string& name() const override 
    { 
        static std::string n = "LeadLag"; 
        return n; 
    }
    
    SignalType type() const override 
    { 
        return SignalType::LEAD_LAG; 
    }
    
    void update(const MarketDataContext& book) override
    {
        // Lead-lag requires external updates from lead contracts
        // This method just stores current contract's mid
        _current_mid = book.getMidPrice();
        _current_timestamp = book.getTimestamp();
    }
    
    const SignalResult& result() const override { return _result; }
    
    double getAlphaValue() const override { return _result.alpha; }
    
    bool enabled() const override { return _enabled; }
    void setEnabled(bool e) override { _enabled = e; }
    
    void reset() override
    {
        _lead_contracts.clear();
        _mid_change_histories.clear();
        _cumulative_signal = 0;
        _result = AlphaSignalResult();
    }
    
    /// Set configuration
    void setConfig(const Config& cfg) { _cfg = cfg; }
    
    //==========================================================================
    // Lead Contract Management
    //==========================================================================
    
    /// Add a lead contract
    void addLeadContract(const std::string& code, double correlation = 1.0)
    {
        _lead_contracts[code] = LeadContractInfo();
        _lead_contracts[code].code = code;
        _lead_contracts[code].correlation = correlation;
    }
    
    /// Update lead contract price
    void updateLeadContract(const std::string& code, double mid, uint64_t timestamp)
    {
        auto it = _lead_contracts.find(code);
        if (it == _lead_contracts.end()) return;
        
        LeadContractInfo& info = it->second;
        
        if (info.last_mid > 0)
        {
            double mid_change = (mid - info.last_mid) / info.last_mid;
            
            // 新增RingBuffer存储mid_change历史，计算窗口内加权平均
            // 原代码仅存储单次mid_change，window/lag_ms配置未使用。
            // 单次mid_change噪声大，容易被单笔大单或瞬时波动误导。
            // 现在用RingBuffer存储最近window次mid_change，计算加权平均：
            //   越新的数据权重越大(线性衰减)，越旧的数据权重越小。
            auto& history = _mid_change_histories[code];
            history.push(mid_change);
            
            // Compute weighted average of mid_change history
            double weighted_sum = 0;
            double weight_sum = 0;
            size_t n = history.size();
            for (size_t i = 0; i < n; ++i)
            {
                // Linear decay: newest gets weight n, oldest gets weight 1
                double w = static_cast<double>(i + 1);
                weighted_sum += history[i] * w;
                weight_sum += w;
            }
            info.mid_change = (weight_sum > 0) ? (weighted_sum / weight_sum) : 0.0;
        }
        
        info.last_mid = mid;
        info.last_timestamp = timestamp;
        
        // Recalculate signal
        calculateSignal();
    }
    
    //==========================================================================
    // Signal Access
    //==========================================================================
    
    /// Get lead-lag signal [-1, 1]
    double getSignal() const { return _result.alpha; }
    
    /// Get lead-lag component (for alpha aggregation)
    double getComponent() const { return _result.lead_lag_component; }
    
private:
    Config _cfg;
    bool _enabled;
    AlphaSignalResult _result;
    
    std::unordered_map<std::string, LeadContractInfo> _lead_contracts;
    
    // RingBuffer存储mid_change历史，用于窗口内加权平均
    using MidChangeHistory = RingBuffer<double, 64>;  // power of 2 for optimal performance
    std::unordered_map<std::string, MidChangeHistory> _mid_change_histories;
    
    double _cumulative_signal;
    double _current_mid;
    uint64_t _current_timestamp;
    
    void calculateSignal()
    {
        double total_signal = 0;
        double total_weight = 0;
        
        for (const auto& [code, info] : _lead_contracts)
        {
            double weight = std::abs(info.correlation);
            // Scale mid_change (ratio) by configurable factor
            // Old: × 100 (too weak for high-priced contracts like EC)
            // New: × scale_factor (default 10000 = bps)
            total_signal += info.mid_change * weight * _cfg.scale_factor;
            total_weight += weight;
        }
        
        if (total_weight > 0)
        {
            total_signal /= total_weight;
        }
        
        // Normalize to [-1, 1]
        _result.lead_lag_component = std::tanh(total_signal);
        _result.alpha = _result.lead_lag_component;  // For standalone use
        _result.valid = true;
        _result.timestamp = _current_timestamp;
    }
};

} // namespace futu
