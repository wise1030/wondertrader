/*!
 * \file BlackScholes.cpp
 * \brief Black-Scholes option pricing implementation
 * 
 * Migrated from longbeach/quantbox/strategy/optioncore/BlackCalc.cc
 * Pure C++ implementation without QuantLib dependency
 */

#include "BlackScholes.h"
#include <cmath>
#include <algorithm>
#include <limits>

namespace wt_option {

// Normal distribution functions
double normalCDF(double x) {
    // Abramowitz and Stegun approximation
    const double a1 =  0.254829592;
    const double a2 = -0.284496736;
    const double a3 =  1.421413741;
    const double a4 = -1.453152027;
    const double a5 =  1.061405429;
    const double p  =  0.3275911;
    
    int sign = 1;
    if (x < 0) {
        sign = -1;
        x = -x;
    }
    
    double t = 1.0 / (1.0 + p * x);
    double y = 1.0 - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t * std::exp(-x * x / 2.0);
    
    return 0.5 * (1.0 + sign * y);
}

double normalPDF(double x) {
    return INV_SQRT_2PI * std::exp(-0.5 * x * x);
}

BlackScholes::BlackScholes(OptionRight optionType, double strike, double forward, 
                           double stdDev, double discount)
    : m_strike(strike)
    , m_forward(forward)
    , m_stdDev(stdDev)
    , m_discount(discount)
    , m_variance(stdDev * stdDev)
{
    initialize(optionType);
}

void BlackScholes::initialize(OptionRight optionType) {
    // Validate inputs
    if (m_strike < 0.0) {
        m_strike = 0.0;
    }
    if (m_forward <= 0.0) {
        m_forward = EPSILON;
    }
    if (m_discount <= 0.0) {
        m_discount = EPSILON;
    }
    
    if (m_stdDev >= EPSILON) {
        if (close(m_strike, 0.0)) {
            m_d1 = std::numeric_limits<double>::max();
            m_d2 = std::numeric_limits<double>::max();
            m_cum_d1 = 1.0;
            m_cum_d2 = 1.0;
            m_n_d1 = 0.0;
            m_n_d2 = 0.0;
        } else {
            m_d1 = std::log(m_forward / m_strike) / m_stdDev + 0.5 * m_stdDev;
            m_d2 = m_d1 - m_stdDev;
            m_cum_d1 = normalCDF(m_d1);
            m_cum_d2 = normalCDF(m_d2);
            m_n_d1 = normalPDF(m_d1);
            m_n_d2 = normalPDF(m_d2);
        }
    } else {
        if (close(m_forward, m_strike)) {
            m_d1 = 0;
            m_d2 = 0;
            m_cum_d1 = 0.5;
            m_cum_d2 = 0.5;
            m_n_d1 = INV_SQRT_2PI * std::sqrt(2.0);
            m_n_d2 = INV_SQRT_2PI * std::sqrt(2.0);
        } else if (m_forward > m_strike) {
            m_d1 = std::numeric_limits<double>::max();
            m_d2 = std::numeric_limits<double>::max();
            m_cum_d1 = 1.0;
            m_cum_d2 = 1.0;
            m_n_d1 = 0.0;
            m_n_d2 = 0.0;
        } else {
            m_d1 = std::numeric_limits<double>::lowest();
            m_d2 = std::numeric_limits<double>::lowest();
            m_cum_d1 = 0.0;
            m_cum_d2 = 0.0;
            m_n_d1 = 0.0;
            m_n_d2 = 0.0;
        }
    }
    
    m_x = m_strike;
    m_DxDstrike = 1.0;
    m_DxDs = 0.0;
    
    // Set alpha and beta based on option type
    switch (optionType) {
        case OptionRight::Call:
            m_alpha = m_cum_d1;           //  N(d1)
            m_DalphaDd1 = m_n_d1;         //  n(d1)
            m_beta = -m_cum_d2;            // -N(d2)
            m_DbetaDd2 = -m_n_d2;          // -n(d2)
            break;
        case OptionRight::Put:
            m_alpha = -1.0 + m_cum_d1;    // -N(-d1)
            m_DalphaDd1 = m_n_d1;          //  n(d1)
            m_beta = 1.0 - m_cum_d2;       //  N(-d2)
            m_DbetaDd2 = -m_n_d2;          // -n(d2)
            break;
    }
}

double BlackScholes::value() const {
    return m_discount * (m_forward * m_alpha + m_x * m_beta);
}

double BlackScholes::delta() const {
    double temp = m_stdDev * m_forward;
    if (temp < EPSILON) return (m_alpha > 0) ? 1.0 : 0.0;
    
    double DalphaDforward = m_DalphaDd1 / temp;
    double DbetaDforward = m_DbetaDd2 / temp;
    double temp2 = DalphaDforward * m_forward + m_alpha + DbetaDforward * m_x;
    
    return m_discount * temp2;
}

double BlackScholes::gamma() const {
    double temp = m_stdDev * m_forward;
    if (temp < EPSILON) return 0.0;
    
    double DalphaDforward = m_DalphaDd1 / temp;
    double DbetaDforward = m_DbetaDd2 / temp;
    
    double D2alphaDforward2 = -DalphaDforward / m_forward * (1 + m_d1 / m_stdDev);
    double D2betaDforward2 = -DbetaDforward / m_forward * (1 + m_d2 / m_stdDev);
    
    double temp2 = D2alphaDforward2 * m_forward + 2.0 * DalphaDforward + D2betaDforward2 * m_x;
    
    return m_discount * temp2;
}

double BlackScholes::vega(double maturity) const {
    if (maturity < 0.0 || m_variance < EPSILON) return 0.0;
    
    double temp = std::log(m_strike / m_forward) / m_variance;
    double DalphaDsigma = m_DalphaDd1 * (temp + 0.5) * std::sqrt(maturity);
    double DbetaDsigma = m_DbetaDd2 * (temp - 0.5) * std::sqrt(maturity);
    
    double temp2 = DalphaDsigma * m_forward + DbetaDsigma * m_x;
    
    return m_discount * temp2;
}

double BlackScholes::thetaPerDay(double rate, double vol) const {
    // Theta from two parts: discounting decay and gamma decay
    return -rate * value() / 365.0  // One calendar day discounting decay
           - 0.5 * vol * vol * m_forward * m_forward * gamma() / 252.0;  // One business day gamma decay
}

double BlackScholes::rho(double maturity) const {
    if (maturity < 0.0) return 0.0;
    
    double DalphaDr = m_DalphaDd1 / m_stdDev;
    double DbetaDr = m_DbetaDd2 / m_stdDev;
    double temp = DalphaDr * m_forward + m_alpha * m_forward + DbetaDr * m_x;
    
    return maturity * (m_discount * temp - value());
}

double BlackScholes::vanna(double maturity) const {
    if (maturity < 0.0 || m_variance < EPSILON) return 0.0;
    
    double temp = m_stdDev * m_forward;
    double DalphaDforward = m_DalphaDd1 / temp;
    double DbetaDforward = m_DbetaDd2 / temp;
    
    double temp2 = std::log(m_strike / m_forward) / m_variance;
    double sqrtT = std::sqrt(maturity);
    
    double D2alphaDforwardDsigma = -DalphaDforward * m_d1 * (temp2 + 0.5) - m_DalphaDd1 / m_stdDev * sqrtT;
    double D2betaDforwardDsigma = -DbetaDforward * m_d2 * (temp2 - 0.5) - m_DbetaDd2 / m_stdDev * sqrtT;
    
    double DalphaDsigma = m_DalphaDd1 * (temp2 + 0.5) * sqrtT;
    double temp3 = D2alphaDforwardDsigma * m_forward + DalphaDsigma + D2betaDforwardDsigma * m_x;
    
    return m_discount * temp3;
}

double BlackScholes::volga(double maturity) const {
    if (maturity < 0.0 || m_variance < EPSILON) return 0.0;
    
    double temp = std::log(m_strike / m_forward) / m_variance;
    double sqrtT = std::sqrt(maturity);
    
    double DalphaDsigma = m_DalphaDd1 * (temp + 0.5) * sqrtT;
    double DbetaDsigma = m_DbetaDd2 * (temp - 0.5) * sqrtT;
    
    double fwdSq = m_forward * m_forward;
    double D2alphaDsigma2 = -DalphaDsigma * m_d1 * (temp + 0.5) * sqrtT - m_DalphaDd1 / fwdSq / m_stdDev;
    double D2betaDsigma2 = -DbetaDsigma * m_d2 * (temp - 0.5) * sqrtT - m_DbetaDd2 / fwdSq / m_stdDev;
    
    double temp2 = D2alphaDsigma2 * m_forward + D2betaDsigma2 * m_x;
    
    return m_discount * temp2;
}

OptionGreeks BlackScholes::calculateGreeks(double maturity, double rate, double vol) const {
    OptionGreeks greeks;
    greeks.delta() = delta();
    greeks.gamma() = gamma();
    greeks.vega() = vega(maturity);
    greeks.theta() = thetaPerDay(rate, vol);
    greeks.rho() = rho(maturity);
    greeks.vanna() = vanna(maturity);
    greeks.volga() = volga(maturity);
    return greeks;
}

double BlackScholes::impliedVolatility(OptionRight optionType, double marketPrice,
                                        double forward, double strike, double maturity,
                                        double discount, double initialGuess) {
    if (marketPrice <= 0 || maturity <= 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    
    const int maxIterations = 100;
    const double tolerance = 1e-8;
    
    double vol = initialGuess;
    
    for (int i = 0; i < maxIterations; ++i) {
        double stdDev = vol * std::sqrt(maturity);
        BlackScholes bs(optionType, strike, forward, stdDev, discount);
        
        double price = bs.value();
        double vega = bs.vega(maturity);
        
        if (std::abs(vega) < EPSILON) {
            // Vega too small, try a different approach
            vol *= 1.5;
            continue;
        }
        
        double diff = price - marketPrice;
        if (std::abs(diff) < tolerance) {
            return vol;
        }
        
        vol -= diff / vega;
        
        // Bound volatility
        vol = std::max(MIN_VOL, std::min(MAX_VOL, vol));
    }
    
    return std::numeric_limits<double>::quiet_NaN();
}

void BlackScholes::calculate(double forward, double strike, double maturity, double rate, double q, double vol, 
                             OptionRight right, OptionValues& values) {
    if (maturity < 0) maturity = 0;
    // stdDev = vol * sqrt(T)
    double stdDev = vol * std::sqrt(maturity);
    double discount = std::exp(-rate * maturity);
    
    // BlackScholes constructor takes forward, so q is implicitly handled in forward calculation 
    // OR if forward is spot, we need to adjust?
    // StandardOptionPricer passes "forward" which is already F = S * e^{(r-q)T}.
    // But BlackScholes constructor signature is (type, strike, forward, stdDev, discount).
    // So 'q' parameter here is redundant if 'forward' is truly forward price.
    // However, if we want to be precise about rho (which depends on q via forward sensitivity?),
    // BlackScholes implementation assumes forward is given.
    
    // Check call site: StandardOptionPricer computes forward using r and q.
    // So valid.
    
    BlackScholes bs(right, strike, forward, stdDev, discount);
    
    values.theoreticalPrice = bs.value();
    
    // Fill greeks
    OptionGreeks greeks = bs.calculateGreeks(maturity, rate, vol);
    values.greeks = greeks;
}

double BlackScholes::impliedVol(double price, double forward, double strike, double maturity, 
                                double rate, double q, OptionRight right) {
    double discount = std::exp(-rate * maturity);
    return impliedVolatility(right, price, forward, strike, maturity, discount);
}

} // namespace wt_option
