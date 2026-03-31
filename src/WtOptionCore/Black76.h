/*!
 * \file Black76.h
 * \brief Black76 option pricing model for futures options
 * 
 * Used for pricing options on futures where forward price is the underlying.
 */

#pragma once

#include "OptionTypes.h"
#include "OptionGreeks.h"

namespace wt_option {

/**
 * @brief Black76 option pricing calculator
 * 
 * Standard model for pricing options on futures.
 * Assumes the underlying is a forward price (cost of carry = 0).
 */
class Black76 {
public:
    /**
     * @brief Construct Black76 calculator
     * @param optionType Call or Put
     * @param strike Strike price
     * @param forward Forward price of underlying
     * @param stdDev Standard deviation (volatility * sqrt(time))
     * @param discount Discount factor (e^(-r*t))
     */
    Black76(OptionRight optionType, double strike, double forward, 
            double stdDev, double discount = 1.0);
    
    virtual ~Black76() = default;
    
    double value() const;
    double delta() const;
    double gamma() const;
    double vega(double maturity) const;
    double thetaPerDay(double rate, double vol) const;
    double rho(double maturity) const;
    
    OptionGreeks calculateGreeks(double maturity, double rate, double vol) const;

    // Static helpers for StandardOptionPricer
    static void calculate(double forward, double strike, double maturity, double rate, double vol, 
                          OptionRight right, OptionValues& values);
                          
    static double impliedVol(double price, double forward, double strike, double maturity, 
                             double rate, OptionRight right);

protected:
    void initialize(OptionRight optionType);
    
    double m_strike;
    double m_forward;
    double m_stdDev;
    double m_discount;
    double m_variance;
    
    double m_d1, m_d2;
    double m_alpha, m_beta;     // N(d1), N(d2) or related
    double m_n_d1, m_n_d2;      // PDF values
    double m_DalphaDd1, m_DbetaDd2;
};

using Black76Ptr = std::shared_ptr<Black76>;

} // namespace wt_option
