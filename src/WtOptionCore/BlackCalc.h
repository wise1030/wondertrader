/*!
 * \file BlackCalc.h
 * \brief Black-Scholes / Black76 option calculator (migrated from quantbox, no QuantLib dependency)
 *
 * Original: longbeach::optioncore::BlackCalc, depended on QuantLib for
 * CumulativeNormalDistribution, Option::Type, and QL_REQUIRE/QL_FAIL macros.
 * Replaced with standard C++ (<cmath> erfc/exp) and a local OptionType enum.
 */
#pragma once

#include "optioncoretypes.h"
#include <cmath>

namespace wt_option {

// Option type replacing QuantLib::Option::Type.
// QuantLib uses Call = +1, Put = -1; we preserve those numeric values so
// arithmetic on optionType_ (alpha/beta sign flips) is unchanged.
enum OptionType { OT_Call = 1, OT_Put = -1 };

/*! \brief Black76 / Black-Scholes style option pricer.
 *
 * \bug When the variance is null, division by zero occurs during
 *      the calculation of gamma, gamma forward, rho, dividend rho, vega.
 */
class BlackCalc {
public:
    BlackCalc(OptionType optionType, double strike, double forward, double stdDev, double discount = 1.0);
    virtual ~BlackCalc() {}

    double value() const;

    /*! dP/dF */
    double delta() const;

    /*! d2P/dF2 */
    double gamma() const;

    /*! d2P/dFdV */
    double vanna(double maturity) const;

    /*! Sensitivity to time to maturity. */
    virtual double thetaPerDay(double spot, double maturity) const;

    /*! dP/dV */
    double vega(double maturity) const;

    /*! d2P/dV2 */
    double volga(double maturity) const;

    /*! Sensitivity to discounting rate. */
    double rho(double maturity) const;

    /*! Sensitivity to dividend/growth rate. */
    double dividendRho(double maturity) const;

protected:
    void initialize(const OptionType& optionType);
    double strike_, forward_, stdDev_, discount_, variance_;
    double d1_, d2_;
    double alpha_, beta_, DalphaDd1_, DbetaDd2_;
    double n_d1_, cum_d1_, n_d2_, cum_d2_;
    double x_, DxDs_, DxDstrike_;
};

} // namespace wt_option
