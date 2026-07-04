/*!
 * \file BlackImpliedCalculator.cpp
 * \brief BlackImpliedCalculator implementation (migrated from quantbox, no QuantLib)
 */
#include "BlackImpliedCalculator.h"
#include "BlackCalc.h"

#include <cmath>

namespace wt_option {

namespace {

// Price-error functor: residual of Black price minus target price.
// Original used QuantLib::Option::Type; now uses wt_option::OptionType.
class PriceError {
public:
    PriceError(
        OptionType optionType,
        double strike, double forward, double price, double maturity, double discount);
    double operator()(double x) const;

private:
    OptionType m_optionType;
    double m_strike, m_forward, m_price, m_maturity, m_discount;
};

PriceError::PriceError(
    OptionType optionType,
    double strike, double forward, double price, double maturity, double discount)
: m_optionType(optionType),
  m_strike(strike), m_forward(forward), m_price(price), m_maturity(maturity), m_discount(discount) {}

double PriceError::operator()(double x) const {
    BlackCalc bc(m_optionType, m_strike, m_forward, x*std::sqrt(m_maturity), m_discount);
    return bc.value() - m_price;
}

} // anonymous namespace

BlackImpliedCalculator::BlackImpliedCalculator(
    OptionType optionType,
    double strike, double forward, double maturity, double discount)
    : optionType_(optionType),
      strike_(strike), forward_(forward), maturity_(maturity), discount_(discount) {}

double BlackImpliedCalculator::volatility(
    double price,
    double accuracy,
    unsigned long int maxEvaluations,
    double minVol,
    double maxVol) const
{
    PriceError f(optionType_, strike_, forward_, price, maturity_, discount_);
    // Brent solver;
    m_solver.setMaxEvaluations(maxEvaluations);
    double guess = (minVol+maxVol)/2.0;
    double vol = m_solver.solve(f, accuracy, guess, minVol, maxVol);
    return vol;
}

} // namespace wt_option
