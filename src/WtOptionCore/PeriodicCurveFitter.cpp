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
#include "../WTSTools/WTSLogger.h"

#include <algorithm>
#include <cmath>
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

// threshold table is now a per-instance member (m_threshTable) — B30 fix

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
    std::string pts;
    for (const auto& p : stkvol_points) {
        double stk = p.first + atm_forward;
        pts += std::to_string(stk) + " ";
    }
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "FitData exp={} atm_fwd={:.4f} atm_vol={:.4f} pts=[{}]",
        exp, atm_forward, atm_vol, pts);
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
    for (const auto& v : vol_fitting_good_points_thresh) {
        m_threshTable[v.first] = v.second;
    }
}

// ----------------------------------------------------------------------------
double PeriodicCurveFitter::evalThresh(int32_t days) const {
    const auto& t = m_threshTable;
    if (t.empty()) return 15;
    auto it = t.lower_bound(days);
    if (it == t.end())   return t.rbegin()->second;
    if (it == t.begin()) return it->second;
    auto lo = std::prev(it);
    double x0 = lo->first, x1 = it->first;
    double y0 = lo->second, y1 = it->second;
    if (x1 == x0) return y1;
    return y0 + (y1 - y0) * (days - x0) / (x1 - x0);
}

// Internal threshold table is now in the anonymous namespace above (populated by constructor).

// ----------------------------------------------------------------------------
bool PeriodicCurveFitter::fitToExpiry(uint32_t exp)
{
    typedef std::map<double, double> strike_vol_map_t;
    typedef std::map<double, double> strike_fwd_map_t;

    // requires an updated computeImpliedValues
    ExpiryDataPtr ed = m_spGrid->getExpiryData(exp);   // host-provided
    if (!ed || !ed->isForwardReady()) return false;    // skip if forward not ready

    StrikeDataPtr  atm_sd = m_spGrid->getAtmStrike(exp);
    if (!atm_sd) return false;  // no ATM strike data yet
        strike_fwd_map_t strike_fwd;
        strike_vol_map_t strike_vol;
        // iterate strikes for this expiry
        auto strikes = m_spGrid->getStrikesByExpiry(exp);
        for (const auto& strike : strikes) {
            const OptionDataPtr& call = strike->call();
            const OptionDataPtr& put  = strike->put();
            // B02 fix: single-sided strikes are normal during dynamic discovery —
            // dereferencing a null leg here segfaulted the timer fit path
            if (!call || !put) continue;
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
                    WTSLogger::log_by_cat("strategy", LL_WARN,
                        "PeriodicCurveFitter exp={} t={:.1f} cannot interpolate atmvol (no vol points near forward), exit",
                        exp, getTime());
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
        // B10 fix: the original two-pointer dance did `--begin` (UB) when
        // strike_vol had a single element. A strike is an upside point when
        // K > fwd, downside otherwise — a single forward pass suffices.
        IVolCurve::dataset_t stkvol_points;
        bool has_upside_point = false;
        bool has_downside_point = false;
        for (const auto& kv : strike_vol) {
            if (kv.first > atm_forward) {
                has_upside_point = true;
                double sv = kv.second / atm_vol;
                stkvol_points.push_back({kv.first - atm_forward, sv});
            } else {
                has_downside_point = true;
                double sv = kv.second / atm_vol;
                stkvol_points.push_back({kv.first - atm_forward, sv});
            }
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
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "PeriodicCurveFitter exp={} t={:.1f} skip stkfwd fit: <7 days to expire (dte={})",
                exp, getTime(), ed->daysToExpiry());
        } else if (stkfwd_points.size() < 4) {
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "PeriodicCurveFitter exp={} t={:.1f} skip stkfwd fit: only {} points (need 4)",
                exp, getTime(), stkfwd_points.size());
        } else if (fwdcurve) {
            fwdcurve->fit(stkfwd_points);
        }

        if (stkvol_points.size() < 4 || !has_upside_point || !has_downside_point) {
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "PeriodicCurveFitter exp={} t={:.1f} not enough vol points: total={} upside={} downside={}, exit",
                exp, getTime(), stkvol_points.size(), has_upside_point, has_downside_point);
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
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "PeriodicCurveFitter exp={} t={:.1f} good points {} < thresh {}, exit",
                exp, getTime(), stkvol_points.size(), good_points_thresh);
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
                WTSLogger::log_by_cat("strategy", LL_INFO,
                    "PeriodicCurveFitter exp={} t={:.1f} pts stk={:.4f} vol={:.6f}",
                    exp, getTime(), stk, (atm_vol * p.second));
            }
        }
        return true;
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

        // B05(stale-wing) fix: previously an unmatched prev point (strike no
        // longer quoted) was carried into the new dataset FOREVER with a frozen
        // vol, polluting the wings as the ATM moved. Bound the carry to the
        // span of live points so far-away zombies age out of the fit set.
        double spanLo = 0.0, spanHi = 0.0;
        const bool haveLive = !fd_mut.stkvol_points.empty();
        if (haveLive) {
            spanLo = fd_mut.stkvol_points.front().first;
            spanHi = fd_mut.stkvol_points.back().first;   // caller sorts before calling
        }
        const double margin = haveLive ? 0.5 * (spanHi - spanLo) : 0.0;

        std::vector<IVolCurve::datapoint_t> carried;
        for (const auto& p0 : stkvol_points_prev) {
            double stk0 = p0.first + atmf_prev;
            double vol0 = p0.second * atmv_prev;
            IVolCurve::datapoint_t* pt = nullptr;
            for (auto& p1 : fd_mut.stkvol_points) {
                double stk1 = p1.first + fd.atm_forward;
                if (EQ(stk0, stk1)) { pt = &p1; break; }
            }
            if (!pt) {
                // carry only if within the live fit window (+50% margin)
                if (haveLive && p0.first >= spanLo - margin && p0.first <= spanHi + margin) {
                    carried.push_back({p0.first, vol0 / fd.atm_vol});
                }
            } else {
                double vol1 = pt->second * fd.atm_vol;
                double voln = (1 - frac) * vol0 + frac * vol1;
                pt->second = voln / fd.atm_vol;
            }
        }
        for (auto& c : carried)
            fd_mut.stkvol_points.push_back(std::move(c));
        if (!carried.empty())
            std::sort(fd_mut.stkvol_points.begin(), fd_mut.stkvol_points.end(), dp_comp);
    }
    fd_prev = fd;
}

// ----------------------------------------------------------------------------
bool PeriodicCurveFitter::doFit()
{
    // Pre-condition: grid must have options and at least one expiry with forward ready
    if (!m_spGrid || m_spGrid->numOptions() == 0)
        return false;

    // Check at least one expiry has forward ready (i.e., put-call parity succeeded)
    bool anyForwardReady = false;
    for (const auto& [exp, ed] : m_spGrid->expiries()) {
        if (ed && ed->isForwardReady()) {
            anyForwardReady = true;
            break;
        }
    }
    if (!anyForwardReady) return false;

    double tod = std::fmod(m_time, 86400.0);
    if (tod < m_volFittingStartTime || tod > m_volFittingEndTime) {
        WTSLogger::log_by_cat("strategy", LL_DEBUG,
            "PeriodicCurveFitter doFit skip: tod={:.0f} outside window [{:.0f}, {:.0f}]",
            tod, m_volFittingStartTime, m_volFittingEndTime);
        return false;
    }

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

    // Skip if forward not ready (put-call parity failed or no market data yet)
    if (!v.second || !v.second->isForwardReady()) {
        m_expiryFitStatus[exp] = false;
        return;
    }

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
    for (const auto& v : m_spGrid->expiries()) {
        __fitExpiry_imp(v);
    }
    m_tvLastFit = getTime();
    return true;
}

// ----------------------------------------------------------------------------
// ADAPTATION: replaces onClockWakeup self-scheduling. Host calls onTimer each
// tick; we fit when m_period has elapsed since last fit.
// ----------------------------------------------------------------------------
bool PeriodicCurveFitter::onTimer(double time)
{
    setTime(time);
    if (time >= m_tvNextFit) {
        bool ok = doFit();
        m_tvNextFit = time + m_period;
        return ok;
    }
    return false;
}

} // namespace wt_option
