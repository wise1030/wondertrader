/*!
 * \file CompositeOptionPricer.h
 * \brief Composite pricer for Market Making
 * 
 * Adapted from longbeach CompositeCommPricer/CompositeOptionPricer:
 * - computeValues_FAST: quick theo update on every tick (~50μs)
 * - computeValues_SLOW: full recalc with IV solve, curve refit (~5ms)
 * - Time-based dispatcher in computeValues()
 */
#pragma once

#include "IOptionPricer.h"
#include "OptionGrid.h"
#include "IAlphaSignal.h"
#include "UnderlyingTradingData.h"
#include "QuoteStatistics.h"
#include <map>
#include <string>
#include <chrono>

namespace wt_option {

struct ExpiryRiskConfig {
    bool enable = true;
    bool enableAutoClose = true;
    
    int32_t maxPosFut = 100;
    int32_t maxPosOpt = 500;
    int32_t maxOrderSize = 10;
    
    double deltaMin = 0.1;
    double deltaMax = 0.9;
    
    double spreadFut = 0.0002;
    double spreadVol = 0.005;
    double minSpread = 0.0005;
    double spreadMultiplier = 1.0; 
    
    std::string configName;
    
    bool quoteUnderlying = false;
    double minUnderlyingSpread = 0.0002;
    int32_t underlyingOrderSize = 0;
    
    // Risk Tolerances
    double riskTolDelta = 100.0;
    double riskTolVega = 1000.0;
    
    // Inventory Skewing
    double maxPositionDelta = 100.0;
    double riskShiftDeltaRatio = 1.0;
    double maxPositionVega = 500.0;
    double riskShiftVegaRatio = 1.0;
    
    // Trade Shock (Anti-Ping)
    int32_t shockTicks = 2;
};

class OptionRisk; // Forward declaration
using OptionRiskPtr = std::shared_ptr<OptionRisk>;

class WtOptionStrategy; // Forward declaration

class CompositeOptionPricer : public IOptionPricer {
public:
    CompositeOptionPricer();
    virtual ~CompositeOptionPricer();
    
    // IOptionPricer Implementation (Delegates to sub-pricer)
    virtual bool computeValues(OptionGrid* grid) override;
    virtual bool computeImpliedValues(OptionGrid* grid) override;
    
    virtual double getATMVol(uint32_t expiry) const override;
    virtual void setATMVol(uint32_t expiry, double vol) override;
    virtual double getATMForward(uint32_t expiry) const override;
    virtual double getMaturity(uint32_t expiry) const override;
    virtual double getVol(uint32_t expiry, double strike) const override;
    virtual IVolCurvePtr getVolCurve(uint32_t expiry) const override;
    virtual void setReprice(bool bReprice) override;

    //=========================================================================
    // FAST/SLOW Dual-Path (from longbeach CompositeCommPricer pattern)
    //=========================================================================

    /**
     * @brief FAST path — called on every tick
     * 
     * Only updates theoretical prices using cached vol/Greeks.
     * Skips: implied vol solve, curve fitting, forward recalc, Greeks decay.
     * Target latency: ~50μs
     */
    void computeValues_FAST(OptionGrid* grid);

    /**
     * @brief SLOW path — called periodically
     * 
     * Full recalculation: implied vol solve, curve refit, 
     * forward recalc from synthetic parity, Greeks decay.
     * Target latency: ~5ms
     */
    void computeValues_SLOW(OptionGrid* grid);

    /**
     * @brief Set slow compute interval in milliseconds
     */
    void setSlowComputePeriod(uint64_t periodMs) { m_slowComputePeriodMs = periodMs; }

    // Configuration
    void setTheoreticalPricer(IOptionPricerPtr pricer) { m_theoPricer = pricer; }
    void setOptionRisk(OptionRiskPtr risk) { m_risk = risk; }
    void setStrategy(WtOptionStrategy* strategy) { m_strategy = strategy; }
    void setExpiryConfig(uint32_t expiry, const ExpiryRiskConfig& config);
    
    // Signals
    void setPriceSignal(IAlphaSignalPtr signal) { m_priceSignal = signal; }
    void setVolSignal(IAlphaSignalPtr signal) { m_volSignal = signal; }
    
    // Data Updates
    void onTick(const char* code, wtp::WTSTickData* tick);
    
    // Market Making Logic
    void computeOurMarkets(OptionGrid* grid);
    
    QuoteStatistics& getQuoteStats() { return m_quoteStats; }
    const QuoteStatistics& getQuoteStats() const { return m_quoteStats; }
    
protected:
    void computeOurMarketsForOption(OptionData* option, const ExpiryData* expiryData, const ExpiryRiskConfig& config);
    void computeOurMarketsForUnderlying(UnderlyingTradingData* underlying, const ExpiryRiskConfig& config);
    
    // Logic Helpers
    void risk_adjustment(OptionData* option, const ExpiryRiskConfig& config);
    void alpha_adjustment(OptionData* option, const ExpiryRiskConfig& config);
    
private:
    IOptionPricerPtr m_theoPricer;
    OptionRiskPtr m_risk;
    WtOptionStrategy* m_strategy = nullptr;
    IAlphaSignalPtr m_priceSignal;
    IAlphaSignalPtr m_volSignal;
    QuoteStatistics m_quoteStats;
    std::map<uint32_t, ExpiryRiskConfig> m_expiryConfigs;
    bool m_bReprice;
    
    // FAST/SLOW timing (from longbeach pattern)
    using clock_t = std::chrono::steady_clock;
    using time_point_t = std::chrono::steady_clock::time_point;
    
    time_point_t m_lastSlowCompute;
    uint64_t m_slowComputePeriodMs = 100;  // Default 100ms between SLOW computes
    bool m_firstCompute = true;
};

using CompositeOptionPricerPtr = std::shared_ptr<CompositeOptionPricer>;

} // namespace wt_option
