/*!
 * \file GammaScalpOptionPricer.h
 * \brief Gamma Scalping Pricer with dynamic hedging
 * 
 * Supports FAST/SLOW path execution logic for theoretical pricing,
 * option selection based on Gamma/Theta ratio,
 * and automated Delta hedging based on Whalley-Wilmott optimal hedging band.
 */
#pragma once

#include "IOptionPricer.h"
#include "OptionGrid.h"
#include "UnderlyingTradingData.h"
#include "QuoteStatistics.h"
#include <map>
#include <string>
#include <chrono>

// Forward declarations for WonderTrader infrastructure
namespace wtp {
    class WTSTickData;
}

namespace wt_option {

class OptionRisk;
using OptionRiskPtr = std::shared_ptr<OptionRisk>;

class WtOptionStrategy;

struct GammaScalpConfig {
    bool enable = true;
    bool enableAutoClose = true;
    
    // Position sizing limits
    int32_t maxPosFut = 100;
    int32_t maxPosOpt = 500;
    int32_t maxOrderSize = 10;
    
    // Deep ITM/OTM filter (only allow closing)
    double deltaMin = 0.1;
    double deltaMax = 0.9;
    
    // Option pricing spreads
    double spreadFut = 0.0002;
    double spreadVol = 0.005;
    double minSpread = 0.0005;
    double spreadMultiplier = 1.0; 
    
    std::string configName;
    
    // Underlying quoting
    bool quoteUnderlying = false;
    double minUnderlyingSpread = 0.0002;
    int32_t underlyingOrderSize = 1;

    // Gamma scalping strategy specific
    double targetGamma = 1000.0;     // Target gamma for the portfolio
    
    // Hedging band logic
    double hedgeThresholdRisk = 10.0; // The threshold beyond which we hedge
    double transactionCost = 0.0005;  // Slippage + fee assumption for the underlying
    double impliedVolatility = 0.20;  // Baseline IV for expected gamma profit

    // Trade Shock (Anti-Ping)
    int32_t shockTicks = 2;
};

class GammaScalpOptionPricer : public IOptionPricer {
public:
    GammaScalpOptionPricer();
    virtual ~GammaScalpOptionPricer();
    
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

    // FAST/SLOW Dual-Path
    void computeValues_FAST(OptionGrid* grid);
    void computeValues_SLOW(OptionGrid* grid);
    void setSlowComputePeriod(uint64_t periodMs) { m_slowComputePeriodMs = periodMs; }

    // Configuration
    void setTheoreticalPricer(IOptionPricerPtr pricer) { m_theoPricer = pricer; }
    void setOptionRisk(OptionRiskPtr risk) { m_risk = risk; }
    void setStrategy(WtOptionStrategy* strategy) { m_strategy = strategy; }
    void setExpiryConfig(uint32_t expiry, const GammaScalpConfig& config);
    
    // Data Updates
    void onTick(const char* code, wtp::WTSTickData* tick);
    
    // Strategy Logic
    void computeOurMarkets(OptionGrid* grid);
    void evaluateDynamicHedging(OptionGrid* grid); // Trigger hedging
    
    QuoteStatistics& getQuoteStats() { return m_quoteStats; }
    const QuoteStatistics& getQuoteStats() const { return m_quoteStats; }
    
protected:
    void computeOurMarketsForOption(OptionData* option, const ExpiryData* expiryData, const GammaScalpConfig& config);
    void computeOurMarketsForUnderlying(UnderlyingTradingData* underlying, const GammaScalpConfig& config);
    
    void gamma_theta_adjustment(OptionData* option, const GammaScalpConfig& config);
    
private:
    IOptionPricerPtr m_theoPricer;
    OptionRiskPtr m_risk;
    WtOptionStrategy* m_strategy = nullptr;
    QuoteStatistics m_quoteStats;
    std::map<uint32_t, GammaScalpConfig> m_expiryConfigs;
    bool m_bReprice;
    
    // FAST/SLOW timing
    using clock_t = std::chrono::steady_clock;
    using time_point_t = std::chrono::steady_clock::time_point;
    
    time_point_t m_lastSlowCompute;
    uint64_t m_slowComputePeriodMs = 100;
    bool m_firstCompute = true;

    uint64_t m_lastHedgeCheckMs = 0;
};

using GammaScalpOptionPricerPtr = std::shared_ptr<GammaScalpOptionPricer>;

} // namespace wt_option
