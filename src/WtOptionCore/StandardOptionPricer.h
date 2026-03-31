/*!
 * \file StandardOptionPricer.h
 * \project	WonderTrader
 *
 * \brief Standard Black-Scholes/Black76 Pricer with ExpiryInfo Caching
 * 
 * Adapted from longbeach CommPricer pattern:
 * - ExpiryInfo caches per-expiry parameters (maturity, ATMForward, ATMVol, volCurve)
 * - initValuesCompute/computeValue/finalizeCompute lifecycle
 * - Avoids redundant per-option lookups
 */
#pragma once

#include "IOptionPricer.h"
#include "CurveFitter.h"
#include "Black76.h"
#include "BlackScholes.h"
#include "OptionGrid.h"
#include <map>

namespace wt_option {

class StandardOptionPricer : public IOptionPricer
{
public:
    StandardOptionPricer();
    virtual ~StandardOptionPricer();

    //=========================================================================
    // IOptionPricer Interface
    //=========================================================================

    virtual bool computeValues(OptionGrid* grid) override;
    virtual bool computeImpliedValues(OptionGrid* grid) override;

    virtual double getATMVol(uint32_t expiry) const override;
    virtual void setATMVol(uint32_t expiry, double vol) override;

    virtual double getATMForward(uint32_t expiry) const override;
    virtual double getMaturity(uint32_t expiry) const override;
    virtual double getVol(uint32_t expiry, double strike) const override;
    virtual IVolCurvePtr getVolCurve(uint32_t expiry) const override;

    virtual void setReprice(bool bReprice) override { m_bReprice = bReprice; }

    //=========================================================================
    // Lifecycle Methods (from longbeach CommPricer pattern)
    //=========================================================================

    virtual bool initValuesCompute(OptionGrid* grid) override;
    virtual void computeValue(OptionData* option) override;
    virtual void finalizeCompute(OptionGrid* grid) override;

    //=========================================================================
    // ExpiryInfo — per-expiry cache (from longbeach CommPricer::ExpiryInfo)
    //=========================================================================

    /**
     * Caches per-expiry parameters to avoid redundant lookups.
     * Built in initValuesCompute(), used in computeValue().
     *
     * Adapted from longbeach CommPricer::ExpiryInfo:
     *   - m_expiry, m_atmforward, m_atmvol, m_maturity
     *   - m_spVolCurve (fitted vol curve)
     *   - m_settleFrac (settlement fraction for commodity options)
     */
    struct ExpiryInfo {
        uint32_t expiry = 0;
        double atmForward = 0.0;       // Cached forward price
        double atmVol = 0.0;           // Cached ATM vol
        double maturity = 0.0;         // Time to expiry (years)
        double settleFrac = 1.0;       // Settlement fraction
        double riskFreeRate = 0.0;     // Risk-free rate
        double dividendYield = 0.0;    // Dividend yield
        double discount = 0.0;         // e^(-r*T)
        IVolCurvePtr volCurve;         // Fitted vol curve
        ExpiryData* expiryData = nullptr;  // Back-pointer for efficiency
        
        bool isValid() const { return maturity > 0.0001 && atmForward > 0; }
        
        /// Get vol for a given strike, using curve if available
        double getVolForStrike(double strike) const;
        
        /// Compute forward from underlying and carry
        void computeForward(double underlyingPrice);
    };

    const std::map<uint32_t, ExpiryInfo>& getExpiryInfoMap() const { return m_expiryInfo; }
    const ExpiryInfo* getExpiryInfo(uint32_t expiry) const;

private:
    void calculateTheoretical(OptionData* option, const ExpiryInfo& ei);
    void calculateImplied(OptionData* option, const ExpiryInfo& ei);

private:
    bool m_bReprice;
    bool m_useBlack76 = true;
    
    /// Per-expiry cache — rebuilt each computeValues() cycle
    std::map<uint32_t, ExpiryInfo> m_expiryInfo;
    
    /// Pointer to current grid (valid during compute cycle)
    OptionGrid* m_currentGrid = nullptr;
};

} // namespace wt_option
