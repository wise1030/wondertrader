/*!
 * \file BlackCalc.cpp
 * \brief BlackCalc implementation (migrated from quantbox, no QuantLib dependency)
 */
#include "BlackCalc.h"

#include <cmath>
#include <cfloat>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sstream>

namespace wt_option {

namespace {
// Standard normal CDF via complementary error function:
//   Phi(x) = 0.5 * erfc(-x / sqrt(2))
inline double cumNormal(double x)
{
    return 0.5 * erfc(-x / M_SQRT2);
}

// Standard normal PDF:
//   phi(x) = exp(-0.5 * x^2) / sqrt(2*pi)
inline double normalPdf(double x)
{
    return std::exp(-0.5 * x * x) / std::sqrt(2.0 * M_PI);
}

// Replaces QuantLib::close(a, b) with size-based epsilon comparison.
inline bool close_enough(double a, double b)
{
    return std::fabs((a) - (b)) < 1e-15;
}
} // namespace

BlackCalc::BlackCalc(OptionType optionType, double strike, double forward, double stdDev, double discount)
    : strike_(strike), forward_(forward), stdDev_(stdDev),
      discount_(discount), variance_(stdDev*stdDev) {
    initialize(optionType);
}

void BlackCalc::initialize(const OptionType& optionType) {
    // QL_REQUIRE replacements: stream-message into std::runtime_error.
    {
        if (!(strike_>=0.0)) {
            std::ostringstream oss; oss << "strike (" << strike_ << ") must be non-negative";
            throw std::runtime_error(oss.str());
        }
    }
    {
        if (!(forward_>0.0)) {
            std::ostringstream oss; oss << "forward (" << forward_ << ") must be positive";
            throw std::runtime_error(oss.str());
        }
    }
    //QL_REQUIRE(stdDev_>=0.0, "stdDev (" << stdDev_ << ") must be non-negative");
    {
        if (!(discount_>0.0)) {
            std::ostringstream oss; oss << "discount (" << discount_ << ") must be positive";
            throw std::runtime_error(oss.str());
        }
    }

    if (stdDev_>=1e-15) {                 // QL_EPSILON -> 1e-15
        if (close_enough(strike_, 0.0)) { // close(a,b)
            d1_ = DBL_MAX;                // QL_MAX_REAL -> DBL_MAX
            d2_ = DBL_MAX;
            cum_d1_ = 1.0;
            cum_d2_ = 1.0;
            n_d1_ = 0.0;
            n_d2_ = 0.0;
        } else {
            d1_ = std::log(forward_/strike_)/stdDev_ + 0.5*stdDev_;
            d2_ = d1_-stdDev_;
            // CumulativeNormalDistribution f; cum_d1_ = f(d1_); ...
            cum_d1_ = cumNormal(d1_);
            cum_d2_ = cumNormal(d2_);
            n_d1_ = normalPdf(d1_);
            n_d2_ = normalPdf(d2_);
        }
    } else {
        if (close_enough(forward_, strike_)) {
            d1_ = 0;
            d2_ = 0;
            cum_d1_ = 0.5;
            cum_d2_ = 0.5;
            // M_SQRT_2 * M_1_SQRTPI == 1/sqrt(2*pi) == normalPdf(0)
            n_d1_ = normalPdf(0.0);
            n_d2_ = normalPdf(0.0);
        } else if (forward_>strike_) {
            d1_ = DBL_MAX;
            d2_ = DBL_MAX;
            cum_d1_ = 1.0;
            cum_d2_ = 1.0;
            n_d1_ = 0.0;
            n_d2_ = 0.0;
        } else {
            d1_ = -DBL_MAX;               // QL_MIN_REAL -> -DBL_MAX
            d2_ = -DBL_MAX;
            cum_d1_ = 0.0;
            cum_d2_ = 0.0;
            n_d1_ = 0.0;
            n_d2_ = 0.0;
        }
    }

    x_ = strike_;
    DxDstrike_ = 1.0;

    // the following one will probably disappear as soon as
    // super-share will be properly handled
    DxDs_ = 0.0;

    // this part is always executed.
    // in case of plain-vanilla payoffs, it is also the only part
    // which is executed.
    switch (optionType) {
    case OT_Call:                         // Option::Call
        alpha_     =  cum_d1_;//  N(d1)
        DalphaDd1_ =    n_d1_;//  n(d1)
        beta_      = -cum_d2_;// -N(d2)
        DbetaDd2_  = -  n_d2_;// -n(d2)
        break;
    case OT_Put:                          // Option::Put
        alpha_     = -1.0+cum_d1_;// -N(-d1)
        DalphaDd1_ =        n_d1_;//  n( d1)
        beta_      =  1.0-cum_d2_;//  N(-d2)
        DbetaDd2_  =     -  n_d2_;// -n( d2)
        break;
    default:
        throw std::runtime_error("invalid option type"); // QL_FAIL
    }

}

double BlackCalc::value() const {
    double result = discount_ * (forward_ * alpha_ + x_ * beta_);
    return result;
}

double BlackCalc::delta() const {

    double temp = stdDev_*forward_;
    double DalphaDforward = DalphaDd1_/temp;
    double DbetaDforward  = DbetaDd2_/temp;
    double temp2 = DalphaDforward * forward_ + alpha_
        +DbetaDforward  * x_; // DXDforward = 0.0

    return discount_ * temp2;
}

double BlackCalc::gamma() const {

    double temp = stdDev_*forward_;
    double DalphaDforward = DalphaDd1_/temp;
    double DbetaDforward  = DbetaDd2_/temp;

    double D2alphaDforward2 = - DalphaDforward/forward_*(1+d1_/stdDev_);
    double D2betaDforward2  = - DbetaDforward /forward_*(1+d2_/stdDev_);

    double temp2 = D2alphaDforward2 * forward_ + 2.0 * DalphaDforward
        +D2betaDforward2  * x_; // DXDforward = 0.0

    return discount_ * temp2;
}

double BlackCalc::vanna(double maturity) const {
    if (!(maturity>=0.0)) {
        throw std::runtime_error("negative maturity not allowed");
    }

    double temp = stdDev_*forward_;
    double DalphaDforward = DalphaDd1_/temp;
    double DbetaDforward  = DbetaDd2_/temp;

    double temp2 = std::log(strike_/forward_)/variance_;
    double DalphaDsigma = DalphaDd1_*(temp2+0.5)*std::sqrt(maturity);

    double D2alphaDforwardDsigma = -DalphaDforward*d1_*(temp2+0.5) - DalphaDd1_/stdDev_*std::sqrt(maturity);
    double D2betaDforwardDsigma  = -DbetaDforward*d2_*(temp2-0.5) - DbetaDd2_/stdDev_*std::sqrt(maturity);

    double temp3 = D2alphaDforwardDsigma * forward_ + DalphaDsigma
        +D2betaDforwardDsigma  * x_; // DXDforward = 0.0

    return discount_ * temp3;
}

double BlackCalc::thetaPerDay(double rate, double vol) const {
    // our custom theta definition assuming forward doesn't change
    // now theta should come from two parts: a part from gamma and a part from discounting
    return - rate * value() / 365 // one calendary day discounting decay
        - 0.5 * vol * vol * forward_ * forward_ * gamma() / 252; // one business day gamma decay
}

double BlackCalc::vega(double maturity) const {
    if (!(maturity>=0.0)) {
        throw std::runtime_error("negative maturity not allowed");
    }

    double temp = std::log(strike_/forward_)/variance_;
    double DalphaDsigma = DalphaDd1_*(temp+0.5)*std::sqrt(maturity);
    double DbetaDsigma  = DbetaDd2_ *(temp-0.5)*std::sqrt(maturity);

    double temp2 = DalphaDsigma * forward_ + DbetaDsigma * x_;

    return discount_ * temp2;

}

double BlackCalc::volga(double maturity) const {
    if (!(maturity>=0.0)) {
        throw std::runtime_error("negative maturity not allowed");
    }

    double temp = std::log(strike_/forward_)/variance_;
    double DalphaDsigma = DalphaDd1_*(temp+0.5)*std::sqrt(maturity);
    double DbetaDsigma  = DbetaDd2_ *(temp-0.5)*std::sqrt(maturity);

    double D2alphaDsigma2 = -DalphaDsigma*d1_*(temp+0.5)*std::sqrt(maturity) - DalphaDd1_/forward_/forward_/stdDev_;
    double D2betaDsigma2 = -DbetaDsigma*d2_*(temp-0.5)*std::sqrt(maturity) - DbetaDd2_/forward_/forward_/stdDev_;

    double temp2 = D2alphaDsigma2 * forward_ + D2betaDsigma2 * x_;

    return discount_ * temp2;

}

double BlackCalc::rho(double maturity) const {
    if (!(maturity>=0.0)) {
        throw std::runtime_error("negative maturity not allowed");
    }

    // actually DalphaDr / T
    double DalphaDr = DalphaDd1_/stdDev_;
    double DbetaDr  = DbetaDd2_/stdDev_;
    double temp = DalphaDr * forward_ + alpha_ * forward_ + DbetaDr * x_;

    return maturity * (discount_ * temp - value());
}

double BlackCalc::dividendRho(double maturity) const {
    if (!(maturity>=0.0)) {
        throw std::runtime_error("negative maturity not allowed");
    }

    // actually DalphaDq / T
    double DalphaDq = -DalphaDd1_/stdDev_;
    double DbetaDq  = -DbetaDd2_/stdDev_;

    double temp = DalphaDq * forward_ - alpha_ * forward_ + DbetaDq * x_;

    return maturity * discount_ * temp;
}

} // namespace wt_option
