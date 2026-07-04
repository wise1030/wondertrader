/*!
 * \file LinearVolCurve.cpp
 * \brief LinearVolCurve implementation (migrated from quantbox)
 *
 * Original longbeach code used boost::tuple/get<0>, BOOST_FOREACH,
 * longbeach::FP_EPSILON, LE/GE helpers. Migrated to std::pair/.first/.second,
 * range-for, and std::fabs comparisons with a local epsilon.
 */
#include "LinearVolCurve.h"
#include "OptionData.h"

#include <algorithm>
#include <iostream>
#include <cmath>

namespace wt_option {

namespace {
const double FP_EPSILON = 1.0e-9;
inline bool LE(double a, double b) { return a < b - FP_EPSILON; }
inline bool GE(double a, double b) { return a > b + FP_EPSILON; }
inline bool EQ(double a, double b) { return std::fabs(a - b) < FP_EPSILON; }

typedef std::pair<double, double> point_t;
bool comp(const point_t& a, const point_t& b)
{
    return a.first < b.first;
}
} // namespace

LinearVolCurve::LinearVolCurve(const LinearVolCurve::Config& c)
    : m_bInitialized(false)
    , m_extrapolateFlat(c.flat_extrapolation)
    , m_atmforward(NAN)
    , m_maturity(NAN)
{
    if (!c.points.empty())
    {
        dataset_t pts;
        for (const auto& v : c.points)
        {
            pts.push_back(make_datapoint(v.first, v.second));
        }
        fit(pts);
    }
}

LinearVolCurve::LinearVolCurve()
    : m_bInitialized(false)
    , m_extrapolateFlat(false)
    , m_atmforward(NAN)
    , m_maturity(NAN)
{
}

LinearVolCurve::~LinearVolCurve()
{
}

double LinearVolCurve::eval(double xi) const
{
    if (!m_bInitialized)
        return 1.0;

    auto upper_point = m_points.begin();

    if (LE(xi, m_minXpt.first)) // left wing extrapolation
    {
        if (!std::isnan(m_maturity) && LE(m_maturity, 1.0 / 252))
            // To be safe, flat extrapolation on expiration day for expiring month
            return m_minXpt.second;
        if (m_extrapolateFlat)
            return m_minXpt.second;

        auto lower = m_points.begin();
        auto upper = lower; ++upper;
        double xa = lower->first;  double ya = lower->second;
        double xb = upper->first;  double yb = upper->second;
        double lower_weight = std::fabs(xb - xa) < FP_EPSILON ? 1.0 : (xb - xi) / (xb - xa);
        double v = lower_weight * ya + (1.0 - lower_weight) * yb;
        v = std::min(LINEAR_MAXVOL, std::max(LINEAR_MINVOL, v));
        return v;
    }
    else if (GE(xi, m_maxXpt.first)) // right wing extrapolation
    {
        if (!std::isnan(m_maturity) && LE(m_maturity, 1.0 / 252))
            return m_maxXpt.second;
        if (m_extrapolateFlat)
            return m_maxXpt.second;

        auto upper = m_points.end(); --upper;
        auto lower = upper; --lower;
        double xa = lower->first;  double ya = lower->second;
        double xb = upper->first;  double yb = upper->second;
        double lower_weight = std::fabs(xb - xa) < FP_EPSILON ? 1.0 : (xb - xi) / (xb - xa);
        double v = lower_weight * ya + (1.0 - lower_weight) * yb;
        v = std::min(LINEAR_MAXVOL, std::max(LINEAR_MINVOL, v));
        return v;
    }
    else // linear interpolation
    {
        auto upper = std::upper_bound(m_points.begin(), m_points.end(),
                                      point_t(xi, 0), comp);
        auto lower = upper; --lower;
        double xa = lower->first;  double ya = lower->second;
        double xb = upper->first;  double yb = upper->second;
        double lower_weight = std::fabs(xb - xa) < FP_EPSILON ? 1.0 : (xb - xi) / (xb - xa);
        double v = lower_weight * ya + (1.0 - lower_weight) * yb;
        v = std::min(LINEAR_MAXVOL, std::max(LINEAR_MINVOL, v));
        return v;
    }
}

double LinearVolCurve::operator()(const OptionData& od, double atmforward) const
{
    double xi = od.getStrike() - m_atmforward; // sticky strike vol
    return eval(xi);
}

double LinearVolCurve::operator()(double diff) const
{
    return eval(diff);
}

bool LinearVolCurve::fit(const dataset_t& src_pts)
{
    m_points = src_pts;
    std::sort(m_points.begin(), m_points.end(), comp);

    m_minXpt = m_points.front();
    m_maxXpt = m_points.back();

    m_bInitialized = true;
    return true;
}

} // namespace wt_option
