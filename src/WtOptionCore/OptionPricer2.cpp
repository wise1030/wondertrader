/*!
 * \file OptionPricer2.cpp
 * \brief Black76 / GVV option pricer implementation (migrated from quantbox)
 *
 * Business logic preserved 1:1. Only dependency-replacements applied:
 *  - namespace -> wt_option
 *  - longbeach ClockMonitor getTime() -> m_time (double seconds, setTime())
 *  - IPriceProvider removed; ExpiryInfo reads ExpiryData::getForward()/getMaturity()
 *  - boost::shared_ptr/make_shared -> std::
 *  - BOOST_FOREACH preserved (wondertrader has boost)
 *  - tbb parallel_for/bind preserved (wondertrader has tbb)
 *  - longbeach::trading::getBestMarket / get_real_best / MarketLevel -> inline helpers
 *  - boost::math::isnan -> std::isnan
 *  - QuantLib Option::Call/Put -> wt_option OT_Call/OT_Put
 *  - TRACE(std::cout, N) macro -> simple fmt::print guarded by traceLevel
 *  - MySQL db read in updateGvvParams removed (no db dep); stubbed
 *  - notifiable<double> -> plain double
 *  - LONGBEACH_THROW_ERROR_SS -> throw std::runtime_error
 *  - round_to_precision / round_to_nearest_integer / round_to_tick / FP_EPSILON
 *    -> inline local helpers
 */
#include "OptionPricer2.h"
#include "BlackCalc.h"
#include "BlackImpliedCalculator.h"
#include "PowerVolCurve.h"
#include "OptionGrid.h"
#include "WTSLogger.h"
#include "StrikeData.h"
#include "ExpiryData.h"
#include "OptionData.h"
#include "OptionValues.h"
#include "OptionGreeks.h"
#include "GvvVolCurve.h"
#include "LinearVolCurve.h"
#include "PeriodicCurveFitter.h"

#include <limits>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <algorithm>
#include <iostream>

#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>
#include <boost/bind.hpp>
#include <boost/ref.hpp>

// #include <tbb/parallel_for.h>  // Replaced by OpenMP
#include <omp.h>

// Forward declarations for sibling types (definitions pulled in as needed)
namespace wt_option {
class OptionGrid;          // see IOptionGrid.h / OptionGrid.h
class OptionRisk;          // OptionRisk.h
class StrikeData;          // StrikeData.h
class ExpiryData;          // ExpiryData.h
class OptionData;          // OptionData.h

// Helpers replacing longbeach utilities ------------------------------------------------
namespace {

// Floating point comparison helpers (replaces longbeach fpmath GT/GE/LT/LE/EQ/EQZ/NE)
constexpr double FP_EPSILON = 1e-9;
inline bool EQ (double a, double b, double eps = FP_EPSILON) { return std::fabs(a - b) <= eps; }
inline bool EQZ(double a, double eps = FP_EPSILON)          { return std::fabs(a) <= eps; }
inline bool NE (double a, double b, double eps = FP_EPSILON) { return !EQ(a, b, eps); }
inline bool GT (double a, double b, double eps = FP_EPSILON) { return a - b >  eps; }
inline bool GE (double a, double b, double eps = FP_EPSILON) { return a - b > -eps; }
inline bool LT (double a, double b, double eps = FP_EPSILON) { return b - a >  eps; }
inline bool LE (double a, double b, double eps = FP_EPSILON) { return b - a > -eps; }

inline double round_to_precision(double v, double prec) {
    if (prec <= 0) return v;
    return std::round(v / prec) * prec;
}
inline int32_t round_to_nearest_integer(double v) { return (int32_t)std::lround(v); }
// timeval_diff in seconds (longbeach duration_t.total_seconds())
inline double timeval_diff(double a, double b) { return a - b; }

} // anonymous namespace
} // namespace wt_option

namespace wt_option {

// ----------------------------------------------------------------------------
// Config ctor
// ----------------------------------------------------------------------------
OptionPricer2Config::OptionPricer2Config() = default;

// ----------------------------------------------------------------------------
// P1-b: shared curve factory (also used by AsyncVolFitter for fitted curves)
// ----------------------------------------------------------------------------
IVolCurvePtr createVolCurveByType(const std::string& type, int powerParams,
                                  double gvvBeta, double gvvAlphaDn,
                                  double gvvAlphaUp)
{
    // 2026-11-26: whitelist is curve1 = gvv|power, curve2 = linear; unknown
    // strings fall through to GVV (same defensive default as before).
    if (type == "linear")
        return std::make_shared<LinearVolCurve>();
    if (type == "power")
        return std::make_shared<PowerVolCurve>(static_cast<GvvVolCurve::Parameter>(powerParams));
    GvvVolCurve::Config cfg;
    cfg.beta = gvvBeta;
    // Audit 11-22 (round 3): optional alpha search-range override (<0 = default)
    if (gvvAlphaDn >= 0.0) cfg.alpha_dn = gvvAlphaDn;
    if (gvvAlphaUp >= 0.0) cfg.alpha_up = gvvAlphaUp;
    return std::make_shared<GvvVolCurve>(cfg);
}

// ----------------------------------------------------------------------------
// ExpiryInfo
// ----------------------------------------------------------------------------
OptionPricer2::ExpiryInfo::ExpiryInfo(uint32_t exp)
    : m_expiry(exp)
    , m_atmforward(NAN)
    , m_atmvol(0.0)
    , m_maturity(0.0)
    , m_settleFrac(0.0)
    , m_futsprd(0.0)
    , m_atmvolsprd(0.0)
    , m_prop_atmv(0.0)
    , m_prop_atmfwd(0.0)
{
}

void OptionPricer2::ExpiryInfo::setATMV(double atmv)
{
    m_atmvol = atmv;
    m_prop_atmv = round_to_precision(atmv, 0.0001);
    // Propagate to vol curves (critical for GVV fit: m_atmvol scales vi and volvol2)
    if (m_spVolCurve)  m_spVolCurve->setATMVol(atmv);
    if (m_spVolCurve2) m_spVolCurve2->setATMVol(atmv);
}

void OptionPricer2::ExpiryInfo::setATMVReseed(double atmv)
{
    // Bootstrap scale-correction (2026-11-26 scale-race fix): the anchor move
    // corrects OUR estimate of the ATM vol — the market did not move — so the
    // curves' stored normalizations are re-based by old/new to keep the
    // ABSOLUTE vol they encode unchanged.  GVV is anchor-invariant (eval
    // divides by the live anchor) and its rescale() is a no-op; linear/power
    // store normalized multipliers and would otherwise be silently scaled by
    // the anchor change (the exact mechanism of the 2026-11-26 21:05
    // explosion).  The periodic re-anchor path deliberately does NOT re-base:
    // there the anchor intentionally follows the market between fits.
    const double old = m_atmvol;
    if (old > 0.0 && atmv > 0.0 && std::fabs(atmv - old) > 1e-12) {
        const double factor = old / atmv;
        if (m_spVolCurve)  m_spVolCurve->rescale(factor);
        if (m_spVolCurve2) m_spVolCurve2->rescale(factor);
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "ATM bootstrap reseed exp={} {:.4f} -> {:.4f} (curves re-based by {:.4f})",
            m_expiry, old, atmv, factor);
    }
    setATMV(atmv);
}

void OptionPricer2::ExpiryInfo::computeForwardPrice(OptionGrid* grid, const ExpiryDataPtr& ed)
{
    // 通过 grid->getAtmForward 触发 __getBestSyntheticPrice 初始化 forward
    // 与 quantbox 一致: grid->getAtmForward(expiry) 而非直接读 ed->getForward()
    if (grid && ed) {
        m_atmforward = grid->getAtmForward(ed->getExpiry());
    } else if (ed) {
        m_atmforward = ed->getForward();
    }
    if (!std::isnan(m_atmforward)) {
        if (m_spVolCurve)  m_spVolCurve->setATMForward(m_atmforward);
        if (m_spVolCurve2) m_spVolCurve2->setATMForward(m_atmforward);
        m_prop_atmfwd = m_atmforward;
    } else {
        m_prop_atmfwd = 0;
    }
}

void OptionPricer2::ExpiryInfo::computeMaturity(const ExpiryData* ed)
{
    // ADAPTATION: read ExpiryData::getMaturity() and getSettlementFraction() directly
    if (ed) {
        m_maturity   = ed->getMaturity();
        m_settleFrac = ed->getSettlementFraction();
    }
    if (m_spVolCurve)  m_spVolCurve->setMaturity(m_maturity);
    if (m_spVolCurve2) m_spVolCurve2->setMaturity(m_maturity);
}

// ----------------------------------------------------------------------------
// Constructor / destructor
// ----------------------------------------------------------------------------
OptionPricer2::OptionPricer2(const OptionPricer2Config& c,
                             OptionGrid* grid,
                             OptionRisk* risk)
    : m_config(c)
    , m_grid(grid)
    , m_spPositionRisk(risk)
    , m_defaultVolatility(0.1)
    , m_prop_atmsig(0.0)
    , m_bReprice(true)
{
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "OptionPricer2 created (fitter={}, start={:.0f}, end={:.0f})",
        config().enable_fitter, config().vol_fitting_start_time, config().vol_fitting_end_time);

    // 创建 PeriodicCurveFitter (与 quantbox 一致, 使用配置参数)
    if (config().enable_fitter && m_grid) {
        auto self = std::shared_ptr<OptionPricer2>(this, [](OptionPricer2*){});
        // Use config values; fall back to sensible defaults if not set
        double startTime  = config().vol_fitting_start_time;
        double endTime    = config().vol_fitting_end_time;
        double decayWin   = config().vol_fitting_decay_window > 0 ? config().vol_fitting_decay_window : 300.0;
        double period     = config().vol_fitting_period > 0 ? config().vol_fitting_period : 60.0;
        double threshold  = config().vol_fitting_threshold;
        bool   fitAll     = config().vol_fitting_to_all_expiries;
        // Default good-points thresh if not configured
        auto threshMap = config().vol_fitting_good_points_thresh;
        if (threshMap.empty()) threshMap = {{0, 15}};

        m_spFitter = std::make_shared<PeriodicCurveFitter>(
            m_grid, self,
            startTime, endTime,
            threshMap,
            decayWin,
            period,
            threshold,
            fitAll
        );
        m_spFitter->setTraceLevel(config().trace_level);
    }

    setTraceLevel(config().trace_level);
    // m_spPositionRisk->setOptionPricer(this);  // host responsibility
}

OptionPricer2::~OptionPricer2() = default;

// ----------------------------------------------------------------------------
// Curve / expiry accessors
// ----------------------------------------------------------------------------
IVolCurvePtr OptionPricer2::getVolCurve(uint32_t exp) const  { return getExpiryInfo(exp)->m_spVolCurve; }
IVolCurvePtr OptionPricer2::getVolCurve2(uint32_t exp) const { return getExpiryInfo(exp)->m_spVolCurve2; }
IVolCurvePtr OptionPricer2::getFwdCurve(uint32_t exp) const  { return getExpiryInfo(exp)->m_spFwdCurve; }
double OptionPricer2::getMaturity(uint32_t exp) const        { return getExpiryInfo(exp)->m_maturity; }
void   OptionPricer2::setATMVol(uint32_t exp, double v)      { getExpiryInfo(exp)->setATMV(v); }
double OptionPricer2::getATMVol(uint32_t exp) const          { return getExpiryInfo(exp)->m_atmvol; }
double OptionPricer2::getATMForward(uint32_t exp) const      { return getExpiryInfo(exp)->m_atmforward; }
double OptionPricer2::getFutSprd(uint32_t exp) const         { return getExpiryInfo(exp)->m_futsprd; }
double OptionPricer2::getATMVolSprd(uint32_t exp) const      { return getExpiryInfo(exp)->m_atmvolsprd; }

const OptionPricer2::ExpiryInfoPtr& OptionPricer2::getExpiryInfo(uint32_t exp) const
{
    ExpiryInfoPtr& ei = m_expiryInfoTable[exp];
    if (!ei)
    {
        ei = std::make_shared<ExpiryInfo>(exp);
        ei->m_atmforward = NAN;
        ei->m_atmvol = m_defaultVolatility;
        // 创建波动率曲线 (与 quantbox 一致, 但不依赖 Lua config)
        // P1-b: honor volcurve_type for the initial main curve (fitted
        // replacement comes from AsyncVolFitter / PeriodicCurveFitter).
        ei->m_spVolCurve  = createVolCurveByType(m_config.volcurve_type,
                                                 0, m_config.volcurve_beta,
                                                 m_config.volcurve_alpha_dn,
                                                 m_config.volcurve_alpha_up);
        // R2: restore the original GVV×Power dual-curve design. With the
        // migration-era default ("gvv") both curves are identical and
        // volcurve_weight degenerates to a no-op.
        ei->m_spVolCurve2 = createVolCurveByType(m_config.curve2_type,
                                                 GvvVolCurve::RHO, m_config.volcurve_beta,
                                                 m_config.volcurve_alpha_dn,
                                                 m_config.volcurve_alpha_up);
        // hardcode defaults (preserve original)
        ei->m_futsprd = 1.0;
        ei->m_atmvolsprd = 0.2;
        updateGvvParams(exp, ei->m_spVolCurve);
    }
    return ei;
}

void OptionPricer2::publishFittedCurves(const std::vector<FittedCurveSet>& fitted)
{
    for (const auto& f : fitted) {
        if (f.version == 0 && f.version2 == 0) continue;
        if (!(f.atmVol > 0) || !std::isfinite(f.atmVol)) continue;
        uint64_t& version  = m_curveVersion[f.expiry];
        uint64_t& version2 = m_curve2Version[f.expiry];
        ExpiryInfoPtr ei = getExpiryInfo(f.expiry);

        bool updated = false;
        // Re-base the ATM anchor instead of rejecting (2026-11-26): a
        // bootstrap reseed or re-anchor step may land between submit and
        // publish.  The curve was fitted with normalization f.atmVol; keeping
        // the LIVE anchor `cur` and scaling the stored multipliers by
        // f.atmVol/cur leaves the absolute vol they encode unchanged (GVV is
        // anchor-invariant — rescale() is a no-op for it).  Reverting the
        // anchor to the snapshot value would silently undo a re-anchor step.
        const double cur = (ei->m_atmvol > 0 && std::isfinite(ei->m_atmvol))
            ? ei->m_atmvol : f.atmVol;

        if (f.volCurve && f.version > version) {
            if (std::fabs(f.atmVol - cur) > 1e-12) f.volCurve->rescale(f.atmVol / cur);
            ei->m_spVolCurve = f.volCurve;
            ei->m_spVolCurve->setATMVol(cur);
            ei->m_spVolCurve->setATMForward(ei->m_atmforward);
            ei->m_spVolCurve->setMaturity(ei->m_maturity);
            version = f.version;
            updated = true;
        }
        if (f.volCurve2 && f.version2 > version2) {
            if (std::fabs(f.atmVol - cur) > 1e-12) f.volCurve2->rescale(f.atmVol / cur);
            ei->m_spVolCurve2 = f.volCurve2;
            ei->m_spVolCurve2->setATMVol(cur);
            ei->m_spVolCurve2->setATMForward(ei->m_atmforward);
            ei->m_spVolCurve2->setMaturity(ei->m_maturity);
            version2 = f.version2;
            updated = true;
        }
        if (!updated) continue;
        // Keep the expiry anchor at the live value (the curves were re-based
        // onto it); only adopt the snapshot value when there was none.
        ei->m_atmvol = cur;
        ei->m_prop_atmv = round_to_precision(cur, 0.0001);

        ++m_publishCount[f.expiry];
        m_publishTime[f.expiry] = m_grid ? m_grid->getComputeTime() : 0.0;

        // Dynamic readiness (L4): curves ready = required curves available
        // for the CURRENT weight && startup-confirm satisfied.  (Freshness is
        // re-checked every compute pass in initValuesCompute; here the set is
        // fresh by construction.)
        refreshCurvesReady(f.expiry);

        // A successful accepted publish is the proof that live market points
        // were sufficient to fit — lift the strategy-owned drought/session
        // block (as before the curvesReady split).  Drought re-blocks stay
        // the strategy's responsibility on subsequent submits.
        if (m_grid) {
            ExpiryDataPtr ed = m_grid->getExpiryData(f.expiry);
            if (ed) {
                if (!ed->isFitReady())
                    ed->setFitReady(true);
                if (ed->isCurvesReady()) {
                    WTSLogger::log_by_cat("strategy", LL_INFO,
                        "MarketReadiness: expiry {} curves ready — publish v{}/v{} "
                        "(main={}, curve2={}, weight={:.2f}, publish #{})",
                        f.expiry, version, version2,
                        f.volCurve ? "ok" : "absent",
                        f.volCurve2 ? "ok" : "absent",
                        getGvvWeight(f.expiry), m_publishCount[f.expiry]);
                } else {
                    WTSLogger::log_by_cat("strategy", LL_WARN,
                        "MarketReadiness: expiry {} published but NOT ready "
                        "(required curve missing for weight={:.2f} or startup-confirm "
                        "{}/{} pending)",
                        f.expiry, getGvvWeight(f.expiry), m_publishCount[f.expiry],
                        m_config.volcurve_startup_confirm_versions);
                }
            }
        }
    }
}

uint64_t OptionPricer2::getCurveVersion(uint32_t exp) const
{
    auto it = m_curveVersion.find(exp);
    return it == m_curveVersion.end() ? 0 : it->second;
}

uint64_t OptionPricer2::getCurve2Version(uint32_t exp) const
{
    auto it = m_curve2Version.find(exp);
    return it == m_curve2Version.end() ? 0 : it->second;
}

uint64_t OptionPricer2::getAnyCurveVersion(uint32_t exp) const
{
    return std::max(getCurveVersion(exp), getCurve2Version(exp));
}

double OptionPricer2::getGvvWeight(uint32_t exp) const
{
    auto it = m_gvv_weight.find(exp);
    return it != m_gvv_weight.end() ? it->second : m_config.volcurve_weight;
}

void OptionPricer2::setATMVolReseed(uint32_t exp, double v)
{
    getExpiryInfo(exp)->setATMVReseed(v);
}

// ----------------------------------------------------------------------------
// Dynamic readiness (L4, 2026-11-26): a single static fitReady flag cannot
// express "the curves THIS weight actually prices with are available".  The
// weight is hot-adjustable (SETPARAM volcurve_weight), so readiness is
// re-evaluated on every publish, every weight change, and every compute pass
// (freshness), and stored in ExpiryData::curvesReady — the market-readiness
// drought flag (fitReady) stays strategy-owned and orthogonal.
// ----------------------------------------------------------------------------
bool OptionPricer2::hasRequiredCurves(uint32_t exp) const
{
    const double w = std::max(0.0, std::min(1.0, getGvvWeight(exp)));
    const ExpiryInfoPtr& ei = getExpiryInfo(exp);
    const bool mainOk = (w > 0.0)
        ? (ei->m_spVolCurve && ei->m_spVolCurve->isInitialized()
           && getCurveVersion(exp) > 0)
        : true;   // weight=0: main curve is a gate-only mirror, not priced
    const bool c2Ok = (w < 1.0)
        ? (ei->m_spVolCurve2 && ei->m_spVolCurve2->isInitialized()
           && getCurve2Version(exp) > 0)
        : true;   // weight=1: curve2 not priced
    return mainOk && c2Ok;
}

void OptionPricer2::refreshCurvesReady(uint32_t exp)
{
    if (!m_grid) return;
    ExpiryDataPtr ed = m_grid->getExpiryData(exp);
    if (!ed) return;

    // Before ANY accepted publish this flag is not ours to gate: pre-publish
    // readiness is the strategy-owned fitReady domain (and tests may preset
    // it).  Gating starts once a curve history exists.
    auto cntIt = m_publishCount.find(exp);
    const uint64_t cnt = cntIt != m_publishCount.end() ? cntIt->second : 0;
    if (cnt == 0) return;

    bool ready = hasRequiredCurves(exp);

    // Startup-confirm: an expiry must accumulate N accepted publishes before
    // its first unlock — one poisoned snapshot can never arm quoting alone.
    const int32_t confirm = std::max<int32_t>(1, m_config.volcurve_startup_confirm_versions);
    if (ready && cnt < static_cast<uint64_t>(confirm))
        ready = false;

    // Freshness: an accepted publish older than volcurve_max_age (compute
    // clock) stops pricing — a stale smile must not quote forever.
    if (ready && m_config.volcurve_max_age > 0) {
        auto tIt = m_publishTime.find(exp);
        if (tIt != m_publishTime.end()) {
            const double now = m_grid->getComputeTime();
            if (now > tIt->second && now - tIt->second > m_config.volcurve_max_age)
                ready = false;
        }
    }

    if (ed->isCurvesReady() != ready) {
        ed->setCurvesReady(ready);
        WTSLogger::log_by_cat("strategy", ready ? LL_INFO : LL_WARN,
            "MarketReadiness: expiry {} curves {} (weight={:.2f}, publishes={}, "
            "confirm={}, maxAge={:.0f}s)",
            exp, ready ? "READY" : "not ready", getGvvWeight(exp), cnt,
            confirm, m_config.volcurve_max_age);
    }
}

void OptionPricer2::setGvvWeight(uint32_t exp, double wgt)
{
    m_gvv_weight[exp] = wgt;
    // Hot weight change → immediate readiness re-evaluation (0→1 with a
    // never-fitted main curve must drop the expiry out of quoting at once).
    refreshCurvesReady(exp);
}

// ----------------------------------------------------------------------------
// updateGvvParams — DB read removed (no longbeach db dep). Stub kept so the
// host can override later. Original logic preserved in comments.
// ----------------------------------------------------------------------------
void OptionPricer2::updateGvvParams(uint32_t exp, IVolCurvePtr curve) const
{
    (void)exp;
    (void)curve;
    // Original fetched GVV params (spotvol/rho/volvol/alpha) from a MySQL table
    // sandbox.vol_curve filtered by date/sym/exp. With longbeach db removed,
    // parameter loading is now the host's responsibility (e.g. read from a
    // config file at startup). Curve-specific GvvVolCurve setParameter calls
    // would go here once GvvVolCurve is fully migrated.
}

// ----------------------------------------------------------------------------
// computeValues (FAST/SLOW scheduling) — preserved verbatim modulo getTime()
// ----------------------------------------------------------------------------
void OptionPricer2::__doComputeValueVec(const std::vector<OptionData*>& v, int32_t i)
{
    OptionData* od = v[i];
    computeValue(od);
    decayGreeks(od);
}

void OptionPricer2::__doComputeValueStrikeVec(const std::vector<StrikeData*>& v, double refpx, int32_t i)
{
    StrikeData* sd = v[i];
    strike_t spx = sd->getStrikePrice();
    // pair otm and itm options to apply put-call parity
    const OptionDataPtr& otm = spx < refpx ? sd->put()  : sd->call();
    const OptionDataPtr& itm = spx < refpx ? sd->call() : sd->put();
    if (!otm || !itm) return;  // skip single-sided strikes during dynamic discovery
    __computeValue(otm.get(), itm.get());
    decayGreeks(otm.get());
    decayGreeks(itm.get());
}

bool OptionPricer2::computeValues(OptionGrid* grid)
{
    if (getTime() == m_tvLastCompute)
        return true;
    m_tvLastCompute = getTime();

    // ADAPTATION: read underlying price directly from grid (no IPriceProvider).
    // IOptionGrid exposes getUnderlyingPrice(); host sets it.
    double refPrice = grid->getUnderlyingPrice();
    if (getTraceLevel() >= 0) {
        // os-style trace removed; nothing required here
    }
    if (refPrice <= std::numeric_limits<double>::min()) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "Warning: bad underlying price: {:.6f}", refPrice);
        return false;
    }

    m_perfCounter.start();
    initValuesCompute(grid);

    double tv_diff = timeval_diff(getTime(), m_tvLastSlowCompute);
    m_bReprice = m_bReprice || (tv_diff > config().slow_compute_interval);
    if (!m_bReprice) {
        // fast update without repricing greeks
        for (const auto& od : grid->getAllOptions()) {
            updateTheoreticalValues(od.get());
            decayGreeks(od.get());
        }
        finalizeCompute(grid);
        m_perfCounter.stop();
        return true;
    } else {
        m_bReprice = false;
        m_tvLastSlowCompute = getTime();
    }

    {
        if (config().use_parallel_for)
        {
            // ensure expiry infos exist
            for (const auto& v : grid->expiries()) {
                getExpiryInfo(v.second->getExpiry());
            }
            std::vector<StrikeData*> strikes;
            strikes.reserve(grid->numStrikes());
            for (const auto& strike : grid->getAllStrikes()) {
                strikes.push_back(strike.get());
            }
            #pragma omp parallel for schedule(dynamic, 4)
            for (long long i = 0; i < (long long)strikes.size(); ++i) {
                __doComputeValueStrikeVec(strikes, refPrice, (size_t)i);
            }
        }
        else
        {
            for (const auto& sd : grid->getAllStrikes())
            {
                OptionDataPtr otm = sd->getStrikePrice() < refPrice ? sd->put()  : sd->call();
                OptionDataPtr itm = sd->getStrikePrice() < refPrice ? sd->call() : sd->put();
                if (!otm || !itm) continue;  // skip single-sided strikes during dynamic discovery
                otm->values(getValuesIndex()).setPriced(false);
                itm->values(getValuesIndex()).setPriced(false);
                computeValue(otm.get());
                computeValue(itm.get());
                decayGreeks(otm.get());
                decayGreeks(itm.get());
            }
        }
        finalizeCompute(grid);
    }

    if (m_spFitter && getTime() > m_spFitter->getLastFitTime()) {
        m_spFitter->setTime(getTime());
        bool fitOk = m_spFitter->doFit();
        if (fitOk) {
            WTSLogger::log_by_cat("strategy", LL_INFO, "doFit success expiry count updated");
        }
    }
    m_perfCounter.stop();
    return true;
}

// ----------------------------------------------------------------------------
// BlackCalculatorInfo setup (theoretical forward, maturity, discount)
// ----------------------------------------------------------------------------
const OptionPricer2::BlackCalculatorInfoPtr&
OptionPricer2::__setupBlackCalculatorInfo(OptionData* option)
{
    // Audit 2026-11-13 (v2, T1-5): the original side-table keyed BCI by raw
    // OptionData* and never evicted it. OptionGrid::purgeExpired erases
    // contracts on every session begin, so the map kept dangling keys and
    // grew without bound; and if the allocator ever reused an address, the
    // stale ExpiryInfo/strike/type would silently price a DIFFERENT option.
    // The cache only ever saved one getExpiryInfo() map lookup plus two
    // assignments — negligible on a hash map. De-keying to a single
    // thread-local instance removes the entire lifetime-bug class at zero
    // practical cost and is OpenMP-safe (one instance per thread).
    static thread_local BlackCalculatorInfoPtr s_bci = std::make_shared<BlackCalculatorInfo>();
    BlackCalculatorInfoPtr& bci = s_bci;
    bci->ei = getExpiryInfo(option->getExpiry());
    bci->m_optionType = (option->getRight() == OR_Call) ? OT_Call : OT_Put;
    bci->m_strike = option->getStrike();
    const ExpiryDataPtr& ed = option->getExpiryData();
    const ExpiryInfo& ei = *(bci->ei);
    // P1-⑤ (audit 2026-11-18): price on the SAME forward the vol curve is
    // fitted/looked-up on (ei.m_atmforward, the grid's weighted parity pool).
    // The old per-strike parity spread (m_atmforward + __forwardSpreadFor)
    // put the Black forward and the vol-lookup moneyness on two different
    // coordinates that disagreed by 1700-4000 points on simnow's wide quotes
    // (per-strike parity noise ±600-1300), mis-locating the skew per strike.
    // The grid's own ema_sprd_vs_atmfwd signal keeps the per-strike parity
    // information; __forwardSpreadFor stays available for diagnostics.
    bci->m_forward = ei.m_atmforward;
    bci->m_discount   = ed ? ed->getDiscountFactor() : 1.0;
    bci->m_maturity   = ei.m_maturity;
    bci->m_settleFrac = ei.m_settleFrac;
    return bci;
}

// implied-vol helpers ---------------------------------------------------------
// P1-c: single implementation of the per-strike forward-spread resolution
// (previously duplicated near-verbatim in __setupBlackCalculatorInfo and the
// FAST incremental path).
double OptionPricer2::__forwardSpreadFor(OptionData* option, double atmForward) const
{
    if (!config().use_sprd_vs_atmfwd)
        return 0.0;

    // 1) externally fitted forward-spread curve (primary path, previously a
    //    dead branch — kept for override/debug purposes)
    IVolCurvePtr fwdcurve = getFwdCurve(option->getExpiry());
    if (fwdcurve && fwdcurve->isInitialized())
        return (*fwdcurve)(option->getStrike());

    // 2) per-strike parity synthetic forwards captured by the grid
    const ExpiryDataPtr& ed = option->getExpiryData();
    if (ed) {
        const auto& pts = ed->synForwardPoints();
        if (pts.size() >= 2) {
            const double K = option->getStrike();
            // sorted by strike: flat extrapolation outside the range
            if (K <= pts.front().first) return pts.front().second - atmForward;
            if (K >= pts.back().first)  return pts.back().second - atmForward;
            auto hi = std::upper_bound(pts.begin(), pts.end(), K,
                [](double k, const std::pair<double, double>& p) { return k < p.first; });
            auto lo = std::prev(hi);
            const double x0 = lo->first, y0 = lo->second;
            const double x1 = hi->first, y1 = hi->second;
            const double w = (x1 - x0) > 1e-12 ? (K - x0) / (x1 - x0) : 0.0;
            return (y0 + (y1 - y0) * w) - atmForward;
        }
    }

    // 3) fallback: this option's own EMA of forward spread
    EMAFilter& filter = option->values().ema_sprd_vs_atmfwd();
    if (filter.isOK())
        return filter.getMean();

    return 0.0;
}

void OptionPricer2::__doImplied_imp(OptionData* od) {
    const std::shared_ptr<BlackCalculatorInfo>& bci = __setupBlackCalculatorInfo(od);
    computeImpliedValues(od, *bci);
}
void OptionPricer2::__doImplied(const OptionDataPtr& od) { __doImplied_imp(od.get()); }
void OptionPricer2::__doImpliedStrikes(const StrikeDataPtr& sd) {
    __doImplied_imp(sd->call().get());
    __doImplied_imp(sd->put().get());
}
void OptionPricer2::__doImpliedVec(const std::vector<OptionData*>& v, int32_t i) { __doImplied_imp(v[i]); }
void OptionPricer2::__doImpliedStrikeVec(const std::vector<StrikeData*>& v, int32_t i) {
    __doImplied_imp(v[i]->call().get());
    __doImplied_imp(v[i]->put().get());
}

bool OptionPricer2::computeImpliedValues(OptionGrid* grid)
{
    if (getTime() == m_tvLastComputeImplied)
        return true;
    m_tvLastComputeImplied = getTime();

    m_perfCounter.start();
    // initValuesCompute is called here because doFit() -> computeImpliedValues
    // may run BEFORE CompositeOptionPricer::computeValues (e.g., in on_timer).
    // Without it, IV calculation uses stale forward/maturity from previous cycle.
    // When called from CompositeOptionPricer::computeValues_SLOW (after init),
    // the guard m_tvLastComputeImplied prevents double-execution.
    initValuesCompute(grid);

    {
        std::vector<OptionData*> opts;
        opts.reserve(grid->numStrikes() * 2);
        for (const auto& o : grid->getAllOptions()) {
            opts.push_back(o.get());
        }
        // B34b fix: gate the IV parallel section on use_parallel_for like the
        // pricing loop — it used to run multi-threaded even with the option off
        #pragma omp parallel for schedule(dynamic, 4) if (m_config.use_parallel_for)
        for (long long i = 0; i < (long long)opts.size(); ++i) {
            __doImpliedVec(opts, (size_t)i);
        }
    }

    m_perfCounter.stop();
    return true;
}

// ----------------------------------------------------------------------------
// P1-④ (audit 2026-11-18): shared ATM-IV bootstrap collector.  Collects the
// implied vols of all legs within ±3% log-moneyness of the forward and returns
// their median; NAN when fewer than 3 valid IVs are available.  Used by the
// pre-first-fit scale bootstrap and by the periodic ATM re-anchor.
// ----------------------------------------------------------------------------
double OptionPricer2::__bootstrapMedianIV(const ExpiryDataPtr& ed, OptionGrid* grid)
{
    if (!ed || !grid) return NAN;
    const double fwd = ed->getForward();
    if (!(fwd > 0)) return NAN;

    std::vector<double> atmIvs;
    for (const auto& sd : grid->getStrikesByExpiry(ed->getExpiry()))
    {
        if (!sd) continue;
        const double K = sd->getStrikePrice();
        if (std::fabs(std::log(K / fwd)) > 0.03) continue;  // ±3% around forward
        for (const OptionData* leg : {sd->call().get(), sd->put().get()})
        {
            if (!leg) continue;
            const double iv = leg->values(getValuesIndex()).impliedVol();
            if (iv > 0) atmIvs.push_back(iv);
        }
    }
    if (atmIvs.size() < 3) return NAN;
    std::sort(atmIvs.begin(), atmIvs.end());
    return atmIvs[atmIvs.size() / 2];
}

// ----------------------------------------------------------------------------
// initValuesCompute
// ----------------------------------------------------------------------------
bool OptionPricer2::initValuesCompute(OptionGrid* grid)
{
    for (const auto& v : grid->expiries())
    {
        const ExpiryDataPtr& ed = v.second;
        const ExpiryInfoPtr& ei = getExpiryInfo(ed->getExpiry());
        ei->computeForwardPrice(grid, ed);
        ei->computeMaturity(ed.get());

        if (config().expire_greeks_window_bdays > 0
            && ei->m_maturity * 252 < config().expire_greeks_window_bdays)
        {
            double decay = (ei->m_maturity * 252.0 - 1.0) / config().expire_greeks_window_bdays;
            decay = std::max(0.0, std::min(1.0, decay));
            ed->setExpireGreeksFrac(decay);
        }

        // Market bootstrap for the FIRST fit of an expiry (approved design):
        // before any accepted fit the vol curve is the uninitialized default
        // (flat m_defaultVolatility = 0.1), so the stored Greeks that feed
        // the vol-fit point filter would be computed on a 10%-vol basis —
        // far below the market level — and the delta band would shrink below
        // the good-points gate forever (the fit never starts, nothing is ever
        // priced).  Seed m_atmvol from the median near-ATM mid IV (inverted
        // by computeImpliedValues on the previous pass) so the stored Greeks
        // are market-scale until the first fit publishes.  Only runs while no
        // curve version has been accepted; afterwards the fitted atmVol owns
        // the scale.
        // P1 (2026-11-18): the version==0 bootstrap window can be missed —
        // the async fitter publishes a first curve (with the flat 0.1 default
        // scale baked in) before any bootstrap pass lands, after which this
        // branch never runs again and the re-anchor crawls at max_step for
        // ~1h to reach the market (observed 09:57→10:55, 0.1000→0.1629 in
        // eleven 5% steps, theo systematically cheap all morning). Seed
        // whenever the scale is still at the default, regardless of version.
        // 2026-11-26: the seed goes through setATMVolReseed — the anchor move
        // corrects our scale estimate (the market did not move), so the
        // stored curve normalizations are re-based to keep the absolute vol
        // unchanged (this is the incident-mechanism fix; the median dataset
        // gate in AsyncVolFitter rejects the poisoned fit outright anyway).
        // Any accepted curve (main OR curve2) closes the bootstrap window.
        const bool stillDefaultScale =
            std::fabs(ei->m_atmvol - m_defaultVolatility) < 1e-6;
        if (ed->isForwardReady()
            && (getAnyCurveVersion(ed->getExpiry()) == 0 || stillDefaultScale))
        {
            // Seed the scale from the market before the first accepted fit.
            const double med = __bootstrapMedianIV(ed, grid);
            if (!std::isnan(med) && med >= 0.03 && med <= 1.0
                && std::fabs(med - ei->m_atmvol) > 1e-6)
                setATMVolReseed(ed->getExpiry(), med);
        }
        else if (getAnyCurveVersion(ed->getExpiry()) > 0 && ed->isForwardReady()
                 && config().atm_reanchor_period > 0)
        {
            // P1-④ (audit 2026-11-18): periodic ATM re-anchor.  The bootstrap
            // above only ran while curveVersion==0 and publishFittedCurves
            // writes the snapshot atmVol back verbatim, so the ATM scale froze
            // at first-fit market level (observed: 2701/2702 medDev −2~−4.6%,
            // wings −54~−79% while the market IV kept moving).  Every
            // atm_reanchor_period seconds walk the anchor toward the fresh
            // ±3% median market IV, capped at atm_reanchor_max_step (relative)
            // per re-anchor — a fit/anchor race with the async fitter can then
            // only step the anchor back by one bounded increment per publish
            // and self-corrects on the next period.
            const double now = grid ? grid->getComputeTime() : 0.0;
            const uint32_t exp = ed->getExpiry();
            auto it = m_lastReanchor.find(exp);
            if (it == m_lastReanchor.end())
            {
                // First sight after the first accepted fit: wait one full
                // period before the first re-anchor.
                m_lastReanchor[exp] = now;
            }
            else if (now - it->second >= config().atm_reanchor_period)
            {
                it->second = now;
                const double med = __bootstrapMedianIV(ed, grid);
                if (!std::isnan(med) && med >= 0.03 && med <= 1.0)
                {
                    const double cur = ei->m_atmvol;
                    const double delta = med - cur;
                    const double step = config().atm_reanchor_max_step * std::fabs(cur);
                    const double capped = (std::fabs(delta) > step)
                        ? (delta > 0.0 ? step : -step) : delta;
                    if (std::fabs(capped) > 1e-9)
                    {
                        setATMVol(exp, cur + capped);
                        WTSLogger::log_by_cat("strategy", LL_INFO,
                            "ATM re-anchor exp={} {:.4f} -> {:.4f} (med={:.4f})",
                            exp, cur, cur + capped, med);
                    }
                }
            }
        }

        // Freshness re-check (2026-11-26): every compute pass re-evaluates
        // curves-ready so a smile older than volcurve_max_age drops out of
        // pricing without waiting for a publish or a weight change.
        refreshCurvesReady(ed->getExpiry());
    }
    return true;
}

// ----------------------------------------------------------------------------
// computeValue / __computeValue (theoretical values via BlackCalc)
// ----------------------------------------------------------------------------
namespace {
void __computeTheoreticalValues(OptionData* option, OptionValues& other_values,
    const OptionPricer2::BlackCalculatorInfo* bci, int32_t idx)
{
    OptionValues& values = option->values(idx);
    values.setPriced(false);
    // fees: per-contract transaction fee from contract info (wired onto
    // OptionData by the strategy from stra_get_comminfo). Original read
    // option->getInstrumentMDContext()->getFees(mid).
    values.m_fees = option->getFee();
    bool bcompute = !std::isnan(bci->m_forward) && GT(bci->m_forward, 0)
                  && GE(bci->m_maturity, 0);
    if (!bcompute) {
        return;
    }

    if (EQ(bci->m_settleFrac, 1.0)) // do not apply put-call parity during settlement
    {
        if (other_values.isPriced()) {
            const double F = bci->m_forward;
            const double df = option->getExpiryData() ? option->getExpiryData()->getDiscountFactor() : 1.0;
            const double sign = option->getRight() == OR_Call ? 1.0 : -1.0;
            const double K = option->getStrike();
            values.setForward(F);
            double val = std::max(0.0, other_values.theo() + sign * df * (F - K));
            values.setTheo(val);
            values.greeks().delta() = other_values.greeks().delta() + sign * df;
            values.greeks().gamma() = other_values.greeks().gamma();
            values.greeks().vega()  = other_values.greeks().vega();
            values.greeks().theta() = other_values.greeks().theta();
            values.greeks().vanna() = other_values.greeks().vanna();
            values.greeks().volga() = other_values.greeks().volga();
            values.greeks().vegaTW() = other_values.greeks().vegaTW();
            values.setPriced(true);
            return;
        }
    }

    BlackCalc bc(bci->m_optionType, bci->m_strike, bci->m_forward,
                 values.m_theoVol * std::sqrt(bci->m_maturity), bci->m_discount);
    values.setForward(bci->m_forward);
    values.setTheo(std::max(0.0, bc.value()));

    values.greeks().delta() = bc.delta();
    values.greeks().gamma() = bc.gamma();
    values.greeks().vega()  = bc.vega(bci->m_maturity) / 100;
    values.greeks().theta() = std::min(values.theo(), bc.thetaPerDay(0, values.m_theoVol));
    values.greeks().vanna() = bc.vanna(bci->m_maturity) / 100;
    values.greeks().volga() = bc.volga(bci->m_maturity) / 10000;

    static const double one_week = 5.0 / 252;
    values.greeks().vegaTW() = values.greeks().vega() / std::sqrt(std::max(one_week, bci->m_maturity));
    values.setPriced(true);
}
} // anonymous namespace

void OptionPricer2::computeValue(OptionData* option)
{
    OptionValues& values = option->values(getValuesIndex());
    uint32_t exp = option->getExpiry();
    const ExpiryDataPtr& ed = option->getExpiryData();
    // P0 fix (2026-11-25): no forward, no fit — no theory. Before the first
    // GVV fit publishes (and during readiness droughts) the vol scale is the
    // flat default/bootstrap IV; pricing anyway produced garbage theos that
    // CTG quoted straight off. valuesReady = forwardReady && fitReady.
    if (!ed || !ed->isValuesReady()) {
        values.setPriced(false);
        return;
    }
    const ExpiryInfoPtr& ei = getExpiryInfo(exp);
    double atmvol = ei->m_atmvol;
    // v3-GARCH-lite (borrow plan §1): optional second ATM-vol anchor blended
    // with the fitted atmVol. garch_weight=0 (default) keeps the fitted value;
    // garchVol<=0 (estimator warming up) also falls back to the fitted value.
    if (m_garchWeight > 0 && m_garchVol > 0)
        atmvol = (1.0 - m_garchWeight) * atmvol + m_garchWeight * m_garchVol;
    double atmforward = ei->m_atmforward;
    IVolCurvePtr spVolCurve  = getVolCurve(exp);
    IVolCurvePtr spVolCurve2 = getVolCurve2(exp);
    double gvv_weight = 0.0;
    if (m_gvv_weight.count(exp))
        gvv_weight = std::max(0.0, std::min(1.0, m_gvv_weight[exp]));

    // 2026-11-26 weight short-circuit: only evaluate the curves the current
    // weight actually prices with.  Evaluating a weight-0 main curve could
    // yield inf/NaN whose 0*w term poisons theoVol (0*inf = NaN).
    double vol = atmvol, vol2 = atmvol;
    if (gvv_weight > 1e-12 && spVolCurve)
        vol = (*spVolCurve)(*option, atmforward) * atmvol;
    if (gvv_weight < 1.0 - 1e-12 && spVolCurve2)
        vol2 = (*spVolCurve2)(*option, atmforward) * atmvol;
    double theoVol = (gvv_weight <= 1e-12) ? vol2
                   : (gvv_weight >= 1.0 - 1e-12) ? vol
                   : gvv_weight * vol + (1.0 - gvv_weight) * vol2;
    if (!std::isfinite(theoVol)) {
        // Never turn a failed vol (NaN/Inf) into a 1% quote: that silently
        // prices every option off a fake vol and keeps sending orders.
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "OptionPricer2: non-finite theoVol for {} (gvv_weight={}), skip pricing",
            option->getCode(), gvv_weight);
        values.setPriced(false);
        return;
    }
    values.m_theoVol = std::max(0.01, theoVol);
    std::shared_ptr<BlackCalculatorInfo> bci = __setupBlackCalculatorInfo(option);

    // other side for put-call parity
    auto other_od = m_grid->get(exp, option->getStrike(),
        option->getRight() == OR_Call ? OR_Put : OR_Call);
    if (!other_od) { values.setPriced(false); return; }
    OptionValues& other_values = other_od->values(getValuesIndex());
    __computeTheoreticalValues(option, other_values, bci.get(), getValuesIndex());

    // Market-readiness: theo/Greeks ARE computed (the stored Greeks feed the
    // vol-fit point filter), but while the expiry's curve is still the
    // default/stale one — or the curves the CURRENT weight prices with are
    // missing/stale (curvesReady) — the values must not count as priced.
    if (ed && !ed->isValuesReady())
        values.setPriced(false);
}

void OptionPricer2::__computeValue(OptionData* otm, OptionData* itm)
{
    uint32_t exp = otm->getExpiry();
    const ExpiryDataPtr& ed = otm->getExpiryData();
    const ExpiryInfoPtr& ei = getExpiryInfo(exp);
    double atmvol = ei->m_atmvol;
    // v3-GARCH-lite (borrow plan §1): optional second ATM-vol anchor blended
    // with the fitted atmVol. garch_weight=0 (default) keeps the fitted value;
    // garchVol<=0 (estimator warming up) also falls back to the fitted value.
    if (m_garchWeight > 0 && m_garchVol > 0)
        atmvol = (1.0 - m_garchWeight) * atmvol + m_garchWeight * m_garchVol;
    double atmforward = ei->m_atmforward;
    IVolCurvePtr spVolCurve  = getVolCurve(exp);
    IVolCurvePtr spVolCurve2 = getVolCurve2(exp);
    double gvv_weight = 0.0;
    if (m_gvv_weight.count(exp))
        gvv_weight = std::max(0.0, std::min(1.0, m_gvv_weight[exp]));

    OptionValues& otmvals = otm->values(getValuesIndex());
    OptionValues& itmvals = itm->values(getValuesIndex());
    const BlackCalculatorInfoPtr& obci = __setupBlackCalculatorInfo(otm);
    const BlackCalculatorInfoPtr& ibci = __setupBlackCalculatorInfo(itm);
    otmvals.setPriced(false);
    itmvals.setPriced(false);

    // 2026-11-26 weight short-circuit (see computeValue): only evaluate the
    // curves the current weight actually prices with (0*w NaN guard).
    double vol = atmvol, vol2 = atmvol;
    if (gvv_weight > 1e-12 && spVolCurve)
        vol = (*spVolCurve)(*otm, atmforward) * atmvol;
    if (gvv_weight < 1.0 - 1e-12 && spVolCurve2)
        vol2 = (*spVolCurve2)(*otm, atmforward) * atmvol;
    double theoVol = (gvv_weight <= 1e-12) ? vol2
                   : (gvv_weight >= 1.0 - 1e-12) ? vol
                   : gvv_weight * vol + (1.0 - gvv_weight) * vol2;
    otmvals.m_theoVol = std::max(0.01, theoVol);
    itmvals.m_theoVol = std::max(0.01, theoVol);

    __computeTheoreticalValues(otm, itmvals, obci.get(), getValuesIndex());
    __computeTheoreticalValues(itm, otmvals, ibci.get(), getValuesIndex());

    // Market-readiness: see computeValue — compute but do not mark priced
    // while the expiry has no accepted fit, or the curves the CURRENT weight
    // prices with are missing/stale.
    if (ed && !ed->isValuesReady()) {
        otmvals.setPriced(false);
        itmvals.setPriced(false);
    }
}

bool OptionPricer2::updateTheoreticalValues(OptionData* option)
{
    uint32_t exp = option->getExpiry();
    const ExpiryInfoPtr& ei = getExpiryInfo(exp);
    if (ei->m_settleFrac < 1.0) {
        computeValue(option);
        return true;
    }
    OptionValues& values = option->values(getValuesIndex());
    if (!values.isPriced()) return false;

    // Market-readiness: a fitted curve is only trustworthy while the expiry
    // is fit-ready AND the curves the CURRENT weight prices with are
    // available/fresh (curvesReady).  Without this check the FAST path would
    // keep serving cached priced values (and thus quotes) off a stale/default
    // curve for up to a whole SLOW interval after a drought re-block or a hot
    // weight switch to a never-fitted main curve.
    const ExpiryDataPtr& ed = option->getExpiryData();
    if (ed && !ed->isValuesReady()) {
        values.setPriced(false);
        return false;
    }

    // P1-⑤ (audit 2026-11-18): FAST incremental path now uses the same
    // single forward coordinate as the full reprice / vol-lookup (see
    // __setupBlackCalculatorInfo).  The removed per-strike parity term was
    // the second half of the two-coordinate discrepancy (1700-4000 pts).
    double F = ei->m_atmforward;
    if (std::isnan(F) || EQ(F, 0)) return false;
    const double dF = F - values.forward();
    if (config().fast_full_reprice_forward_step > 0 &&
        std::fabs(dF) > config().fast_full_reprice_forward_step) {
        computeValue(option);
        return true;
    }

    // First-order delta update is the legacy behavior.  A second-order gamma
    // term materially improves FAST accuracy for index options and still costs
    // only a few multiplies compared with a full Black reprice.
    double theo = values.theo() + values.greeks().delta() * dF;
    if (config().fast_use_gamma_correction)
        theo += 0.5 * values.greeks().gamma() * dF * dF;
    values.setForward(F);
    values.setTheo(std::max(0.0, theo));
    values.greeks().delta() += values.greeks().gamma() * dF;
    return true;
}

void OptionPricer2::decayGreeks(OptionData* option)
{
    OptionValues& values = option->values(getValuesIndex());
    if (!values.isPriced()) return;
    uint32_t exp = option->getExpiry();
    ExpiryInfoPtr ei = getExpiryInfo(exp);
    if (ei->m_settleFrac < 1.0) {
        values.greeks().apply(ei->m_settleFrac, values.greeks());
    }
}

void OptionPricer2::computeImpliedValues(OptionData* option,
    const OptionPricer2::BlackCalculatorInfo& bci)
{
    OptionValues& values = option->values(getValuesIndex());
    // No IBook in migrated OptionData; use OptionData::getMarket bid/ask/mid
    const OptionMarket& mkt = option->getMarket();
    bool book_ok = mkt.bid > 0 || mkt.ask > 0;
    if (!book_ok) {
        values.m_impliedVol = -1;
        values.m_impliedBidVol = -1;
        values.m_impliedAskVol = -1;
        return;
    }
    double bid = mkt.bid;
    double mid = option->getMid();
    double ask = mkt.ask;

    // Pre-condition: forward and maturity must be valid for IV calculation
    if (std::isnan(bci.m_forward) || bci.m_forward <= 0 ||
        std::isnan(bci.m_maturity) || bci.m_maturity <= 0 ||
        std::isnan(bci.m_discount) || bci.m_discount <= 0) {
        values.m_impliedVol = -1;
        values.m_impliedBidVol = -1;
        values.m_impliedAskVol = -1;
        return;
    }

    double iv_bid = -1, iv_mid = -1, iv_ask = -1;
    if (bid > 0 && ask > 0) {
        try {
            // forward induced by put-call parity if both sides present
            double forward_option = bci.m_forward;
            // StrikeData siblings
            // Put-call parity synthetic forward. Require BOTH bid and ask on
            // both legs: getMid() falls back to last-trade price when a side
            // is missing, and a stale last price would bias F_syn and every
            // IV inverted from it (consistent with __getBestSyntheticPrice,
            // which also requires the full four-price set).
            StrikeDataPtr sd = option->getStrikeData();
            if (sd) {
                const OptionDataPtr& c = sd->call();
                const OptionDataPtr& p = sd->put();
                if (c && p && c->getMarket().bid > 0 && c->getMarket().ask > 0
                          && p->getMarket().bid > 0 && p->getMarket().ask > 0) {
                    forward_option = bci.m_strike + (c->getMid() - p->getMid()) / bci.m_discount;
                }
            }
            BlackImpliedCalculator bic(bci.m_optionType, bci.m_strike,
                                       forward_option, bci.m_maturity, bci.m_discount);
            iv_bid = bic.volatility(bid);
            iv_mid = bic.volatility(mid);
            iv_ask = bic.volatility(ask);
            if (GE(iv_ask - iv_bid, config().bid_offer_vol_spread_cap)) {
                iv_bid = iv_mid = iv_ask = -2;
            }
        } catch (...) {
            iv_bid = iv_mid = iv_ask = -3;
        }
    }
    values.m_impliedVol     = iv_mid;
    values.m_impliedBidVol  = iv_bid;
    values.m_impliedAskVol  = iv_ask;
    values.m_impliedVolSprd = iv_ask - iv_bid;
}

void OptionPricer2::finalizeCompute(OptionGrid* grid)
{
    if (!grid) return;
    // Per-option IOptionDataListener notification removed (dead facility) —
    // see CompositeOptionPricer::finalizeCompute for the rationale.
}

} // namespace wt_option

// ============================================================================
// triggerDoFit - 路径1: 定时器触发 doFit (与 quantbox onClockWakeup 等价)
// ============================================================================
namespace wt_option {
bool OptionPricer2::triggerDoFit()
{
    if (!m_spFitter) return false;

    // Use onTimer() so the fitter's m_period interval is respected.
    // onTimer() returns true only if doFit() was actually called (period elapsed).
    // doFit() internally checks anyForwardReady + time window, returning false if skipped.
    m_spFitter->setTime(getTime());
    bool ok = m_spFitter->onTimer(getTime());
    if (ok) {
        WTSLogger::log_by_cat("strategy", LL_INFO, "triggerDoFit fit completed time={:.1f}", getTime());
        for (auto& sink : m_fitCompletedEvent)
            sink(*m_spFitter);
    }
    return ok;
}
} // namespace wt_option
