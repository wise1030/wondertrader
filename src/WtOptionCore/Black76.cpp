/*!
 * \file Black76.cpp
 * \brief Black76 option pricing implementation
 */

#include "Black76.h"
#include "BlackScholes.h" // Reuse normal distribution functions from BS
#include <cmath>
#include <algorithm>
#include <limits>

namespace wt_option {

// Forward declare helper functions from BlackScholes.cpp
double normalCDF(double x);
double normalPDF(double x);

Black76::Black76(OptionRight optionType, double strike, double forward, 
                 double stdDev, double discount)
    : m_strike(strike)
    , m_forward(forward)
    , m_stdDev(stdDev)
    , m_discount(discount)
    , m_variance(stdDev * stdDev)
{
    initialize(optionType);
}

void Black76::initialize(OptionRight optionType) {
    if (m_strike < 0.0) m_strike = 0.0;
    if (m_forward <= 0.0) m_forward = EPSILON;
    
    if (m_stdDev >= EPSILON) {
        m_d1 = std::log(m_forward / m_strike) / m_stdDev + 0.5 * m_stdDev;
        m_d2 = m_d1 - m_stdDev;
    } else {
        // Zero volatility case
        if (m_forward > m_strike) {
            m_d1 = std::numeric_limits<double>::infinity();
            m_d2 = std::numeric_limits<double>::infinity();
        } else if (m_forward < m_strike) {
            m_d1 = -std::numeric_limits<double>::infinity();
            m_d2 = -std::numeric_limits<double>::infinity();
        } else {
            m_d1 = 0;
            m_d2 = 0;
        }
    }
    
    double nd1 = normalCDF(m_d1);
    double nd2 = normalCDF(m_d2);
    m_n_d1 = normalPDF(m_d1);
    
    if (optionType == OptionRight::Call) {
        m_alpha = nd1;
        m_beta = -nd2;
    } else {
        m_alpha = nd1 - 1.0;
        m_beta = 1.0 - nd2;
    }
}

double Black76::value() const {
    return m_discount * (m_forward * m_alpha + m_strike * m_beta);
}

double Black76::delta() const {
    // For Black76, delta is e^(-rT) * N(d1)
    return m_discount * m_alpha;
}

double Black76::gamma() const {
    // For Black76, gamma is e^(-rT) * n(d1) / (F * sigma * sqrt(T))
    if (m_forward * m_stdDev < EPSILON) return 0.0;
    return m_discount * m_n_d1 / (m_forward * m_stdDev);
}

double Black76::vega(double maturity) const {
    // For Black76, vega is F * e^(-rT) * n(d1) * sqrt(T)
    if (maturity < 0.0) return 0.0;
    return m_discount * m_forward * m_n_d1 * std::sqrt(maturity);
}

double Black76::thetaPerDay(double rate, double vol) const {
    // Black76 Theta
    // theta = - (F * e^(-rT) * n(d1) * sigma) / (2 * sqrt(T)) + r * V
    // But simplified:
    // discounting decay: -r * V
    // volatility decay: -0.5 * sigma^2 * F^2 * Gamma
    
    double val = value();
    double gam = gamma();
    
    return -rate * val / 365.0 
           - 0.5 * vol * vol * m_forward * m_forward * gam / 252.0;
}

double Black76::rho(double maturity) const {
    // Sensitivity to interest rate (discounting)
    // rho = -T * V (since forward F is assumed constant w.r.t r in this context strictly speaking, 
    // but usually we care about discounting impact)
    return -maturity * value();
}

OptionGreeks Black76::calculateGreeks(double maturity, double rate, double vol) const {
    OptionGreeks greeks;
    greeks.delta() = delta();
    greeks.gamma() = gamma();
    greeks.vega() = vega(maturity);
    greeks.theta() = thetaPerDay(rate, vol);
    greeks.rho() = rho(maturity);
    return greeks;
}

void Black76::calculate(double forward, double strike, double maturity, double rate, double vol, 
                        OptionRight right, OptionValues& values) {
    if (maturity < 0) maturity = 0;
    // stdDev = vol * sqrt(T)
    double stdDev = vol * std::sqrt(maturity);
    double discount = std::exp(-rate * maturity);
    
    Black76 bs(right, strike, forward, stdDev, discount);
    
    values.theoreticalPrice = bs.value();
    
    // Fill greeks
    OptionGreeks greeks = bs.calculateGreeks(maturity, rate, vol);
    values.greeks = greeks;
}

double Black76::impliedVol(double price, double forward, double strike, double maturity, 
                           double rate, OptionRight right) {
    // Basic NR implementation or reuse BlackScholes::impliedVolatility if applicable
    // Since BS and Black76 are mathematically similar (replace S with F*e^-rT), 
    // but Black76 assumes F is direct forward.
    // However, BlackScholes::impliedVolatility takes forward as input.
    // Let's implement a simple wrapper or call BlackScholes one if compatible.
    // Actually BlackScholes::impliedVolatility takes "forward" parameter.
    
    // Using BlackScholes with dividend=rate (cost of carry = 0 for forward)
    // BlackScholes formula uses forward directly.
    
    double discount = std::exp(-rate * maturity);
    // Reuse BlackScholes solver which is generic enough if we pass discount
    return BlackScholes::impliedVolatility(right, price, forward, strike, maturity, discount);
}

} // namespace wt_option
