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
        uint32_t window;
        double weight;
        uint32_t lag_ms;
        
        Config() 
            : window(50)
            , weight(0.3)
            , lag_ms(50)
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
        return SignalType::CUSTOM; 
    }
    
    void update(const MarketDataContext& book) override
    {
        // Lead-lag requires external updates from lead contracts
        // This method just stores current contract's mid
        _current_mid = book.getMidPrice();
        _current_timestamp = book.getTimestamp();
    }
    
    const SignalResult& result() const override { return _result; }
    
    bool enabled() const override { return _enabled; }
    void setEnabled(bool e) override { _enabled = e; }
    
    void reset() override
    {
        _lead_contracts.clear();
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
            info.mid_change = (mid - info.last_mid) / info.last_mid;
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
            total_signal += info.mid_change * weight * 100.0;  // Scale for range
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
