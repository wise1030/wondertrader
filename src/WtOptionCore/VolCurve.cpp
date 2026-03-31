/*!
 * \file VolCurve.cpp
 * \brief Volatility curve implementations
 * 
 * Full GVV implementation migrated from longbeach/quantbox/strategy/optioncore/GvvVolCurve.cc
 * Includes Brent minimization and weighted linear regression.
 */

#include "VolCurve.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <iostream>
#include "../WTSTools/WTSLogger.h"

namespace wt_option {

//=============================================================================
// Brent Minimization (Golden Section Search)
//=============================================================================

namespace {

/**
 * @brief Golden section search for 1D minimization
 * 
 * Finds minimum of f in [low, high] starting from mid
 */
template<class F>
double BrentMinimize(double low, double mid, double high, double tolerance, 
                     size_t maxIt, const F& objectiveFunction)
{
    const double W = 0.5 * (3.0 - std::sqrt(5.0));  // Golden ratio reciprocal
    double x = W * low + (1 - W) * high;
    if (mid > low && mid < high)
        x = mid;
    
    double midValue = objectiveFunction(x);
    
    size_t iterations = 0;
    while (high - low > tolerance && iterations < maxIt) {
        if (x - low > high - x) {  // Left interval is bigger
            double tentativeNewMid = W * low + (1 - W) * x;
            double tentativeNewMidValue = objectiveFunction(tentativeNewMid);
            
            if (tentativeNewMidValue < midValue) {  // Go left
                high = x;
                x = tentativeNewMid;
                midValue = tentativeNewMidValue;
            } else {  // Go right
                low = tentativeNewMid;
            }
        } else {
            double tentativeNewMid = W * x + (1 - W) * high;
            double tentativeNewMidValue = objectiveFunction(tentativeNewMid);
            
            if (tentativeNewMidValue < midValue) {  // Go right
                low = x;
                x = tentativeNewMid;
                midValue = tentativeNewMidValue;
            } else {  // Go left
                high = tentativeNewMid;
            }
        }
        ++iterations;
    }
    return x;
}

/**
 * @brief Brent root-finding solver
 */
template<class F>
double BrentSolve(const F& f, double tol, double guess, double low, double high, int maxEval)
{
    double a = low, b = high;
    double fa = f(a), fb = f(b);
    
    if (fa * fb > 0) {
        // Try to find a bracket
        double fg = f(guess);
        if (fa * fg < 0) {
            b = guess; fb = fg;
        } else if (fb * fg < 0) {
            a = guess; fa = fg;
        } else {
            return guess;  // No bracket found
        }
    }
    
    double c = a, fc = fa;
    double d = b - a, e = d;
    
    for (int iter = 0; iter < maxEval; ++iter) {
        if (fb * fc > 0) {
            c = a; fc = fa;
            d = e = b - a;
        }
        if (std::abs(fc) < std::abs(fb)) {
            a = b; b = c; c = a;
            fa = fb; fb = fc; fc = fa;
        }
        
        double tol1 = 2.0 * 1e-15 * std::abs(b) + 0.5 * tol;
        double xm = 0.5 * (c - b);
        
        if (std::abs(xm) <= tol1 || fb == 0) {
            return b;
        }
        
        if (std::abs(e) >= tol1 && std::abs(fa) > std::abs(fb)) {
            double s = fb / fa;
            double p, q;
            if (a == c) {
                p = 2.0 * xm * s;
                q = 1.0 - s;
            } else {
                q = fa / fc;
                double r = fb / fc;
                p = s * (2.0 * xm * q * (q - r) - (b - a) * (r - 1.0));
                q = (q - 1.0) * (r - 1.0) * (s - 1.0);
            }
            if (p > 0) q = -q;
            p = std::abs(p);
            
            double min1 = 3.0 * xm * q - std::abs(tol1 * q);
            double min2 = std::abs(e * q);
            
            if (2.0 * p < std::min(min1, min2)) {
                e = d;
                d = p / q;
            } else {
                d = xm;
                e = d;
            }
        } else {
            d = xm;
            e = d;
        }
        
        a = b;
        fa = fb;
        
        if (std::abs(d) > tol1) {
            b += d;
        } else {
            b += (xm > 0 ? tol1 : -tol1);
        }
        fb = f(b);
    }
    
    return b;
}

/**
 * @brief Price error function for GVV vol solve
 */
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

}  // anonymous namespace

//=============================================================================
// ConstantVolCurve implementation
//=============================================================================

bool ConstantVolCurve::fit(const DataSet& points) {
    if (points.empty()) return false;
    
    double sum = 0;
    for (const auto& pt : points) {
        sum += std::get<1>(pt);
    }
    m_vol = sum / points.size();
    m_initialized = true;
    return true;
}

//=============================================================================
// LinearVolCurve implementation
//=============================================================================

double LinearVolCurve::operator()(double x) const {
    return m_a + m_b * x;
}

double LinearVolCurve::getVol(double strike, double atmForward) const {
    if (atmForward <= 0) return m_a;
    double moneyness = strike / atmForward - 1.0;
    return m_a + m_b * moneyness;
}

bool LinearVolCurve::fit(const DataSet& points) {
    if (points.size() < 2) {
        if (points.size() == 1) {
            m_a = std::get<1>(points[0]);
            m_b = 0;
            m_initialized = true;
            return true;
        }
        return false;
    }
    
    double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;
    size_t n = points.size();
    
    for (const auto& pt : points) {
        double x = std::get<0>(pt);
        double y = std::get<1>(pt);
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumX2 += x * x;
    }
    
    double denom = n * sumX2 - sumX * sumX;
    if (std::abs(denom) < EPSILON) {
        m_a = sumY / n;
        m_b = 0;
    } else {
        m_b = (n * sumXY - sumX * sumY) / denom;
        m_a = (sumY - m_b * sumX) / n;
    }
    
    m_initialized = true;
    return true;
}

//=============================================================================
// GvvVolCurve implementation
//=============================================================================

GvvVolCurve::GvvVolCurve()
    : m_beta(0.0)
    , m_atmForward(NAN)
    , m_maturity(NAN)
    , m_atmVol(GVV_MINATMVOL)
    , m_spotvol(0.2)
    , m_rho(0.0)
    , m_volvol(GVV_MINVOLVOL)
    , m_alpha(1.0)
    , m_alphaLow(0.5)
    , m_alphaHigh(1.5)
    , m_initialized(false)
    , m_lastFitOK(false)
{
}

GvvVolCurve::GvvVolCurve(double beta, double alphaLow, double alphaHigh)
    : m_beta(beta)
    , m_atmForward(NAN)
    , m_maturity(NAN)
    , m_atmVol(GVV_MINATMVOL)
    , m_spotvol(0.2)
    , m_rho(0.0)
    , m_volvol(GVV_MINVOLVOL)
    , m_alpha(0.5 * (alphaLow + alphaHigh))
    , m_alphaLow(std::max(GVV_MINALPHA, alphaLow))
    , m_alphaHigh(std::min(GVV_MAXALPHA, alphaHigh))
    , m_initialized(false)
    , m_lastFitOK(false)
{
}

double GvvVolCurve::getParameter(Parameter param) const {
    switch (param) {
        case SPOTVOL: return m_spotvol;
        case RHO: return m_rho;
        case VOLVOL: return m_volvol;
        case ALPHA: return m_alpha;
        case ATMVOL: return m_atmVol;
        default: return 0;
    }
}

void GvvVolCurve::setParameter(Parameter param, double value) {
    switch (param) {
        case SPOTVOL:
            m_spotvol = std::max(GVV_MINATMVOL, std::min(GVV_MAXATMVOL, value));
            break;
        case RHO:
            m_rho = std::max(GVV_MINRHO, std::min(GVV_MAXRHO, value));
            break;
        case VOLVOL:
            m_volvol = std::max(GVV_MINVOLVOL, std::min(GVV_MAXVOLVOL, value));
            break;
        case ALPHA:
            m_alpha = std::max(GVV_MINALPHA, std::min(GVV_MAXALPHA, value));
            break;
        case ATMVOL:
            m_atmVol = std::max(GVV_MINATMVOL, std::min(GVV_MAXATMVOL, value));
            break;
    }
}

void GvvVolCurve::updateParameters(double spotvol, double rho, double volvol, double alpha) {
    m_spotvol = spotvol;
    m_rho = rho;
    m_volvol = volvol;
    m_alpha = alpha;
}

double GvvVolCurve::getVolBump(Parameter param, double paramBump, double strike) {
    double x = std::log(strike / m_atmForward);
    double v0 = eval(x);
    
    if (param == SPOTVOL) {
        return paramBump;
    }
    
    double param0 = getParameter(param);
    setParameter(param, param0 + paramBump);
    double v = eval(x);
    setParameter(param, param0);  // Reset
    return m_atmVol * (v - v0);
}

double GvvVolCurve::eval(double x) const {
    if (!m_initialized) {
        return 1.0;
    }
    
    try {
        // Calculate ATM vol from model parameters
        // From GVV model: atmvol = solve for v in v^2 = spotvol^2 + rho*spotvol*volvol*T
        double a = (1.0 + 0.25 * m_volvol * m_volvol);
        double b = -m_rho * m_spotvol * m_volvol;
        double c = -m_spotvol * m_spotvol;
        double atmvol = (-b + std::sqrt(b * b - 4 * a * c)) / (2 * a);
        
        // Calculate skew and kurtosis
        double kurt = m_volvol * m_volvol / m_maturity / std::pow(atmvol, 2.0 * m_alpha);
        double skew = 2.0 * m_rho * m_spotvol * std::sqrt(kurt);
        
        // Solve for vol at log-moneyness x
        PriceError f(x, atmvol, skew, kurt, m_alpha);
        double guess = (GVV_MINATMVOL + GVV_MAXATMVOL) / 2.0;
        double vol = BrentSolve(f, 1.0e-4, guess, GVV_MINATMVOL, GVV_MAXATMVOL, 100);
        
        return vol / m_atmVol;  // Return normalized by ATM vol
    }
    catch (...) {
        return GVV_MINATMVOL / m_atmVol;
    }
}

double GvvVolCurve::operator()(double x) const {
    return eval(x);
}

double GvvVolCurve::getVol(double strike, double atmForward) const {
    if (!m_initialized || atmForward <= 0) {
        return m_atmVol;
    }
    
    double x = std::log(strike / m_atmForward);  // Use stored ATM forward (sticky strike)
    return eval(x) * m_atmVol;
}

void GvvVolCurve::weightedLinearFit(const std::vector<double>& X1,
                                     const std::vector<double>& X2,
                                     const std::vector<double>& Y,
                                     const std::vector<double>& W,
                                     double* c0, double* c1, double* c2,
                                     double* chisq)
{
    // Weighted least squares: min sum(w_i * (y_i - c0 - c1*x1_i - c2*x2_i)^2)
    // Normal equations: X'WX * c = X'WY
    
    size_t n = Y.size();
    
    // Compute X'WX (3x3 matrix)
    double s00 = 0, s01 = 0, s02 = 0;
    double s11 = 0, s12 = 0, s22 = 0;
    double b0 = 0, b1 = 0, b2 = 0;
    
    for (size_t i = 0; i < n; ++i) {
        double w = W[i];
        double x1 = X1[i];
        double x2 = X2[i];
        double y = Y[i];
        
        s00 += w * 1.0 * 1.0;
        s01 += w * 1.0 * x1;
        s02 += w * 1.0 * x2;
        s11 += w * x1 * x1;
        s12 += w * x1 * x2;
        s22 += w * x2 * x2;
        
        b0 += w * y * 1.0;
        b1 += w * y * x1;
        b2 += w * y * x2;
    }
    
    // Solve 3x3 system using Cramer's rule or direct inversion
    // Matrix: [s00 s01 s02]   [c0]   [b0]
    //         [s01 s11 s12] * [c1] = [b1]
    //         [s02 s12 s22]   [c2]   [b2]
    
    double det = s00 * (s11 * s22 - s12 * s12)
               - s01 * (s01 * s22 - s12 * s02)
               + s02 * (s01 * s12 - s11 * s02);
    
    if (std::abs(det) < 1e-15) {
        *c0 = b0 / s00;
        *c1 = 0;
        *c2 = 0;
        *chisq = 1e10;
        return;
    }
    
    // Compute inverse and multiply by RHS
    double inv00 = (s11 * s22 - s12 * s12) / det;
    double inv01 = -(s01 * s22 - s12 * s02) / det;
    double inv02 = (s01 * s12 - s11 * s02) / det;
    double inv11 = (s00 * s22 - s02 * s02) / det;
    double inv12 = -(s00 * s12 - s02 * s01) / det;
    double inv22 = (s00 * s11 - s01 * s01) / det;
    
    *c0 = inv00 * b0 + inv01 * b1 + inv02 * b2;
    *c1 = inv01 * b0 + inv11 * b1 + inv12 * b2;
    *c2 = inv02 * b0 + inv12 * b1 + inv22 * b2;
    
    // Compute chi-squared
    double chi = 0;
    for (size_t i = 0; i < n; ++i) {
        double resid = Y[i] - (*c0 + *c1 * X1[i] + *c2 * X2[i]);
        chi += W[i] * resid * resid;
    }
    *chisq = chi;
}

double GvvVolCurve::fitWithAlpha(double alpha, const DataSet& points,
                                  double* atmvol2, double* skew, double* kurt)
{
    size_t n = points.size();
    
    std::vector<double> Y(n), X1(n), X2(n), W(n);
    
    for (size_t i = 0; i < n; ++i) {
        double stki = std::get<0>(points[i]) + m_atmForward;  // points[i] is (diff, vol)
        double xi = std::log(stki / m_atmForward);  // log-moneyness
        double vi = std::get<1>(points[i]) * m_atmVol;  // denormalized vol
        double temp = xi * std::pow(vi, alpha - 1);
        
        Y[i] = vi * vi;
        X1[i] = temp;
        X2[i] = temp * temp;
        
        // Weight: exp(-beta * x^2 / (atmVol^2 * T))
        W[i] = std::exp(-m_beta * xi * xi / (m_atmVol * m_atmVol * m_maturity));
    }
    
    double chisq;
    weightedLinearFit(X1, X2, Y, W, atmvol2, skew, kurt, &chisq);
    
    return chisq;
}

bool GvvVolCurve::fit(const DataSet& srcPoints) {
    m_lastFitOK = false;
    
    if (srcPoints.size() < 4) {
        WTSLogger::log_by_cat("strategy", LL_WARN, "GvvVolCurve fit failed: less than 4 points");
        return false;
    }
    
    if (m_atmVol < GVV_MINATMVOL || m_atmVol > GVV_MAXATMVOL) {
        WTSLogger::log_by_cat("strategy", LL_WARN, fmt::format("GvvVolCurve fit failed: atmVol={}", m_atmVol).c_str());
        return false;
    }
    
    if (std::isnan(m_maturity) || m_maturity <= 0) {
        WTSLogger::log_by_cat("strategy", LL_WARN, fmt::format("GvvVolCurve fit failed: maturity={}", m_maturity).c_str());
        return false;
    }
    
    if (std::isnan(m_atmForward) || m_atmForward <= 0) {
        WTSLogger::log_by_cat("strategy", LL_WARN, fmt::format("GvvVolCurve fit failed: atmForward={}", m_atmForward).c_str());
        return false;
    }
    
    // Sort points by moneyness
    DataSet points = srcPoints;
    std::sort(points.begin(), points.end(), [](const DataPoint& a, const DataPoint& b) {
        return std::get<0>(a) < std::get<0>(b);
    });
    
    // Optimize alpha using Brent minimization
    double atmvol2, skew, kurt;
    
    auto objective = [this, &points, &atmvol2, &skew, &kurt](double alpha) {
        return this->fitWithAlpha(alpha, points, &atmvol2, &skew, &kurt);
    };
    
    double optAlpha = BrentMinimize(
        m_alphaLow,      // low
        m_alpha,         // mid (initial guess)
        m_alphaHigh,     // high
        1.0e-3,          // tolerance
        100,             // maxIt
        objective
    );
    
    // Final fit with optimal alpha
    objective(optAlpha);
    
    // Validate atmvol^2
    if (atmvol2 < GVV_MINATMVOL * GVV_MINATMVOL || atmvol2 > GVV_MAXATMVOL * GVV_MAXATMVOL) {
        WTSLogger::log_by_cat("strategy", LL_WARN, fmt::format("GvvVolCurve fit failed: atmvol2={}", atmvol2).c_str());
        return false;
    }
    double atmvol = std::sqrt(atmvol2);
    
    // Calculate volvol
    double volvol2 = kurt * m_maturity * std::pow(atmvol, 2.0 * optAlpha);
    if (volvol2 < GVV_MINVOLVOL * GVV_MINVOLVOL || volvol2 > GVV_MAXVOLVOL * GVV_MAXVOLVOL) {
        WTSLogger::log_by_cat("strategy", LL_WARN, fmt::format("GvvVolCurve fit failed: volvol2={}", volvol2).c_str());
        return false;
    }
    double volvol = std::sqrt(volvol2);
    
    // Calculate spotvol
    double temp = std::pow(atmvol, optAlpha + 1) * m_maturity;
    double spotvol = std::sqrt(atmvol * atmvol - 0.5 * skew * temp + 0.25 * kurt * temp * temp);
    
    // Calculate rho
    double rho = 0.5 * skew / spotvol / std::sqrt(kurt);
    if (rho < GVV_MINRHO || rho > GVV_MAXRHO) {
        WTSLogger::log_by_cat("strategy", LL_WARN, fmt::format("GvvVolCurve fit failed: rho={}", rho).c_str());
        return false;
    }
    
    // Update parameters
    updateParameters(spotvol, rho, volvol, optAlpha);
    m_initialized = true;
    m_lastFitOK = true;
    
    return true;
}

} // namespace wt_option
