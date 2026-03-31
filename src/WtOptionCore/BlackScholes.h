/*!
 * \file BlackScholes.h
 * \brief Black-Scholes option pricing model
 * 
 * Migrated from longbeach/quantbox/strategy/optioncore/BlackCalc.h
 * Removed QuantLib dependency, implemented pure C++ version
 */

#pragma once

#include "OptionTypes.h"
#include "OptionGreeks.h"

namespace wt_option {

/**
 * @brief Black-Scholes option pricing calculator
 * 
 * Calculates option values and Greeks using the Black-Scholes model.
 * Uses forward price instead of spot to handle cost of carry.
 */
class BlackScholes {
public:
    /**
     * @brief Construct Black-Scholes calculator
     * @param optionType Call or Put
     * @param strike Strike price
     * @param forward Forward price of underlying
     * @param stdDev Standard deviation (volatility * sqrt(time))
     * @param discount Discount factor (e^(-r*t))
     */
    BlackScholes(OptionRight optionType, double strike, double forward, 
                 double stdDev, double discount = 1.0);
    
    virtual ~BlackScholes() = default;
    
    /**
     * @brief Calculate option value
     * @return Option price
     */
    double value() const;
    
    /**
     * @brief Calculate delta (dV/dF)
     * @return Forward delta
     */
    double delta() const;
    
    /**
     * @brief Calculate gamma (d²V/dF²)
     * @return Forward gamma
     */
    double gamma() const;
    
    /**
     * @brief Calculate vega (dV/dσ)
     * @param maturity Time to maturity in years
     * @return Vega per 1% volatility change
     */
    double vega(double maturity) const;
    
    /**
     * @brief Calculate theta per day
     * @param rate Risk-free rate
     * @param vol Volatility
     * @return Daily theta (decay per calendar day)
     */
    virtual double thetaPerDay(double rate, double vol) const;
    
    /**
     * @brief Calculate rho (dV/dr)
     * @param maturity Time to maturity in years
     * @return Rho per 1% rate change
     */
    double rho(double maturity) const;
    
    /**
     * @brief Calculate vanna (d²V/dFdσ)
     * @param maturity Time to maturity in years
     * @return Vanna (cross-gamma between delta and vega)
     */
    double vanna(double maturity) const;
    
    /**
     * @brief Calculate volga (d²V/dσ²)
     * @param maturity Time to maturity in years
     * @return Volga (vega convexity)
     */
    double volga(double maturity) const;
    
    /**
     * @brief Calculate all Greeks at once
     * @param maturity Time to maturity in years
     * @param rate Risk-free rate
     * @param vol Volatility
     * @return OptionGreeks structure with all Greeks
     */
    OptionGreeks calculateGreeks(double maturity, double rate, double vol) const;
    
    // Static utility functions
    
    /**
     * @brief Calculate implied volatility using Newton-Raphson method
     * @param optionType Call or Put
     * @param marketPrice Market price of option
     * @param forward Forward price
     * @param strike Strike price
     * @param maturity Time to maturity in years
     * @param discount Discount factor
     * @param initialGuess Initial volatility guess (default 0.2)
     * @return Implied volatility, or NaN if convergence fails
     */
    static double impliedVolatility(OptionRight optionType, double marketPrice,
                                    double forward, double strike, double maturity,
                                    double discount, double initialGuess = 0.2);

    // Static helpers for StandardOptionPricer
    static void calculate(double forward, double strike, double maturity, double rate, double q, double vol, 
                          OptionRight right, OptionValues& values);

    static double impliedVol(double price, double forward, double strike, double maturity, 
                             double rate, double q, OptionRight right);

protected:
    void initialize(OptionRight optionType);
    
    double m_strike;
    double m_forward;
    double m_stdDev;
    double m_discount;
    double m_variance;
    
    double m_d1, m_d2;
    double m_alpha, m_beta;
    double m_DalphaDd1, m_DbetaDd2;
    double m_n_d1, m_cum_d1;
    double m_n_d2, m_cum_d2;
    double m_x;
    double m_DxDs, m_DxDstrike;
};

using BlackScholesPtr = std::shared_ptr<BlackScholes>;

} // namespace wt_option
