/*!
 * \file PowerVolCurve.cpp
 * \brief Power-law vol curve implementation (quantbox absorption, domain-fixed)
 */
#include "PowerVolCurve.h"
#include "OptionData.h"
#include "WTSLogger.h"

#include <cmath>
#include <vector>

namespace wt_option {

PowerVolCurve::PowerVolCurve()
    : m_gvvparam(GvvVolCurve::RHO)
    , m_c(-1.0)          // original semantics: RHO regime → baseline −1
{
}

PowerVolCurve::PowerVolCurve(GvvVolCurve::Parameter gvvparam)
    : m_gvvparam(gvvparam)
    , m_c(gvvparam == GvvVolCurve::RHO ? -1.0 : 0.0)
{
}

void PowerVolCurve::updateParameters(double a, double b)
{
    m_a = a;
    m_b = b;
}

double PowerVolCurve::eval(double x) const
{
    if (!m_bInitialized)
        return 1.0;   // neutral multiplier until first fit (matches Gvv convention)
    return m_a * std::exp(m_b * x) + m_c;
}

double PowerVolCurve::operator()(const OptionData& od, double atmforward) const
{
    // Sticky-strike in log-ratio space, mirroring GvvVolCurve::operator():
    const double stk = od.getStrike();
    const double f   = (m_atmforward > 0) ? m_atmforward : atmforward;
    if (f <= 0 || stk <= 0)
        return 1.0;
    const double x = std::log(stk / f);
    return eval(x);
}

double PowerVolCurve::operator()(double diff) const
{
    return eval(diff);
}

// ---------------------------------------------------------------------------
// fit — closed-form weighted linear LS for sv = a·e^{b·x} + c
//   transform: y_i = ln(sv_i − c),  design row: [1, x_i]
//   normal equations (2×2): solve [[S0,S1],[S1,S2]]·[lnA,b]ᵀ = [T0,T1]ᵀ
//   with S_k = Σ w·x^k,  T_k = Σ w·y·x^k
// ---------------------------------------------------------------------------
bool PowerVolCurve::fit(const dataset_t& src_pts)
{
    // Collect usable points: need sv − c > 0 and finite x
    std::vector<double> xs, ys, ws;
    xs.reserve(src_pts.size());
    ys.reserve(src_pts.size());
    ws.reserve(src_pts.size());

    for (const auto& pt : src_pts) {
        // PeriodicCurveFitter supplies K-F, while eval() uses log(K/F).
        // When ATM forward is available, normalize the training x to the same
        // log-ratio feature space as prediction.
        double x = pt.first;
        if (m_atmforward > 0) {
            const double strike = pt.first + m_atmforward;
            if (strike > 0)
                x = std::log(strike / m_atmforward);
        }
        const double sv = pt.second;
        if (!std::isfinite(x) || !std::isfinite(sv))
            continue;
        const double yv = sv - m_c;
        if (!(yv > 1e-9))          // ln domain guard (original NaN'd here)
            continue;
        xs.push_back(x);
        ys.push_back(std::log(yv));
        ws.push_back(1.0);         // uniform weight (original used 1.0 too)
    }

    if (xs.size() < 3) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "PowerVolCurve::fit: only {} usable points (need >=3), keep previous params",
            xs.size());
        return false;
    }

    // Weighted normal equations
    double S0 = 0, S1 = 0, S2 = 0, T0 = 0, T1 = 0;
    for (size_t i = 0; i < xs.size(); ++i) {
        const double w = ws[i], x = xs[i], y = ys[i];
        S0 += w;
        S1 += w * x;
        S2 += w * x * x;
        T0 += w * y;
        T1 += w * y * x;
    }

    const double det = S0 * S2 - S1 * S1;
    if (!std::isfinite(det) || std::fabs(det) < 1e-12) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "PowerVolCurve::fit: degenerate normal matrix (det={:.3e}), keep previous params", det);
        return false;
    }

    const double lnA = (S2 * T0 - S1 * T1) / det;
    const double b   = (S0 * T1 - S1 * T0) / det;
    if (!std::isfinite(lnA) || !std::isfinite(b)) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "PowerVolCurve::fit: non-finite solution, keep previous params");
        return false;
    }

    updateParameters(std::exp(lnA), b);

    // 2026-11-26 quality gate (scale-race fix, user-approved plan §L2) —
    // PowerVolCurve had NO parameter-domain protection, so a mis-normalized
    // dataset (e.g. points scaled by IV/0.1 against a 0.24 market) fit
    // "successfully" and published.  The stored multipliers must describe a
    // market-scale smile:
    //   1. ATM level: eval(0) = a + c must be a sane vol multiplier.
    //   2. Wing bound: eval(±0.5) (= ±50% moneyness) finite and positive.
    //   3. Residual: max |eval(x_i) − sv_i| within tolerance on used points.
    const double level = m_a * std::exp(m_b * 0.0) + m_c;
    const double wingLo = m_a * std::exp(m_b * -0.5) + m_c;
    const double wingHi = m_a * std::exp(m_b *  0.5) + m_c;
    double maxResid = 0.0;
    for (size_t i = 0; i < xs.size(); ++i) {
        const double model = m_a * std::exp(m_b * xs[i]) + m_c;
        const double sv = std::exp(ys[i]) + m_c;   // ys stores ln(sv − c)
        const double resid = std::fabs(model - sv);
        if (resid > maxResid) maxResid = resid;
    }
    if (!std::isfinite(level) || level < 0.45 || level > 1.55
        || !std::isfinite(wingLo) || wingLo < 0.05
        || !std::isfinite(wingHi) || wingHi < 0.05
        || maxResid > 0.4) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "PowerVolCurve::fit: rejected (a={:.4f} b={:.4f} c={:.2f} level={:.3f} "
            "wing=[{:.3f},{:.3f}] maxResid={:.3f})",
            m_a, m_b, m_c, level, wingLo, wingHi, maxResid);
        return false;
    }

    m_bInitialized = true;

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "PowerVolCurve::fit: n={} a={:.4f} b={:.4f} c={:.2f}",
        xs.size(), m_a, m_b, m_c);
    return true;
}

} // namespace wt_option
