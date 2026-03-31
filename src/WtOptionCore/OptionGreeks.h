/*!
 * \file OptionGreeks.h
 * \brief Option Greeks calculation and container
 * 
 * Migrated from longbeach/quantbox/strategy/optioncore/OptionGreeks.h
 */

#pragma once

#include "OptionTypes.h"
#include <memory>

namespace wt_option {

/**
 * @brief Container for option Greeks values
 */
class OptionGreeks {
public:
    OptionGreeks();
    
    void reset();
    
    OptionGreeks& accum(const OptionGreeks& g);
    OptionGreeks& accum(double m, const OptionGreeks& g);
    OptionGreeks& reduce(const OptionGreeks& g);
    OptionGreeks& reduce(double m, const OptionGreeks& g);
    OptionGreeks& apply(double m, const OptionGreeks& g);
    
    // Accessors
    double& delta() { return m_delta; }
    double delta() const { return m_delta; }
    
    double& gamma() { return m_gamma; }
    double gamma() const { return m_gamma; }
    
    double& vega() { return m_vega; }
    double vega() const { return m_vega; }
    
    double& vegaTW() { return m_vegaTW; }  // Time-weighted vega
    double vegaTW() const { return m_vegaTW; }
    
    double& theta() { return m_theta; }
    double theta() const { return m_theta; }
    
    double& rho() { return m_rho; }
    double rho() const { return m_rho; }
    
    double& vanna() { return m_vanna; }
    double vanna() const { return m_vanna; }
    
    double& volga() { return m_volga; }
    double volga() const { return m_volga; }
    
    // Operators
    OptionGreeks& operator-=(const OptionGreeks& rhs);
    OptionGreeks operator+(const OptionGreeks& rhs) const;
    OptionGreeks operator-(const OptionGreeks& rhs) const;
    OptionGreeks operator-() const;
    OptionGreeks operator*(double m) const;
    
private:
    double m_delta;
    double m_gamma;
    double m_vega;
    double m_vegaTW;
    double m_theta;
    double m_rho;
    double m_vanna;
    double m_volga;
};

using OptionGreeksPtr = std::shared_ptr<OptionGreeks>;

/**
 * @brief Container for calculated option values
 */
struct OptionValues {
    // Theoretical values
    double theoreticalPrice;
    double impliedVol;
    double underlyingPrice;
    double timeToExpiry;
    OptionGreeks greeks;
    
    // Market Making Targets (Quoting Logic)
    double ourBid;
    double ourAsk;
    int32_t ourBidSize;
    int32_t ourAskSize;
    double volBias;        // Alpha/Risk adjustment to volatility
    double priceBias;      // Direct price adjustment
    
    // Status
    bool isPriced;
    uint64_t updateTime;
    uint64_t quoteChangeTime;
    
    OptionValues() { reset(); }
    
    void reset() {
        theoreticalPrice = 0;
        impliedVol = 0;
        underlyingPrice = 0;
        timeToExpiry = 0;
        greeks.reset();
        
        ourBid = 0;
        ourAsk = 0;
        ourBidSize = 0;
        ourAskSize = 0;
        volBias = 0;
        priceBias = 0;
        
        isPriced = false;
        updateTime = 0;
        quoteChangeTime = 0;
    }
};

} // namespace wt_option
