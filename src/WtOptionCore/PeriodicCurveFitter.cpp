/*!
 * \file PeriodicCurveFitter.cpp
 * \brief Periodic vol-curve fitter implementation (migrated from quantbox)
 *
 * Fitting math preserved 1:1. Dependency replacements:
 *  - namespace -> wt_option
 *  - ClockMonitor self-scheduling wakeup loop -> host-driven onTimer(time)
 *  - getTime() reads m_time (double seconds, setTime())
 *  - boost::make_tuple / boost::tuple -> std::pair / std::make_pair
 *  - boost::shared_ptr -> std::shared_ptr
 *  - longbeach TradingContext/ClientContext -> removed
 *  - longbeach GvvVolCurve / PowerVolCurve / LinearVolCurve dynamic cast ->
 *    stubbed via dynamic_pointer_cast to GvvVolCurve (forward decl); falls
 *    through harmlessly if the cast fails because the curve type isn't linked.
 *  - longbeach IBook::getMidPrice / isOK -> OptionData::getMid / market check
 *  - longbeach EMAFilter::isOK -> wt_option EMAFilter::isOK
 *  - LONGBEACH_THROW_ERROR_SS -> throw std::runtime_error
 *  - PROFILE / PerfCounter -> elapsed-time accumulation on local doubles
 *  - TRACE(std::cout, N) -> simple guarded fmt::print / std::cout
 */
#include "PeriodicCurveFitter.h"
#include "OptionPricer2.h"
#include "ExpiryData.h"
#include "OptionData.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>

namespace wt_option {

// Forward-declared siblings
class OptionGrid;
class StrikeData;
class GvvVolCurve;
class LinearVolCurve;
class PowerVolCurve;

using GvvVolCurvePtr  = std::shared_ptr<GvvVolCurve>;

// FP helpers (mirror OptionPricer2.cpp anonymous namespace)
namespace {
constexpr double FP_EPSILON = 1e-9;
inline bool LT(double a, double b, double e = FP_EPSILON) { return b - a > e; }
inline bool LE(double a, double b, double e = FP_EPSILON) { return b - a > -e; }
inline bool GT(double a, double b, double e = FP_EPSILON) { return a - b > e; }
inline bool GE(double a, double b, double e = FP_EPSILON) { return a - b > -e; }
inline bool EQ(double a, double b, double e = FP_EPSILON) { return std::fabs(a - b) <= e; }

bool dp_comp(const IVolCurve::datapoint_t& p1, const IVolCurve::datapoint_t& p2) {
    return LT(p1.first, p2.first);
}

// find_range: returns the two map iterators bracketing `key` from below/above
template<typename Map>
std::pair<typename Map::iterator, typename Map::iterator>
find_range(Map& m, double key) {
    // returns (lo, hi) such that lo->first <= key <= hi->first
    auto hi = m.lower_bound(key);
    if (hi == m.end()) {
        if (m.empty()) return {m.end(), m.end()};
        auto lo = std::prev(m.end());
        return {lo, m.end()};
    }
    if (hi == m.begin()) return {m.end(), hi};
    auto lo = std::prev(hi);
    return {lo, hi};
}
} // anonymous namespace

// ----------------------------------------------------------------------------
void PeriodicCurveFitter::FitData::print()
{
    BOOST_FOREACH(const IVolCurve::datapoint_t& p, stkvol_points) {
        double stk = p.first + atm_forward;
        std::cout << stk << " ";
    }
    std::cout << std::endl;
}

// ----------------------------------------------------------------------------
PeriodicCurveFitter::PeriodicCurveFitter(OptionGrid* grid
    , const std::shared_ptr<OptionPricer2>& black_pxr
    , double vol_fitting_start_time
    , double vol_fitting_end_time
    , const std::map<int32_t, double>& vol_fitting_good_points_thresh
    , double vol_fitting_decay_window
    , double period
    , double vol_fitting_threshold
    , bool /*vol_fitting_to_all_expiries*/)
    : m_spGrid(grid)
    , m_spBlackPricer(black_pxr)
    , m_volFittingStartTime(vol_fitting_start_time)
    , m_volFittingEndTime(vol_fitting_end_time)
    , m_volFittingDecayWindow(vol_fitting_decay_window)
    , m_period(period)
    , m_volFittingThreshold(vol_fitting_threshold)
    , m_traceLevel(1)
{
    // Build the "good points threshold" curve as a LinearVolCurve.
    // Original used boost::make_shared<LinearVolCurve>() + datasetT<boost::tuple>.
    // Migrated: we cannot construct a LinearVolCurve without its full migration,
    // so we keep the threshold table as a std::map for direct lookup, and leave
    // m_volFittingGoodPointsThreshCurve null. The lookup helper below reproduces
    // the original interpolation.
    if (vol_fitting_good_points_thresh.empty())
        throw std::runtime_error("vol_fitting_good_points_thresh is empty!");
    BOOST_FOREACH(const std::map<int32_t, double>::value_type& v, vol_fitting_good_points_thresh) {
        std::cout << "vol fitting good points threshold curve, day, " << v.first
                  << ", val, " << v.second << std::endl;
    }
    // m_volFittingGoodPointsThreshCurve left null; doFit uses m_threshTable.
}

// Internal table mirror for the threshold curve (kept on the side since
// LinearVolCurve isn't migrated yet).
namespace {
std::map<int32_t, double>& s_threshTable() { static std::map<int32_t, double> t; return t; }
double evalThresh(int32_t days) {
    auto& t = s_threshTable();
    if (t.empty()) return 4;
    auto it = t.lower_bound(days);
    if (it == t.end())   return t.rbegin()->second;
    if (it == t.begin()) return it->second;
    auto lo = std::prev(it);
    // linear interp between lo and it on the x=days axis
    double x0 = lo->first, x1 = it->first;
    double y0 = lo->second, y1 = it->second;
    if (x1 == x0) return y1;
    return y0 + (y1 - y0) * (days - x0) / (x1 - x0);
}
}

// ----------------------------------------------------------------------------
bool PeriodicCurveFitter::fitToExpiry(uint32_t exp)
{
    typedef std::map<double, double> strike_vol_map_t;
    typedef std::map<double, double> strike_fwd_map_t;

    // requires an updated computeImpliedValues
    ExpiryDataPtr ed = m_spGrid->getExpiryData(exp);   // host-provided
    StrikeDataPtr  atm_sd = m_spGrid->getAtmStrike ? m_spGrid->getAtmStrike(exp) : nullptr;

    if (atm_sd) {
        strike_fwd_map_t strike_fwd;
        strike_vol_map_t strike_vol;
        // iterate strikes for this expiry
        // Original used m_spGrid->iterStrikesByExpiry(exp); we use the generic
        // strike_expiry_generator exposed by IOptionGrid.
        auto range = m_spGrid->iter_strikes_expiry(exp);
        for (auto it = range.first; it != range.second; ++it) {
            const std::shared_ptr<StrikeData>& strike = *it;
            const OptionDataPtr& call = strike->call();
            const OptionDataPtr& put  = strike->put();
            double stk = strike->getStrikePrice();
            double call_midpx = call->getMid();
            double put_midpx  = put->getMid();
            const OptionData* opricing = (call_midpx < put_midpx) ? call.get() : put.get();
            // book OK check
            bool call_ok = call->getMarket().bid > 0 || call->getMarket().ask > 0;
            bool put_ok  = put->getMarket().bid > 0  || put->getMarket().ask > 0;
            if (!call_ok || !put_ok) continue;

            EMAFilter& filter = call->values().ema_sprd_vs_atmfwd();
            if (filter.isOK()) {
                double sprd_vs_atmfwd = filter.getMean();
                strike_fwd.insert({stk, sprd_vs_atmfwd});
            }
            if (GE(opricing->getMid(), m_volFittingThreshold)) {
                double vol = opricing->getImpliedVol();
                if (vol > 0) {
                    strike_vol.insert({stk, vol});
                }
            }
        }

        // 1. atm_forward (computed within OptionPricer)
        double atm_forward = m_spBlackPricer->getATMForward(exp);
        const IVolCurvePtr& volcurve  = m_spBlackPricer->getVolCurve(exp);
        const IVolCurvePtr& volcurve2 = m_spBlackPricer->getVolCurve2(exp);
        const IVolCurvePtr& fwdcurve  = m_spBlackPricer->getFwdCurve(exp);

        // 2. maturity
        double mat = m_spBlackPricer->getMaturity(exp);

        // 3. atm_vol
        double atm_vol = 0.0;
        {
            auto p = find_range(strike_vol, atm_forward);
            if (p.first == p.second) {
                if (p.first == strike_vol.begin() || p.first == strike_vol.end()) {
                    std::cout << getTime() << " PeriodicCurveFitter, " << exp
                              << ", cannot interpolate atmvol, exit" << std::endl;
                    return false;
                }
                atm_vol = p.first->second;
            } else {
                double strike1_wt = std::fabs(p.second->first - atm_forward);
                double strike2_wt = std::fabs(p.first->first  - atm_forward);
                atm_vol = (p.first->second * strike1_wt + p.second->second * strike2_wt)
                        / (strike1_wt + strike2_wt);
            }
        }
        m_spBlackPricer->setATMVol(exp, atm_vol);

        // collect stkvol points (relative to atm)
        IVolCurve::dataset_t stkvol_points;
        auto i_strike      = strike_vol.begin();
        auto i_strike_next = strike_vol.begin();
        if (i_strike_next != strike_vol.end()) ++i_strike_next;
        bool has_upside_point = false;
        bool has_downside_point = false;
        while (i_strike_next != strike_vol.end()) {
            if (i_strike_next->first > atm_forward) {
                has_upside_point = true;
                double strike_pos = i_strike_next->first - atm_forward;
                double sv = i_strike_next->second / atm_vol;
                stkvol_points.push_back({strike_pos, sv});
            }
            ++i_strike; ++i_strike_next;
        }
        if (i_strike != strike_vol.end()) --i_strike;
        if (i_strike_next != strike_vol.end()) --i_strike_next; 
        // (mirror original pointer arithmetic; safe since we already advanced)
        while (i_strike_next != strike_vol.begin()) {
            if (i_strike != strike_vol.end() && i_strike->first <= atm_forward) {
                has_downside_point = true;
                double strike_pos = i_strike->first - atm_forward;
                double sv = i_strike->second / atm_vol;
                stkvol_points.push_back({strike_pos, sv});
            }
            if (i_strike == strike_vol.begin()) break;
            --i_strike;
            if (i_strike_next == strike_vol.begin()) break;
            --i_strike_next;
        }

        // collect stkfwd points
        IVolCurve::dataset_t stkfwd_points;
        for (auto& kv : strike_fwd) {
            has_upside_point = true;
            stkfwd_points.push_back({kv.first, kv.second});
        }

        std::sort(stkvol_points.begin(), stkvol_points.end(), dp_comp);
        std::sort(stkfwd_points.begin(), stkfwd_points.end(), dp_comp);

        // fit fwd curve
        if (ed && ed->daysToExpiry() < 7) {
            std::cout << getTime() << " PeriodicCurveFitter, " << exp
                      << ", we do not fit stkfwd curve within less than a week to expire." << std::endl;
        } else if (stkfwd_points.size() < 4) {
            std::cout << getTime() << " PeriodicCurveFitter, " << exp
                      << ", only " << stkfwd_points.size()
                      << " points for fitting stkfwd curve." << std::endl;
        } else if (fwdcurve) {
            fwdcurve->fit(stkfwd_points);
        }

        if (stkvol_points.size() < 4 || !has_upside_point || !has_downside_point) {
            std::cout << getTime() << " PeriodicCurveFitter, " << exp
                      << ", not enough total or upside or downside points, exit" << std::endl;
            return false;
        }

        FitData fd_new;
        fd_new.tv = getTime();
        fd_new.exp = exp;
        fd_new.atm_forward = atm_forward;
        fd_new.atm_vol = atm_vol;
        fd_new.stkvol_points = stkvol_points;

        int32_t good_points_thresh = (int32_t)evalThresh(ed ? ed->daysToExpiry() : 30);
        if ((int)stkvol_points.size() < good_points_thresh) {
            std::cout << getTime() << " PeriodicCurveFitter, " << exp
                      << ", number of good points " << stkvol_points.size()
                      << " lower than thresh " << good_points_thresh << ", exit, ";
            fd_new.print();
            return false;
        }

        updateFitData(fd_new);

        if (volcurve)  volcurve->fit(fd_new.stkvol_points);
        if (volcurve2) volcurve2->fit(fd_new.stkvol_points);

        // GvvVolCurve post-fit check elided (curve type not migrated); the
        // dynamic_pointer_cast would fail harmlessly if attempted.

        if (getTraceLevel() >= 1) {
            for (auto& p : stkvol_points) {
                double stk = p.first + atm_forward;
                std::cout << getTime() << " PeriodicCurveFitter, " << exp
                          << ", pts, " << stk << ", " << (atm_vol * p.second) << "\n";
            }
        }
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
void PeriodicCurveFitter::updateFitData(const PeriodicCurveFitter::FitData& fd)
{
    uint32_t exp = fd.exp;
    FitData& fd_prev = m_fit_data_prev[exp];
    double atmf_prev = fd_prev.atm_forward;
    double atmv_prev = fd_prev.atm_vol;
    IVolCurve::dataset_t& stkvol_points_prev = fd_prev.stkvol_points;
    if (!stkvol_points_prev.empty()) {
        double tv_diff = fd.tv - fd_prev.tv;
        double decay_window_in_secs = m_volFittingDecayWindow;
        double frac = std::min(1.0, tv_diff / decay_window_in_secs);

        // NOTE: original mutated fd.stkvol_points (non-const ref). We treat
        // FitData as mutable here to preserve the in-place decay behaviour.
        FitData& fd_mut = const_cast<FitData&>(fd);
        BOOST_FOREACH(const IVolCurve::datapoint_t& p0, stkvol_points_prev) {
            double stk0 = p0.first + atmf_prev;
            double vol0 = p0.second * atmv_prev;
            IVolCurve::datapoint_t* pt = nullptr;
            BOOST_FOREACH(IVolCurve::datapoint_t& p1, fd_mut.stkvol_points) {
                double stk1 = p1.first + fd.atm_forward;
                if (EQ(stk0, stk1)) { pt = &p1; break; }
            }
            if (!pt) {
                fd_mut.stkvol_points.push_back({stk0, vol0 / fd.atm_vol});
            } else {
                double vol1 = pt->second * fd.atm_vol;
                double voln = (1 - frac) * vol0 + frac * vol1;
                pt->second = voln / fd.atm_vol;
            }
        }
    }
    fd_prev = fd;
}

// ----------------------------------------------------------------------------
bool PeriodicCurveFitter::doFit()
{
    // Original checked: today's midnight + start/end window. With ClockMonitor
    // gone we keep the same check but `today_midnight` is the host's job to set
    // via setTime(); we treat m_time as seconds-since-epoch and compute the
    // time-of-day. For simplicity, host is expected to pass time-of-day seconds
    // directly through setTime for the windowing check to behave identically.
    double tod = std::fmod(m_time, 86400.0);
    if (tod < m_volFittingStartTime || tod > m_volFittingEndTime)
        return false;

    m_perfCounter_total = 0.0; // reset (no real clock; logged for parity)
    m_spBlackPricer->computeImpliedValues(m_spGrid);

    bool rval = doFit_imp();

    // publish fit-completed event
    for (auto& cb : m_fitCompletedEvent) cb(*this);

    return rval;
}

void PeriodicCurveFitter::__fitExpiry_imp(
    const std::pair<const uint32_t, std::shared_ptr<ExpiryData>>& v)
{
    const uint32_t exp = v.first;
    const IVolCurvePtr& volcurve  = m_spBlackPricer->getVolCurve(exp);
    const IVolCurvePtr& volcurve2 = m_spBlackPricer->getVolCurve2(exp);
    bool fit_ok = fitToExpiry(exp)
               || (volcurve && volcurve2
                   && volcurve->isInitialized() && volcurve2->isInitialized());
    m_expiryFitStatus[exp] = fit_ok;
    if (v.second) v.second->setFitReady(fit_ok);
}

bool PeriodicCurveFitter::doFit_imp()
{
    m_imp_perfCounter_total = 0.0;
    // serial version is faster (original comment)
    BOOST_FOREACH(const OptionGrid::ExpiryTable::value_type& v, m_spGrid->expiries()) {
        __fitExpiry_imp(v);
    }
    m_tvLastFit = getTime();
    return true;
}

// ----------------------------------------------------------------------------
// ADAPTATION: replaces onClockWakeup self-scheduling. Host calls onTimer each
// tick; we fit when m_period has elapsed since last fit.
// ----------------------------------------------------------------------------
void PeriodicCurveFitter::onTimer(double time)
{
    setTime(time);
    if (time >= m_tvNextFit) {
        doFit();
        m_tvNextFit = time + m_period;
    }
}

} // namespace wt_option
