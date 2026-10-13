/*!
 * \file PowerVolCurve.h
 * \brief Power-law volatility curve — absorbed from quantbox optioncore
 *        (longbeach::optioncore::PowerVolCurve), with two deliberate fixes.
 *
 * Purpose (R2): serve as curve2 of the GVV×Power dual-curve blend, restoring
 * the ORIGINAL quantbox PeriodicCurveFitter design. Our migration had fitted
 * TWO GvvVolCurve instances on the same dataset, making volcurve_weight a
 * no-op (vol ≡ vol2). With PowerVolCurve as curve2 the blend becomes
 * meaningful again: GVV captures the parametric smile shape, the power law
 * absorbs the empirical wing tilt from each fit window.
 *
 * Deliberate divergences from the original (documented per D3 "port logic,
 * not defects"):
 *  1. Domain fix: original fit() computed ln(strike_pos) where strike_pos is
 *     an ATM-relative offset that is NEGATIVE for downside strikes → NaN
 *     regression inputs; eval() then consumed raw offsets while fit had been
 *     designed for positive ratios. We fit in log-strike-ratio space instead:
 *         sv(K) = a · (K/F)^b + c      ⇔      eval(x) = a·e^{b·x} + c,
 *     x = ln(K/F) = ATM offset. Well-defined on both wings, monotone, and
 *     consistent between fit and eval.
 *  2. No GSL: the 2-parameter weighted linear LS on design matrix [1, x]
 *     (after the log transform y = ln(sv − c)) has a closed-form 2×2 normal
 *     equation — solved inline, zero external dependency (D3 decision).
 *
 * The `c` baseline mirrors the original: 0 by default, −1 when constructed
 * from GvvVolCurve::RHO (skew-dominated regime), matching quantbox semantics.
 */
#pragma once

#include "IVolCurve.h"
#include "GvvVolCurve.h"   // GvvVolCurve::Parameter reuse

#include <string>
#include <memory>

namespace wt_option {

class OptionData;

class PowerVolCurve : public IVolCurve
{
public:
    enum Parameter { PAR_A, PAR_B };

    PowerVolCurve();
    explicit PowerVolCurve(GvvVolCurve::Parameter gvvparam);
    ~PowerVolCurve() override = default;

    // -- IVolCurve / ICurve -------------------------------------------------
    double eval(double x) const;                                   // x = ln(K/F)
    using IVolCurve::operator();
    double operator()(const OptionData& od, double atmforward) const override;
    double operator()(double diff) const override;                 // diff = ln(K/F)
    bool   fit(const dataset_t& points) override;                  // {offset, sv} normalized
    bool   isInitialized() const override { return m_bInitialized; }
    const std::string getNameString() const override { return "PowerVolCurve"; }

    // -- ATM anchors driven by OptionPricer2 (same protocol as GvvVolCurve) --
    void setATMForward(double fwd) override { m_atmforward = fwd; }
    void setATMVol(double /*atmvol*/) override {}                  // fit input already normalized

    double getParameter(Parameter p) const { return p == PAR_A ? m_a : m_b; }

private:
    void updateParameters(double a, double b);

    GvvVolCurve::Parameter m_gvvparam;
    double m_c;
    double m_a = 1.0;
    double m_b = 0.0;
    double m_atmforward = 0.0;
    bool   m_bInitialized = false;
};

using PowerVolCurvePtr = std::shared_ptr<PowerVolCurve>;

} // namespace wt_option
