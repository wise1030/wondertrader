/*!
 * \file GvvVolCurve.cpp
 * \brief GvvVolCurve implementation (migrated from quantbox)
 *
 * Original used boost::function/bind, QuantLib::Brent, boost::tuple,
 * longbeach::LuaTableLoader, and LONGBEACH_THROW_ERROR_SS. Migrated to:
 *  - std::function / lambda for the objective function
 *  - wt_option::Brent (from BlackImpliedCalculator.h) for root finding
 *  - std::pair for datapoints
 *  - std::runtime_error for errors
 *  - direct double members for alpha_dn/alpha_up (no luabind::object)
 *
 * **BUG FIX (AGENTS.md)**: the original eval() computes
 *   atmvol = (-b + sqrt(b*b - 4*a*c)) / 2a
 * with no guard against b*b-4*a*c < 0, and fit() computes
 *   spotvol = sqrt(atmvol^2 - 0.5*skew*temp + 0.25*kurt*temp^2)
 * with no guard against a negative radicand. Both now clamp the radicand
 * to >= 0 before sqrt.
 */
#include "GvvVolCurve.h"
#include "BlackImpliedCalculator.h"  // wt_option::Brent
#include "OptionData.h"
#include "../WTSTools/WTSLogger.h"

#include <iostream>
#include <cmath>
#include <stdexcept>
#include <functional>

namespace wt_option {

namespace {
const double FP_EPSILON = 1.0e-9;

template<class F>
double BrentMinimize(double low, double mid, double high, double tolerance, size_t maxIt,
                     const F& objectiveFunction)
{
    double W = 0.5 * (3.0 - std::sqrt(5.0));
    double x = W * low + (1 - W) * high;
    if (mid > low && mid < high)
        x = mid;

    double midValue = objectiveFunction(x);

    size_t iterations = 0;
    while (high - low > tolerance && iterations < maxIt) {
        if (x - low > high - x) { // left interval is bigger
            double tentativeNewMid = W * low + (1 - W) * x;
            double tentativeNewMidValue = objectiveFunction(tentativeNewMid);

            if (tentativeNewMidValue < midValue) { // go left
                high = x;
                x = tentativeNewMid;
                midValue = tentativeNewMidValue;
            }
            else { // go right
                low = tentativeNewMid;
            }
        }
        else {
            double tentativeNewMid = W * x + (1 - W) * high;
            double tentativeNewMidValue = objectiveFunction(tentativeNewMid);

            if (tentativeNewMidValue < midValue) { // go right
                low = x;
                x = tentativeNewMid;
                midValue = tentativeNewMidValue;
            }
            else { // go left
                high = tentativeNewMid;
            }
        }
        ++iterations;
    }
    return x;
}

class PriceError {
public:
    PriceError(double x, double atmvol, double skew, double kurt, double alpha)
        : m_x(x), m_atmvol(atmvol), m_skew(skew), m_kurt(kurt), m_alpha(alpha) {}

    double operator()(double v) const {
        double temp = m_x * std::pow(v, m_alpha - 1);
        return v * v - (m_atmvol * m_atmvol + m_skew * temp + m_kurt * temp * temp);
    }

private:
    double m_x, m_atmvol, m_skew, m_kurt, m_alpha;
};

} // anonymous namespace

// ---------------------------------------------------------------------------

GvvVolCurve::GvvVolCurve(const GvvVolCurve::Config& c)
    : m_beta(c.beta)
    , m_atmforward(NAN)
    , m_maturity(NAN)
    , m_atmvol(GVV_MINATMVOL)
    , m_spotvol(GVV_MINATMVOL)
    , m_rho(0.0)
    , m_volvol(GVV_MINVOLVOL)
    , m_alpha(1.0)
    , m_alpha_dn(std::max(GVV_MINALPHA, c.alpha_dn))
    , m_alpha_up(std::min(GVV_MAXALPHA, c.alpha_up))
    , m_chisq(0)
    , m_bInitialized(false)
    , m_bLastFitOK(false)
{
    if (m_alpha_dn > m_alpha_up)
        throw std::runtime_error("GvvVolCurve: alpha_dn > alpha_up");
    m_alpha = 0.5 * (m_alpha_dn + m_alpha_up);
}

GvvVolCurve::GvvVolCurve()
    : m_beta(0.0)
    , m_atmforward(NAN)
    , m_maturity(NAN)
    , m_atmvol(GVV_MINATMVOL)
    , m_spotvol(GVV_MINATMVOL)
    , m_rho(0.0)
    , m_volvol(GVV_MINVOLVOL)
    , m_alpha(1.0)
    , m_alpha_dn(GVV_MINALPHA)
    , m_alpha_up(GVV_MAXALPHA)
    , m_chisq(0)
    , m_bInitialized(false)
    , m_bLastFitOK(false)
{
}

GvvVolCurve::~GvvVolCurve()
{
}

void GvvVolCurve::update_parameters(double spotvol, double rho, double volvol, double alpha)
{
    m_spotvol = spotvol;
    m_rho     = rho;
    m_volvol  = volvol;
    m_alpha   = alpha;
}

double GvvVolCurve::eval(double x) const
{
    if (!m_bInitialized)
        return 1.0;

    try {
        double a = (1.0 + 0.25 * m_volvol * m_volvol);
        double b = -m_rho * m_spotvol * m_volvol;
        double c = -m_spotvol * m_spotvol;

        // BUG FIX: guard against negative discriminant (AGENTS.md)
        double disc = b * b - 4.0 * a * c;
        if (disc < 0.0) disc = 0.0;
        double atmvol = (-b + std::sqrt(disc)) / 2.0 / a;

        double kurt = m_volvol * m_volvol / m_maturity / std::pow(atmvol, 2.0 * m_alpha);
        double skew = 2.0 * m_rho * m_spotvol * std::sqrt(kurt);
        PriceError f(x /*logmoneyness*/, atmvol, skew, kurt, m_alpha);
        Brent solver;
        solver.setMaxEvaluations(100);
        double guess = (GVV_MINATMVOL + GVV_MAXATMVOL) / 2.0;
        double vol = solver.solve(f, 1.0e-4, guess, GVV_MINATMVOL, GVV_MAXATMVOL);
        return vol / m_atmvol; // normalized by m_atmvol
    }
    catch (...) {
        // B31 fix: root-not-bracketed used to collapse the wing to
        // MINATMVOL/atmvol (~1% absolute vol) with no log. Fall back to the
        // ATM value instead and warn (throttled).
        static uint64_t s_warnCount = 0;
        if (s_warnCount++ < 20 || s_warnCount % 1000 == 0) {
            WTSLogger::log_by_cat("strategy", LL_WARN,
                "GvvVolCurve::eval solve failed (x={:.4f}) -> fallback ATM, count={}",
                x, s_warnCount);
        }
        return 1.0;
    }
}

double GvvVolCurve::operator()(const OptionData& od, double atmforward) const
{
    double stk = od.getStrike();
    double x = std::log(stk / m_atmforward); // m_atmforward used, not atmforward — sticky strike
    return eval(x);
}

double GvvVolCurve::operator()(double diff) const
{
    double stk = diff + m_atmforward;
    double x = std::log(stk / m_atmforward);
    return eval(x);
}

// alloc_all/free_all removed — WLS3 is stack-only, zero allocation

void GvvVolCurve::setATMForward(double atmforward) { m_atmforward = atmforward; }
void GvvVolCurve::setATMVol(double atmvol)        { m_atmvol = atmvol; }
void GvvVolCurve::setMaturity(double maturity)    { m_maturity = maturity; }

void GvvVolCurve::setParameter(const Parameter& paramType, double val)
{
    switch (paramType) {
    case SPOTVOL: m_spotvol = std::max(GVV_MINATMVOL, std::min(GVV_MAXATMVOL, val)); break;
    case RHO:     m_rho     = std::max(GVV_MINRHO,    std::min(GVV_MAXRHO,    val)); break;
    case VOLVOL:  m_volvol  = std::max(GVV_MINVOLVOL, std::min(GVV_MAXVOLVOL, val)); break;
    case ALPHA:   m_alpha   = std::max(GVV_MINALPHA,  std::min(GVV_MAXALPHA,  val)); break;
    default: throw std::runtime_error("Unexpected GvvVolCurve::Parameter");
    }
}

double GvvVolCurve::getVolBump(const Parameter& paramType, double paramBump, double stk)
{
    double x = std::log(stk / m_atmforward);
    double v, v0 = eval(x);
    double param0;
    switch (paramType) {
    case SPOTVOL:
        return paramBump;
    case RHO:
    case VOLVOL:
    case ALPHA:
        param0 = getParameter(paramType);
        setParameter(paramType, param0 + paramBump);
        v = eval(x);
        setParameter(paramType, param0); // reset
        return m_atmvol * (v - v0);
    default:
        throw std::runtime_error("Unexpected GvvVolCurve::Parameter");
    }
}

double GvvVolCurve::getParameter(const Parameter& paramType) const
{
    switch (paramType) {
    case SPOTVOL: return m_spotvol;
    case RHO:     return m_rho;
    case VOLVOL:  return m_volvol;
    case ALPHA:   return m_alpha;
    case ATMVOL:  return m_atmvol;
    default: throw std::runtime_error("Unexpected GvvVolCurve::Parameter");
    }
}

double GvvVolCurve::fitWithAlpha(double alpha,
    const IVolCurve::dataset_t& points,
    double* atmvol2, double* skew, double* kurt)
{
    size_t n = points.size();

    // WLS3: hand-written 3×3 weighted least squares (replaces gsl_multifit_wlinear)
    WLS3 wls;
    wls.clear();

    for (size_t i = 0; i < n; ++i) {
        double stki = points[i].first + m_atmforward;
        double xi = std::log(stki / m_atmforward); // logmoneyness
        double vi = points[i].second * m_atmvol;   // vol
        double temp = xi * std::pow(vi, alpha - 1);

        double y_i = vi * vi;
        double w_i = std::exp(-m_beta * xi * xi / m_atmvol / m_atmvol / m_maturity);

        // X row = [1, temp, temp²], weighted by w_i
        wls.add(temp, temp * temp, y_i, w_i);
    }

    double c[3];
    if (!wls.solve(c)) {
        *atmvol2 = 0;
        *skew = 0;
        *kurt = 0;
        m_chisq = 1e18;
        return m_chisq;
    }

    *atmvol2 = c[0];
    *skew    = c[1];
    *kurt    = c[2];

    // Compute chi-squared residual
    double chisq = 0;
    for (size_t i = 0; i < n; ++i) {
        double stki = points[i].first + m_atmforward;
        double xi = std::log(stki / m_atmforward);
        double vi = points[i].second * m_atmvol;
        double temp = xi * std::pow(vi, alpha - 1);
        double y_i = vi * vi;
        double w_i = std::exp(-m_beta * xi * xi / m_atmvol / m_atmvol / m_maturity);
        double resid = y_i - (c[0] + c[1] * temp + c[2] * temp * temp);
        chisq += w_i * resid * resid;
    }
    m_chisq = chisq;
    return chisq;
}

namespace {
typedef std::pair<double, double> point_t;
bool comp(const point_t& a, const point_t& b) { return a.first < b.first; }
} // namespace

bool GvvVolCurve::fit(const IVolCurve::dataset_t& src_pts)
{
    m_bLastFitOK = false; // reset

    if (src_pts.size() < 4) {
        std::cout << "GvvVolCurve fit failed: m_maturity " << m_maturity
                  << " less than 4 points in fitting." << std::endl;
        return false;
    }
    if (m_atmvol < GVV_MINATMVOL || m_atmvol > GVV_MAXATMVOL) {
        std::cout << "GvvVolCurve fit failed: m_maturity " << m_maturity
                  << " m_atmvol " << m_atmvol << std::endl;
        return false;
    }

    dataset_t points = src_pts;
    std::sort(points.begin(), points.end(), comp);
    size_t n = points.size();

    // No alloc/free needed — WLS3 is stack-only

    double atmvol2, skew, kurt;
    auto F = [&](double alpha) {
        return fitWithAlpha(alpha, points, &atmvol2, &skew, &kurt);
    };
    double alpha = BrentMinimize(
        m_alpha_dn, // low
        m_alpha,    // mid
        m_alpha_up, // high
        1.0e-3,     // tolerance
        100,        // maxIt
        F);         // objectiveFunction
    F(alpha);

    if (atmvol2 < GVV_MINATMVOL * GVV_MINATMVOL || atmvol2 > GVV_MAXATMVOL * GVV_MAXATMVOL) {
        std::cout << "GvvVolCurve fit failed: m_maturity " << m_maturity
                  << " atmvol2 " << atmvol2 << std::endl;
        return false;
    }
    double atmvol = std::sqrt(atmvol2);

    double volvol2 = kurt * m_maturity * std::pow(atmvol, 2.0 * m_alpha);
    if (volvol2 < GVV_MINVOLVOL * GVV_MINVOLVOL || volvol2 > GVV_MAXVOLVOL * GVV_MAXVOLVOL) {
        std::cout << "GvvVolCurve fit failed: m_maturity " << m_maturity
                  << " volvol2 " << volvol2 << std::endl;
        return false;
    }
    double volvol = std::sqrt(volvol2);
    double temp = std::pow(atmvol, alpha + 1) * m_maturity;

    // BUG FIX: guard against negative radicand for spotvol (AGENTS.md)
    double spotvol2 = atmvol * atmvol - 0.5 * skew * temp + 0.25 * kurt * temp * temp;
    if (spotvol2 < 0.0) spotvol2 = 0.0;
    double spotvol = std::sqrt(spotvol2);

    // BUG FIX: guard against division by zero / negative for rho
    double kurt_sqrt = std::sqrt(std::max(0.0, kurt));
    double rho = (kurt_sqrt > FP_EPSILON) ? (0.5 * skew / spotvol / kurt_sqrt) : 0.0;
    if (rho < GVV_MINRHO || rho > GVV_MAXRHO) {
        std::cout << "GvvVolCurve fit failed: m_maturity " << m_maturity
                  << " rho " << rho << std::endl;
        return false;
    }

    // parameters updated upon every successful fit
    update_parameters(spotvol, rho, volvol, alpha);
    m_bInitialized = true;
    m_bLastFitOK = true;
    return true;
}

} // namespace wt_option
