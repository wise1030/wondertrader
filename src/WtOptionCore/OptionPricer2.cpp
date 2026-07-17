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
#include "OptionGrid.h"
#include "../WTSTools/WTSLogger.h"
#include "StrikeData.h"
#include "ExpiryData.h"
#include "OptionData.h"
#include "OptionValues.h"
#include "OptionGreeks.h"
#include "GvvVolCurve.h"
#include "PeriodicCurveFitter.h"

#include <limits>
#include <cmath>
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
inline double round_to_tick(double px, double tick) {
    if (tick <= 0) return px;
    return std::round(px / tick) * tick;
}
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
        ei->m_spVolCurve  = std::make_shared<GvvVolCurve>();
        ei->m_spVolCurve2 = std::make_shared<GvvVolCurve>();
        // hardcode defaults (preserve original)
        ei->m_futsprd = 1.0;
        ei->m_atmvolsprd = 0.2;
        updateGvvParams(exp, ei->m_spVolCurve);
    }
    return ei;
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
        m_spFitter->setTime(getTime());  // 同步时间给 fitter
        WTSLogger::log_by_cat("strategy", LL_INFO, "doFit triggered getTime={:.1f} lastFit={:.1f}",
            getTime(), m_spFitter->getLastFitTime());
        bool fitOk = m_spFitter->doFit();
        WTSLogger::log_by_cat("strategy", LL_INFO, "doFit result={}", fitOk);
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
    // NOTE: original stashed bci inside option->get<BlackCalculatorInfoPtr>().
    // OptionData migration does not expose arbitrary typed slots, so we keep a
    // side-table keyed by option pointer. This preserves behaviour with no
    // change to pricing math.
    static thread_local std::map<OptionData*, BlackCalculatorInfoPtr> s_cache;
    BlackCalculatorInfoPtr& bci = s_cache[option];
    if (!bci) {
        bci = std::make_shared<BlackCalculatorInfo>();
        bci->ei = getExpiryInfo(option->getExpiry());
        bci->m_optionType = (option->getRight() == OR_Call) ? OT_Call : OT_Put;
        bci->m_strike = option->getStrike();
    }
    const ExpiryDataPtr& ed = option->getExpiryData();
    const ExpiryInfo& ei = *(bci->ei);
    bci->m_forward = ei.m_atmforward;
    if (config().use_sprd_vs_atmfwd) {
        IVolCurvePtr fwdcurve = getFwdCurve(option->getExpiry());
        if (fwdcurve && fwdcurve->isInitialized()) {
            double sprd_vs_atmfwd = (*fwdcurve)(option->getStrike());
            bci->m_forward = ei.m_atmforward + sprd_vs_atmfwd;
        } else {
            EMAFilter& filter = option->values().ema_sprd_vs_atmfwd();
            if (filter.isOK()) {
                bci->m_forward = ei.m_atmforward + filter.getMean();
            }
        }
    }
    bci->m_discount   = ed ? ed->getDiscountFactor() : 1.0;
    bci->m_maturity   = ei.m_maturity;
    bci->m_settleFrac = ei.m_settleFrac;
    return bci;
}

// implied-vol helpers ---------------------------------------------------------
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
        #pragma omp parallel for schedule(dynamic, 4)
        for (long long i = 0; i < (long long)opts.size(); ++i) {
            __doImpliedVec(opts, (size_t)i);
        }
    }

    m_perfCounter.stop();
    return true;
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
        static int skipCount = 0;
        if (skipCount < 3) {
            skipCount++;
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "__computeTheo skip {} forward={} maturity={} vol={}",
                option->getCode(), bci->m_forward, bci->m_maturity, values.m_theoVol);
        }
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

    {
        static int bcCount = 0;
        if (bcCount < 3) {
            bcCount++;
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "BlackCalc {} type={} K={} F={} sigma={} df={} value={} delta={} vega={}",
                option->getCode(), (int)bci->m_optionType, bci->m_strike, bci->m_forward,
                values.m_theoVol * std::sqrt(bci->m_maturity), bci->m_discount,
                bc.value(), bc.delta(), bc.vega(bci->m_maturity));
        }
    }

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
    const ExpiryInfoPtr& ei = getExpiryInfo(exp);
    double atmvol = ei->m_atmvol;
    double atmforward = ei->m_atmforward;
    IVolCurvePtr spVolCurve  = getVolCurve(exp);
    IVolCurvePtr spVolCurve2 = getVolCurve2(exp);
    double gvv_weight = 0.0;
    if (m_gvv_weight.count(exp))
        gvv_weight = std::max(0.0, std::min(1.0, m_gvv_weight[exp]));
    if (ed && !ed->isFitReady())
        gvv_weight = 0.0;

    double vol  = spVolCurve  ? (*spVolCurve)(*option, atmforward)  * atmvol : atmvol;
    double vol2 = spVolCurve2 ? (*spVolCurve2)(*option, atmforward) * atmvol : atmvol;
    double theoVol = gvv_weight * vol + (1.0 - gvv_weight) * vol2;
    values.m_theoVol = std::max(0.01, theoVol);
    std::shared_ptr<BlackCalculatorInfo> bci = __setupBlackCalculatorInfo(option);

    // other side for put-call parity
    OptionValues& other_values = m_grid->get(exp, option->getStrike(),
        option->getRight() == OR_Call ? OR_Put : OR_Call)->values(getValuesIndex());
    __computeTheoreticalValues(option, other_values, bci.get(), getValuesIndex());
}

void OptionPricer2::__computeValue(OptionData* otm, OptionData* itm)
{
    uint32_t exp = otm->getExpiry();
    const ExpiryDataPtr& ed = otm->getExpiryData();
    const ExpiryInfoPtr& ei = getExpiryInfo(exp);
    double atmvol = ei->m_atmvol;
    double atmforward = ei->m_atmforward;
    IVolCurvePtr spVolCurve  = getVolCurve(exp);
    IVolCurvePtr spVolCurve2 = getVolCurve2(exp);
    double gvv_weight = 0.0;
    if (m_gvv_weight.count(exp))
        gvv_weight = std::max(0.0, std::min(1.0, m_gvv_weight[exp]));
    if (ed && !ed->isFitReady())
        gvv_weight = 0.0;

    OptionValues& otmvals = otm->values(getValuesIndex());
    OptionValues& itmvals = itm->values(getValuesIndex());
    const BlackCalculatorInfoPtr& obci = __setupBlackCalculatorInfo(otm);
    const BlackCalculatorInfoPtr& ibci = __setupBlackCalculatorInfo(itm);
    otmvals.setPriced(false);
    itmvals.setPriced(false);

    double vol  = spVolCurve  ? (*spVolCurve)(*otm, atmforward)  * atmvol : atmvol;
    double vol2 = spVolCurve2 ? (*spVolCurve2)(*otm, atmforward) * atmvol : atmvol;
    double theoVol = gvv_weight * vol + (1.0 - gvv_weight) * vol2;
    otmvals.m_theoVol = std::max(0.01, theoVol);
    itmvals.m_theoVol = std::max(0.01, theoVol);

    __computeTheoreticalValues(otm, itmvals, obci.get(), getValuesIndex());
    __computeTheoreticalValues(itm, otmvals, ibci.get(), getValuesIndex());
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

    double F = ei->m_atmforward;
    if (std::isnan(F) || EQ(F, 0)) return false;
    if (config().use_sprd_vs_atmfwd) {
        IVolCurvePtr fwdcurve = getFwdCurve(option->getExpiry());
        if (fwdcurve && fwdcurve->isInitialized()) {
            F = ei->m_atmforward + (*fwdcurve)(option->getStrike());
        } else {
            EMAFilter& filter = option->values().ema_sprd_vs_atmfwd();
            if (filter.isOK()) F = ei->m_atmforward + filter.getMean();
        }
    }
    const double dF = F - values.forward();
    double theo = std::max(0.0, values.theo() + values.greeks().delta() * dF);
    values.setForward(F);
    values.setTheo(theo);
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
            StrikeDataPtr sd = option->getStrikeData();
            if (sd) {
                const OptionDataPtr& c = sd->call();
                const OptionDataPtr& p = sd->put();
                if (c && p && (c->getMarket().bid > 0 || c->getMarket().ask > 0)
                          && (p->getMarket().bid > 0 || p->getMarket().ask > 0)) {
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
    const size_t idx = getValuesIndex();
    for (const auto& od : grid->getAllOptions()) {
        if (od) od->notifyMarketsPriced(idx);
    }
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
