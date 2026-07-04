/*!
 * \file BlackImpliedCalculator.h
 * \brief Implied-volatility solver via Brent's method (migrated from quantbox)
 *
 * Original: longbeach::optioncore::BlackImpliedCalculator used
 * QuantLib::Brent and QuantLib::Option::Type. Both are replaced:
 *   - Option::Type -> wt_option::OptionType (from BlackCalc.h)
 *   - QuantLib::Brent -> wt_option::Brent (self-contained 1-D root finder
 *     inlined in the .cpp, same solve() surface: functor + accuracy +
 *     guess + bracket [xMin,xMax]).
 */
#pragma once

#include "BlackCalc.h"

#include <cmath>
#include <stdexcept>
#include <utility>  // std::swap

namespace wt_option {

// ---------------------------------------------------------------------------
// Brent's method 1-D root finder (replaces QuantLib::Brent)
// ---------------------------------------------------------------------------
// Reproduces the surface used by the original code:
//   setMaxEvaluations(n)
//   solve(f, accuracy, guess, xMin, xMax)
// Returns the root of f in [xMin, xMax] to within `accuracy`.
// Implementation is header-inline so arbitrary functors (not just
// std::function) work without template instantiation issues.
class Brent
{
public:
    Brent() : m_maxEvaluations(100) {}

    void setMaxEvaluations(unsigned long int n) { m_maxEvaluations = n; }
    unsigned long int getMaxEvaluations() const { return m_maxEvaluations; }

    // Solve f(x) = 0 bracketed in [xMin, xMax], starting from guess.
    // Precondition: f(xMin) and f(xMax) have opposite signs.
    template<typename F>
    double solve(const F& f, double accuracy,
                 double guess, double xMin, double xMax) const
    {
        // The classic Brent algorithm (Numerical Recipes / Brent 1973).
        // Mirrors QuantLib::Brent behaviour for our PriceError functor.
        const double tiny = 1.0e-10;

        double a = xMin, b = xMax;
        double fa = f(a);
        double fb = f(b);

        // If the bracket isn't a sign change, fall back to bisection
        // across the wider range; QuantLib would throw here.
        if (fa * fb > 0.0) {
            // Try the guess as a fallback root if it's a near-zero residual.
            double fg = f(guess);
            if (std::fabs(fg) < accuracy) return guess;
            throw std::runtime_error("Brent::solve: root not bracketed");
        }

        if (std::fabs(fa) < std::fabs(fb)) { std::swap(a, b); std::swap(fa, fb); }

        double c = a, fc = fa;
        double d = b - a;       // last step
        bool mflag = true;

        for (unsigned long int iter = 0; iter < m_maxEvaluations; ++iter) {
            if (std::fabs(fa) < accuracy) return a;
            if (std::fabs(fb) < accuracy) return b;

            double s;
            if (fa != fc && fb != fc) {
                // inverse quadratic interpolation
                s = (a * fb * fc) / ((fa - fb) * (fa - fc))
                  + (b * fa * fc) / ((fb - fa) * (fb - fc))
                  + (c * fa * fb) / ((fc - fa) * (fc - fb));
            } else {
                // secant
                s = b - fb * (b - a) / (fb - fa);
            }

            // Conditions to use bisection instead of IQI/secant:
            bool use_bisect =
                (s < (3 * a + b) / 4 || s > b) ||
                (mflag && std::fabs(s - b) >= std::fabs(b - c) / 2) ||
                (!mflag && std::fabs(s - b) >= std::fabs(c - d) / 2) ||
                (mflag && std::fabs(b - c) < tiny) ||
                (!mflag && std::fabs(c - d) < tiny);

            if (use_bisect) {
                s = 0.5 * (a + b);
                mflag = true;
            } else {
                mflag = false;
            }

            double fs = f(s);
            d = c;          // last-last step
            c = b; fc = fb;

            if (fa * fs < 0.0) {
                b = s; fb = fs;
            } else {
                a = s; fa = fs;
            }

            if (std::fabs(fa) < std::fabs(fb)) {
                std::swap(a, b); std::swap(fa, fb);
            }

            if (std::fabs(fb) < accuracy) return b;
        }

        return b; // best effort after maxEvaluations
    }

private:
    unsigned long int m_maxEvaluations;
};

// ---------------------------------------------------------------------------
// BlackImpliedCalculator
// ---------------------------------------------------------------------------
class BlackImpliedCalculator {
public:
    BlackImpliedCalculator(
        OptionType optionType,
        double strike, double forward, double maturity, double discount = 1.0);
    virtual ~BlackImpliedCalculator() {}

    void update( double fwd, double maturity, double discount )
    {
        forward_ = fwd;
        maturity_ = maturity;
        discount_ = discount;
    }

    double volatility(
        double price,
        double accuracy = 1.0e-4,
        unsigned long int maxEvaluations = 100,
        double minVol = 1.0e-7,
        double maxVol = 4.0) const;

private:
    OptionType optionType_;
    double strike_, forward_, maturity_, discount_;
    mutable Brent m_solver;
};

} // namespace wt_option
