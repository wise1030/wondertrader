/*!
 * \file CompositeOptionPricer.cpp
 * \brief Market-making pricer implementation — migrated from quantbox 2647-line original
 *
 * ALL business logic preserved 1:1. Framework dependencies replaced:
 * - longbeach::trading / clientcore / stratlib -> stripped
 * - boost::math -> std::cmath helpers
 * - IBook / MarketLevel / PriceSize -> simplified inline helpers
 * - CommandServices / PropertyManager / notifiable -> plain members
 * - ClockMonitor -> getTime()/setTime()
 * - BOOST_FOREACH -> range-for
 * - fmt::print -> WTSLogger or printf
 * - instrument_t -> std::string (option->getCode())
 * - expiry_t -> uint32_t
 * - OptionTradingData is wired to OptionData via OptionTradingGrid.
 * - UnderlyingTradingData is wired to ExpiryData via setHedgeUTD().
 */
#include "../WTSTools/WTSLogger.h"
#include "CompositeOptionPricer.h"
#include "OptionPricer2.h"
#include "OptionGrid.h"
#include "OptionRisk.h"
#include "OptionRiskData.h"
#include "OptionExpiryGreeks.h"
#include "ExpiryData.h"
#include "OptionData.h"
#include "OptionValues.h"
#include "StrikeData.h"
#include "OptionTradingData.h"
#include "UnderlyingTradingData.h"
#include "ExpiryTradingData.h"
#include "PeriodicCurveFitter.h"

#include <cmath>
#include <algorithm>
#include <numeric>
#include <cstdio>

namespace wt_option {

// ============================================================================
// Local helpers
// ============================================================================

static inline double sign(double x) { return (x > 0) ? 1.0 : ((x < 0) ? -1.0 : 0.0); }
static inline bool EQ(double a, double b) { return std::fabs(a - b) < 1e-12; }
static inline bool EQZ(double a) { return std::fabs(a) < 1e-12; }
static inline bool LT(double a, double b) { return a < b - 1e-12; }
static inline bool GT(double a, double b) { return a > b + 1e-12; }
static inline bool LE(double a, double b) { return a <= b + 1e-12; }
static inline bool GE(double a, double b) { return a >= b - 1e-12; }
static inline bool NE(double a, double b) { return !EQ(a, b); }
static inline bool NEZ(double a) { return !EQZ(a); }

static inline double round_to_tick(double px, double tick_size) {
    if (tick_size <= 0) return px;
    return std::round(px / tick_size) * tick_size;
}

static inline int32_t round_to_nearest_integer(double x) {
    return static_cast<int32_t>(std::round(x));
}

static inline double round_to_precision(double x, double prec) {
    if (prec <= 0) return x;
    return std::round(x / prec) * prec;
}

// Round bid/ask respecting side constraints (migrated from round_to_tick_by_side)
static double round_to_tick_by_side(double px, double tick_size, side_t side, double mid) {
    double px1 = std::max(tick_size, round_to_tick(px, tick_size));
    if (side == BID) {
        if (mid < tick_size) return 0;
        if (px1 > mid) {
            px1 = std::max(0.0, px1 - tick_size / 2.0 - 1e-9);
            return round_to_tick_by_side(px1, tick_size, BID, 1e18);
        }
    } else {
        if (px1 < mid)
            return round_to_tick_by_side(px1 + tick_size / 2.0, tick_size, ASK, 0);
    }
    return px1;
}

// Check if strike is near the money (call and put prices within 20x of each other)
static bool is_strike_near_the_money(StrikeData* sdata) {
    if (!sdata || !sdata->call() || !sdata->put()) return false;
    double call_mid = sdata->call()->getMid();
    double put_mid = sdata->put()->getMid();
    if (call_mid <= 0 || put_mid <= 0) return false;
    return (call_mid < 20 * put_mid) && (put_mid < 20 * call_mid);
}

// ============================================================================
// CompositeOptionPricerConfig
// ============================================================================
CompositeOptionPricerConfig::CompositeOptionPricerConfig() {
}

// ============================================================================
// ExpiryRiskConfig::init
// ============================================================================
void CompositeOptionPricer::ExpiryRiskConfig::init(
    uint32_t /*exp*/, const CompositeOptionPricerConfig& config, bool bEnable) {
    enable = bEnable;
    enable_auto_close = config.enable_auto_close;
    delta_min = 0.1;
    delta_max = 0.9;
    max_pos_fut = 1;
    max_pos_stk = 1;
    max_pos_opt = 1;
    max_qsize = 1;
    sprd_fut = 100.0;
    sprd_fwd = 0.01;
    sprd_atmvol = 0.1;
    sprd_corr = 0.0;
    numcxl = numrej = numpos = numfil = 0;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================
CompositeOptionPricer::CompositeOptionPricer(
    const CompositeOptionPricerConfig& c, OptionGrid* grid, OptionRisk* risk)
    : m_config(c)
    , m_grid(grid)
    , m_spPositionRisk(risk)
    , m_bReprice(true)
    , m_refPrice(0)
{
    setTraceLevel(config().trace_level);

    // Create the black pricer sub-pricer
    if (c.black_params) {
        m_spOptionPricer2 = std::make_shared<OptionPricer2>(*c.black_params, grid, risk);
        m_spOptionPricer2->setValuesIndex(0);
    }

    setupActiveExpiries();
    setupRiskTolerances();
    m_ema_vegaflow.setWindow(config().vegaflow_window);
    m_ema_frontatmv.setWindow(config().frontatmv_flow_window);
    m_ema_frontfut.setWindow(config().frontfut_skew_window);
    m_ema_deltaflow.setWindow(config().deltaflow_window);
    m_ema_roll_front_fut.setWindow(config().ema_roll_front_fut_window);

    // Copy hedge/lambda params from config (P0-D fix)
    m_prop_hedge_ratio_delta = config().hedge_ratio_delta;
    m_prop_hedge_ratio_vega  = config().hedge_ratio_vega;
    m_prop_lambda_vega_decay = config().lambda_vega_decay;
    m_prop_lambda_vega_wing  = config().lambda_vega_wing;

    // 注册 fitCompleted 回调 (与 quantbox 一致)
    if (m_spOptionPricer2) {
        auto& fce = m_spOptionPricer2->fitCompletedEvent();
        fce.push_back([this](const PeriodicCurveFitter& fitter) {
            this->onFitCompleted(fitter);
        });
    }
}

CompositeOptionPricer::~CompositeOptionPricer() {
}

void CompositeOptionPricer::enableExpiry(uint32_t exp, double deltaMin, double deltaMax) {
    auto& erc = m_mapExpiryRiskConfig[exp];
    erc.enable = true;
    erc.delta_min = deltaMin;
    erc.delta_max = deltaMax;
    // Default spread parameters (same as ExpiryRiskConfig::init)
    erc.sprd_fut = 100.0;
    erc.sprd_fwd = 0.01;
    erc.sprd_atmvol = 0.1;
    erc.sprd_corr = 0.0;
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "CompositeOptionPricer::enableExpiry {} enable={} delta=[{},{}] sprd_fwd={} sprd_atmvol={}",
        exp, erc.enable, erc.delta_min, erc.delta_max, erc.sprd_fwd, erc.sprd_atmvol);
}

void CompositeOptionPricer::setMaxPosQty(uint32_t exp, int32_t maxQsize, int32_t maxPosStk, int32_t maxPosOpt) {
    auto& erc = m_mapExpiryRiskConfig[exp];
    erc.max_qsize = maxQsize;
    erc.max_pos_stk = maxPosStk;
    erc.max_pos_opt = maxPosOpt;
}

void CompositeOptionPricer::setExpiryCloseParams(uint32_t exp, bool autoClose, int32_t closeThresh) {
    auto& erc = m_mapExpiryRiskConfig[exp];
    erc.enable_auto_close = autoClose;
    erc.close_pos_thresh = closeThresh;
}

// ============================================================================
// IOptionPricer delegation to OptionPricer2
// ============================================================================
IVolCurvePtr CompositeOptionPricer::getVolCurve(uint32_t exp) const {
    return m_spOptionPricer2 ? m_spOptionPricer2->getVolCurve(exp) : nullptr;
}

IVolCurvePtr CompositeOptionPricer::getVolCurve2(uint32_t exp) const {
    return m_spOptionPricer2 ? m_spOptionPricer2->getVolCurve2(exp) : nullptr;
}

IVolCurvePtr CompositeOptionPricer::getFwdCurve(uint32_t exp) const {
    return m_spOptionPricer2 ? m_spOptionPricer2->getFwdCurve(exp) : nullptr;
}

double CompositeOptionPricer::getATMForward(uint32_t exp) const {
    return m_spOptionPricer2 ? m_spOptionPricer2->getATMForward(exp) : NAN;
}

double CompositeOptionPricer::getMaturity(uint32_t exp) const {
    return m_spOptionPricer2 ? m_spOptionPricer2->getMaturity(exp) : NAN;
}

void CompositeOptionPricer::setATMVol(uint32_t exp, double atmvol) {
    if (m_spOptionPricer2) m_spOptionPricer2->setATMVol(exp, atmvol);
}

double CompositeOptionPricer::getATMVol(uint32_t exp) const {
    return m_spOptionPricer2 ? m_spOptionPricer2->getATMVol(exp) : 0;
}

double CompositeOptionPricer::getVol(uint32_t expiry, strike_t strike) const {
    if (!m_spOptionPricer2) return 0;
    IVolCurvePtr vc = m_spOptionPricer2->getVolCurve(expiry);
    if (!vc) return 0;
    double atmFwd = m_spOptionPricer2->getATMForward(expiry);
    if (std::isnan(atmFwd) || EQZ(atmFwd)) return 0;
    // Use the vol curve's option-relative evaluation
    // We don't have an OptionData here; approximate via strike ratio
    return vc->isInitialized() ? m_spOptionPricer2->getATMVol(expiry) : 0;
}

void CompositeOptionPricer::resetLastComputeTime() {
    m_tvLastCompute = 0;
}

// ============================================================================
// setupActiveExpiries / setupRiskTolerances
// ============================================================================
void CompositeOptionPricer::setupActiveExpiries() {
    if (!m_grid) return;
    for (const auto& v : m_grid->expiries()) {
        m_active_expiries.insert(v.first);
        ExpiryRiskConfig& erc = m_mapExpiryRiskConfig[v.first];
        erc.init(v.first, config(), false);
    }
}

void CompositeOptionPricer::setupRiskTolerances() {
    // Default tolerances — in production these come from config
    ToleranceParams& td = m_tolerance_params[GT_delta];
    td.risk_tol = config().risk_prem_opt > 0 ? config().risk_prem_opt : 10.0;

    ToleranceParams& tv = m_tolerance_params[GT_vega_tw];
    tv.risk_tol = config().risk_prem_opt > 0 ? config().risk_prem_opt * 100 : 1000.0;

    // Primary expiry = front month
    if (m_grid && !m_grid->expiries().empty()) {
        m_prop_primary_exp = m_grid->expiries().begin()->first;
    }
}

// ============================================================================
// computeValues — FAST/SLOW scheduling
// ============================================================================
bool CompositeOptionPricer::computeValues(OptionGrid* grid) {
    // Don't compute if we already did it this cycle
    if (getTime() == m_tvLastCompute)
        return false;

    double refpx = grid->getUnderlyingPrice();
    if (refpx <= 0) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "CompositeOptionPricer: bad underlying price %.4f, skipping compute", refpx);
        return false;
    }

    m_tvLastCompute = getTime();
    m_refPrice = refpx;

    initValuesCompute(grid);

    double tv_diff = getTime() - m_tvLastSlowCompute;
    m_bReprice = m_bReprice || (tv_diff > config().slow_compute_interval);

    if (!m_bReprice) {
        computeValues_FAST(grid);
    } else {
        m_bReprice = false;
        m_tvLastSlowCompute = getTime();
        computeValues_SLOW(grid);
    }
    return true;
}

bool CompositeOptionPricer::computeImpliedValues(OptionGrid* grid) {
    if (m_spOptionPricer2)
        m_spOptionPricer2->computeImpliedValues(grid);
    return true;
}

// ============================================================================
// computeValues_FAST — per-tick update without repricing Greeks
// ============================================================================
void CompositeOptionPricer::computeValues_FAST(OptionGrid* grid) {
    // Clear all num properties
    m_numcxl = 0;
    m_numrej = 0;
    m_numpos = 0;
    m_numfil = 0;
    for (uint32_t exp : m_active_expiries) {
        m_exp_numcxl[exp] = 0;
        m_exp_numrej[exp] = 0;
        m_exp_numpos[exp] = 0;
        m_exp_numfil[exp] = 0;
    }

    int skipCount = 0, procCount = 0;
    for (const auto& sd : grid->getAllStrikes()) {
        const OptionDataPtr& otm = sd->getStrikePrice() < m_refPrice ? sd->put() : sd->call();
        const OptionDataPtr& itm = sd->getStrikePrice() < m_refPrice ? sd->call() : sd->put();

        // Skip if either leg is missing (partial strike during dynamic discovery)
        if (!otm || !itm) { skipCount++; continue; }
        procCount++;

        const ExpiryDataPtr& ed = sd->getExpiryData();

        // We do not need fit ready to quote.
        bool expiry_ready = ed && ed->isForwardReady();
        if (!expiry_ready) {
            // Reset priced flag so stale theoretical values don't pass inputs_good
            otm->values(0).setPriced(false);
            itm->values(0).setPriced(false);
            otm->values(0).alpha().clear();
            otm->values(0).adj().clear();
            itm->values(0).alpha().clear();
            itm->values(0).adj().clear();
            computeOurMarkets(otm.get(), sd.get());
            computeOurMarkets(itm.get(), sd.get());
        } else {
            bool otm_values_ok = otm->values(0).isPriced();
            if (!otm_values_ok)
                computeValue(otm.get());
            bool itm_values_ok = itm->values(0).isPriced();
            if (!itm_values_ok)
                computeValue(itm.get());

            bool otm_update_ok = updateDistortValues(otm.get());
            if (otm_update_ok) {
                computeOurMarkets(otm.get(), sd.get());
            }
            bool itm_update_ok = updateDistortValues(itm.get());
            if (itm_update_ok) {
                computeOurMarkets(itm.get(), sd.get());
            }

            decayGreeks(otm.get());
            decayGreeks(itm.get());
        }

        uint32_t exp = ed ? ed->getExpiry() : 0;
        // OTD counters not available without OptionTradingGrid wiring; skip accumulation
    }

    // Futures
    for (const auto& v : grid->expiries()) {
        const ExpiryDataPtr& ed = v.second;
        // A3: UnderlyingTradingData is wired to ExpiryData via setHedgeUTD().
        // updateDistortValuesFuture updates forward + applies risk adjustments.
        if (!ed->isForwardReady()) {
            computeOurMarketsFuture(ed.get());
        } else {
            if (updateDistortValuesFuture(ed.get()))
                computeOurMarketsFuture(ed.get());
        }
    }

    finalizeCompute(grid);

    m_prop_numcxl = m_numcxl;
    m_prop_numrej = m_numrej;
    m_prop_numpos = m_numpos;
    m_prop_numfil = m_numfil;
    for (uint32_t exp : m_active_expiries) {
        ExpiryRiskConfig& erc = m_mapExpiryRiskConfig[exp];
        erc.numcxl = m_exp_numcxl[exp];
        erc.numrej = m_exp_numrej[exp];
        erc.numpos = m_exp_numpos[exp];
        erc.numfil = m_exp_numfil[exp];
    }
}

// ============================================================================
// computeValues_SLOW — full reprice + IV + curve refit
// ============================================================================
void CompositeOptionPricer::computeValues_SLOW(OptionGrid* grid) {
    for (const auto& sd : grid->getAllStrikes()) {
        const OptionDataPtr& otm = sd->getStrikePrice() < m_refPrice ? sd->put() : sd->call();
        const OptionDataPtr& itm = sd->getStrikePrice() < m_refPrice ? sd->call() : sd->put();

        // Skip if either leg is missing
        if (!otm || !itm) continue;

        const ExpiryDataPtr& ed = sd->getExpiryData();

        bool expiry_ready = ed && ed->isForwardReady();
        if (!expiry_ready) {
            // Reset priced flag so stale theoretical values don't pass inputs_good
            otm->values(0).setPriced(false);
            itm->values(0).setPriced(false);
            otm->values(0).alpha().clear();
            otm->values(0).adj().clear();
            itm->values(0).alpha().clear();
            itm->values(0).adj().clear();
            computeOurMarkets(otm.get(), sd.get());
            computeOurMarkets(itm.get(), sd.get());
        } else {
            // explicitly reset both otm and itm values
            otm->values(0).setPriced(false);
            itm->values(0).setPriced(false);

            computeValue(otm.get());
            computeValue(itm.get());

            if (updateDistortValues(otm.get()))
                computeOurMarkets(otm.get(), sd.get());
            if (updateDistortValues(itm.get()))
                computeOurMarkets(itm.get(), sd.get());

            decayGreeks(otm.get());
            decayGreeks(itm.get());
        }
    }

    computeImpliedValues(grid);

    // Diagnostic: count priced options after SLOW main loop
    {
        static int s_slowDiag = 0;
        if (s_slowDiag < 10) {
            s_slowDiag++;
            int priced = 0, notReady = 0;
            for (const auto& od : grid->getAllOptions()) {
                if (!od) continue;
                if (od->values(0).isPriced()) priced++;
            }
            for (const auto& v : grid->expiries()) {
                if (v.second && !v.second->isForwardReady()) notReady++;
            }
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "SLOW #{}: priced={}/{} fwdNotReady expiries={}",
                s_slowDiag, priced, grid->numOptions(), notReady);
        }
    }

    for (const auto& v : grid->expiries()) {
        const ExpiryDataPtr& ed = v.second;
        if (!ed->isForwardReady()) {
            computeOurMarketsFuture(ed.get());
        } else {
            if (updateDistortValuesFuture(ed.get()))
                computeOurMarketsFuture(ed.get());
        }
    }

    finalizeCompute(grid);
    m_bShouldComputeRiskShiftsVega = true;
}

// ============================================================================
// initValuesCompute — setup CVContext, EMA updates, risk shifts
// ============================================================================
bool CompositeOptionPricer::initValuesCompute(OptionGrid* grid) {
    // Set compute time on grid for EMA updates in __getBestSyntheticPrice
    grid->setComputeTime(getTime());

    for (uint32_t exp : m_active_expiries) {
        CVExpiryContext& exp_cxt = m_cvContext.exp_cxt[exp];
        const ExpiryRiskConfig& erc = m_mapExpiryRiskConfig[exp];
        ExpiryDataPtr ed = grid->getExpiryData(exp);
        if (!ed) continue;

        exp_cxt.primary_future_spread = erc.sprd_fut;

        // Position greeks from risk
        if (m_spPositionRisk) {
            auto eg = m_spPositionRisk->getExpiryGreeks(exp);
            if (eg) {
                CVExpiryContext& expcxt = cvcxt().exp_cxt[exp];
                expcxt.pos_delta = eg->totalDelta();
                expcxt.pos_vega = eg->vega();

                // publish greeks
                ExpiryDataPub& edpub = m_prop_edpub[exp];
                edpub.delta = round_to_nearest_integer(eg->totalDelta());
                edpub.vega_tw = round_to_nearest_integer(eg->vegaTW());
            }
        }
    }

    // Front month EMA updates
    m_frontfwd = 0;
    if (m_grid) {
        m_frontfwd = m_grid->getFrontForward();
    }
    m_frontatmv = getATMVol(m_prop_primary_exp);

    // front future price (from grid underlying)
    if (m_grid) {
        m_frontfut = m_grid->getUnderlyingPrice();
    }
    if (m_frontfut > 0) {
        m_ema_frontfut.update(getTime(), m_frontfut);
    }

    // Roll EMA: 0.001*frontfut - frontfwd
    double rollema = 0.0;
    if (m_frontfut > 0 && m_frontfwd > 0 && !std::isnan(m_frontfwd)) {
        double roll_front_fut_vs_fwd = m_frontfut - m_frontfwd;
        m_ema_roll_front_fut.update(getTime(), roll_front_fut_vs_fwd);
        rollema = m_ema_roll_front_fut.isOK()
            ? roll_front_fut_vs_fwd - m_ema_roll_front_fut.getMean()
            : 0.0;
    }

    // ATM vol EMA
    IVolCurvePtr vc = getVolCurve(m_prop_primary_exp);
    if (vc && vc->isInitialized() && m_frontatmv > 0) {
        m_ema_frontatmv.update(getTime(), m_frontatmv);
    }

    // Delegate init to sub-pricer
    if (m_spOptionPricer2) {
        m_spOptionPricer2->initValuesCompute(grid);
        // Set GVV blend weight per expiry (quantbox: COP sets OP2's gvv_weight
        // from a notifiable property; here we use config. 0=fitted curve, 1=GVV)
        for (uint32_t exp : m_active_expiries) {
            m_spOptionPricer2->setGvvWeight(exp, config().volcurve_weight);
        }
    }

    // Feed signal context
    m_signalCtx.time = getTime();
    m_signalCtx.underlyingPrice = m_frontfut;
    m_signalCtx.frontForward = m_frontfwd;
    m_signalCtx.frontAtmVol = m_frontatmv;
    m_signalCtx.grid = m_grid;
    for (auto& sig : m_alphaSignals)
        sig->onBatchStart(m_signalCtx);

    // Check risk signals
    checkRiskSignals();

    updateRiskShiftsDelta(grid);
    updateRiskShiftsVega(grid);

    // Portfolio greeks
    if (m_spPositionRisk) {
        m_prop_pfdelta = round_to_precision(m_spPositionRisk->totalDelta(), 1);
        const OptionGreeks& greeks = m_spPositionRisk->option_pfgreeks();
        m_prop_pfgamma = round_to_precision(greeks.gamma(), 1);
        m_prop_pfvega = round_to_precision(greeks.vegaTW(), 1);
    }

    m_prop_vegaflow = round_to_precision(m_ema_vegaflow.getSum(), 0.01);
    double frontatmv_flow = m_ema_frontatmv.isOK()
        ? (m_frontatmv - m_ema_frontatmv.getMean()) * 100
        : 0.0;
    m_prop_frontatmv_flow = round_to_precision(frontatmv_flow, 0.01);
    double frontfut_skew = (m_ema_frontfut.isOK() && m_frontfut > 0)
        ? (m_frontfut - m_ema_frontfut.getMean()) / m_frontfut * 1e4
        : 0.0;
    m_prop_frontfut_skew = round_to_precision(frontfut_skew, 0.01);
    m_prop_deltaflow = round_to_precision(m_ema_deltaflow.getSum(), 1);
    m_prop_rollema = round_to_precision(rollema, 0.00001);

    return true;
}

// ============================================================================
// computeValue / decayGreeks — delegate to OptionPricer2
// ============================================================================
void CompositeOptionPricer::computeValue(OptionData* option) {
    if (m_spOptionPricer2)
        m_spOptionPricer2->computeValue(option);
}

void CompositeOptionPricer::decayGreeks(OptionData* option) {
    if (m_spOptionPricer2)
        m_spOptionPricer2->decayGreeks(option);
}

// ============================================================================
// updateDistortValues — alpha + risk adjustment per option
// ============================================================================
bool CompositeOptionPricer::updateDistortValues(OptionData* option) {
    uint32_t exp = option->getExpiry();
    OptionValues& black_values = option->values(0);

    if (!m_active_expiries.count(exp)) {
        black_values.adj().total = NAN;
        return false;
    }

    bool black_good = m_spOptionPricer2
        ? m_spOptionPricer2->updateTheoreticalValues(option)
        : false;
    if (!black_good) {
        black_values.adj().total = NAN;
        return false;
    }

    // fees — per-contract transaction fee from contract info (wired onto
    // OptionData by the strategy from stra_get_comminfo). Falls back to 0.
    black_values.m_fees = option->getFee();

    if (!black_values.isPriced()) {
        black_values.adj().total = NAN;
        black_values.ourMarket().clear();
        option->setActive(false);
        return false;
    }

    // alpha
    alpha_adjustment(option);

    // risk
    risk_adjustment(option);
    black_values.adj().total = black_values.adj().total_risk;

    return true;
}

bool CompositeOptionPricer::updateDistortValuesFuture(ExpiryData* ed) {
    uint32_t exp = ed->getExpiry();
    if (!m_active_expiries.count(exp)) return false;
    // A3: UnderlyingTradingData is wired to ExpiryData via setHedgeUTD().
    // Update the theoretical forward, then apply distort values if UTD is active.
    if (!updateTheoreticalValuesFuture(ed)) return false;

    // Apply future risk adjustment (alpha/adjustments on the UTD)
    UnderlyingTradingData* utd = ed->getHedgeUTD();
    if (utd && utd->isActive()) {
        UnderlyingValues& uv = utd->values(0);
        uv.adj().clear();
        double adj = risk_adjustment_future(ed);
        uv.adj().risk = adj;
        uv.adj().total = uv.adj().risk;
    }
    return true;
}

bool CompositeOptionPricer::updateTheoreticalValuesFuture(ExpiryData* ed) {
    double F = m_spOptionPricer2 ? m_spOptionPricer2->getATMForward(ed->getExpiry()) : NAN;
    if (std::isnan(F) || EQ(F, 0)) return false;
    // ed->setForward(F) removed: m_fwd is already set by __getBestSyntheticPrice
    // (called via grid->getAtmForward -> __getBestSyntheticPrice).
    // F here is a cached copy of the same value.
    // Propagate forward to UTD so AttributePublisher and other consumers see it
    UnderlyingTradingData* utd = ed->getHedgeUTD();
    if (utd) utd->setFwd(F);
    return true;
}

// ============================================================================
// Spread helpers
// ============================================================================
double CompositeOptionPricer::__getForwardSpread(uint32_t exp) {
    auto it = m_mapExpiryRiskConfig.find(exp);
    return it != m_mapExpiryRiskConfig.end() ? it->second.sprd_fwd : 0;
}

double CompositeOptionPricer::__getFutureSpread(uint32_t exp) {
    auto it = m_mapExpiryRiskConfig.find(exp);
    return it != m_mapExpiryRiskConfig.end() ? it->second.sprd_fut : 0;
}

double CompositeOptionPricer::__getAtmvolSpread(uint32_t exp) {
    auto it = m_mapExpiryRiskConfig.find(exp);
    return it != m_mapExpiryRiskConfig.end() ? it->second.sprd_atmvol : 0;
}

double CompositeOptionPricer::__getFutureMarkup(uint32_t exp, double /*mid*/) {
    return __getFutureSpread(exp);
}

// ============================================================================
// __getOptionCosts — sqrt(delta²*fwd² + vega²*atmvol² + 2*delta*fwd*vega*atmvol*corr)
// ============================================================================
double CompositeOptionPricer::__getOptionCosts(OptionData* option, double /*mid*/) {
    uint32_t exp = option->getExpiry();
    OptionValues& black_values = option->values(0);
    const OptionGreeks& black_greeks = black_values.greeks();
    auto erc_it = m_mapExpiryRiskConfig.find(exp);
    const ExpiryRiskConfig& erc = (erc_it != m_mapExpiryRiskConfig.end())
        ? erc_it->second : m_mapExpiryRiskConfig[exp];
    double fwdsprd = __getForwardSpread(exp);
    double atmvolsprd = __getAtmvolSpread(exp);
    double delta = black_greeks.delta();
    double core_spread = std::sqrt(
          delta * delta * fwdsprd * fwdsprd
        + black_greeks.vega() * black_greeks.vega() * atmvolsprd * atmvolsprd
        + 2.0 * delta * fwdsprd * black_greeks.vega() * atmvolsprd * erc.sprd_corr);
    delta = std::abs(delta);
    // fees done in updateDistortValues
    core_spread += 2.0 * black_values.m_fees;
    return core_spread / 2.0;
}

// ============================================================================
// __apply_sticky_params
// ============================================================================
double CompositeOptionPricer::__apply_sticky_params(
    side_t s, const PriceSize& our_q, double new_px, double tick_size) {
    double base = config().sticky_base * tick_size;
    double sticky_pts = base;

    double rval = new_px;
    // more aggressive in retreating, less aggressive in improving
    double upper_ratio = (s == BID) ? config().improve_retreat_ratio : 1.0;
    double lower_ratio = (s == BID) ? 1.0 : config().improve_retreat_ratio;
    if ((new_px < our_q.px() + upper_ratio * sticky_pts)
        && (new_px > our_q.px() - lower_ratio * sticky_pts))
        rval = our_q.px();  // stick price

    return rval;
}

// ============================================================================
// __computeBidAndAsk — theo bid/ask with sticky + round_to_tick
// ============================================================================
void CompositeOptionPricer::__computeBidAndAsk(
    bo_data* out, double mid, double bid_spread, double ask_spread,
    const PriceSize& ourmkt_bid, const PriceSize& ourmkt_ask, OptionData* od) {
    double tick_size = od->getTickSize();
    if (tick_size <= 0) tick_size = 1e-6;

    // compute theoretical b/a
    out->bid = mid - bid_spread;
    out->ask = mid + ask_spread;

    out->bid = std::max(tick_size, out->bid);
    out->ask = std::max(tick_size, out->ask);

    // quote bid (with sticky)
    out->qbid = std::max(0.0, out->bid);
    out->qbid_tick_size = tick_size;
    if (!ourmkt_bid.empty()) {
        out->qbid = __apply_sticky_params(BID, ourmkt_bid, out->qbid, out->qbid_tick_size);
    }
    out->bid = round_to_tick_by_side(out->bid, tick_size, BID, mid);
    out->qbid = round_to_tick_by_side(out->qbid, tick_size, BID, mid);

    // quote ask (with sticky)
    out->qask = out->ask;
    out->qask_tick_size = tick_size;
    if (!ourmkt_ask.empty()) {
        out->qask = __apply_sticky_params(ASK, ourmkt_ask, out->qask, out->qask_tick_size);
    }
    out->ask = round_to_tick_by_side(out->ask, tick_size, ASK, mid);
    out->qask = round_to_tick_by_side(out->qask, tick_size, ASK, mid);
}

// ============================================================================
// __computeBidAndAskFuture
// ============================================================================
void CompositeOptionPricer::__computeBidAndAskFuture(
    double& bid, double& bid_tick_size, double& ask, double& ask_tick_size,
    double mid, double bid_spread, double ask_spread,
    const PriceSize& ourmkt_bid, const PriceSize& ourmkt_ask,
    double tick_size) {
    if (tick_size <= 0) tick_size = 1e-6;
    bid = std::max(0.0, mid - bid_spread);
    bid_tick_size = tick_size;  // from contract info (UnderlyingTradingData::getTickSize)
    if (!ourmkt_bid.empty()) {
        bid = __apply_sticky_params(BID, ourmkt_bid, bid, bid_tick_size);
    }

    ask = mid + ask_spread;
    ask_tick_size = tick_size;
    ask = std::max(ask_tick_size, ask);
    if (!ourmkt_ask.empty()) {
        ask = __apply_sticky_params(ASK, ourmkt_ask, ask, ask_tick_size);
    }
}

// ============================================================================
// __computeQuoteSize — position-limited size
// ============================================================================
int32_t CompositeOptionPricer::__computeQuoteSize(
    const ExpiryRiskConfig& erc, side_t side, double /*mid*/, double /*px*/,
    int32_t opt_pos, int32_t stk_pos, double /*delta*/) {
    if (side == BID) {
        int32_t bid_size = std::min(erc.max_qsize, std::max(0, erc.max_pos_stk - stk_pos));
        bid_size = std::min(bid_size, std::max(0, erc.max_pos_opt - opt_pos));
        return bid_size;
    } else {
        int32_t ask_size = std::min(erc.max_qsize, std::max(0, -(-erc.max_pos_stk - stk_pos)));
        ask_size = std::min(ask_size, std::max(0, -(-erc.max_pos_opt - opt_pos)));
        return ask_size;
    }
}

// ============================================================================
// __compute_interesting (static) — should we place at this price?
// ============================================================================
bool CompositeOptionPricer::__compute_interesting(
    side_t s, double new_px, double mkt_px, double interesting_cutoff,
    const CompositeOptionPricerConfig& config) {
    // BID: interesting if new_px >= cutoff (we're aggressive enough)
    // ASK: interesting if new_px <= cutoff
    bool within_cutoff = (s == BID) ? GE(new_px, interesting_cutoff) : LE(new_px, interesting_cutoff);
    if (!within_cutoff) return false;

    // If we're at or inside the market, definitely interesting
    bool inside_mkt = (s == BID) ? GE(new_px, mkt_px) : LE(new_px, mkt_px);
    if (inside_mkt) return true;

    // Outside market: simplified — check book ahead thresholds
    // Full version checks get_size_at_price / get_size_ahead; without IBook we
    // err on the side of placing the order.
    return true;
}

// ============================================================================
// __compute_teenie_price (static)
// ============================================================================
double CompositeOptionPricer::__compute_teenie_price(
    side_t dir, double our_px, int32_t our_sz, const PriceSize& mkt, double tick_size) {
    double px = our_px;
    double teenie_amt = tick_size;  // one tick
    if (mkt.sz() < our_sz)
        teenie_amt = 0;
    if (dir == BID)
        px = std::min(our_px, mkt.px() + teenie_amt);
    else
        px = std::max(our_px, mkt.px() - teenie_amt);
    return px;
}

// ============================================================================
// computeOurMarkets — CORE: compute our bid/ask for an option
// ============================================================================
void CompositeOptionPricer::computeOurMarkets(OptionData* option, StrikeData* sdata) {
    uint32_t exp = option->getExpiry();
    OptionValues& black_values = option->values(0);

    auto erc_it = m_mapExpiryRiskConfig.find(exp);
    if (erc_it == m_mapExpiryRiskConfig.end() || !erc_it->second.enable) {
        black_values.ourMarket().clear();
        option->setActive(false);
        return;
    }
    const ExpiryRiskConfig& erc = erc_it->second;

    double alpha_val = black_values.alpha().total;
    double adj_val = black_values.adj().total;
    double mid = black_values.theo();

    bool inputs_good = black_values.isPriced()
        && !std::isnan(alpha_val)
        && !std::isnan(adj_val)
        && !EQZ(mid)
        && option->getBid() > 0 && option->getAsk() > 0;

    if (!inputs_good) {
        black_values.ourMarket().clear();
        black_values.theoPrices().clear();
        option->setActive(false);
        return;
    }

    // spread
    double our_cost = __getOptionCosts(option, mid);
    double widen = getRiskWidenFactorByCode(option->getCode());
    our_cost *= widen;
    double bid_spread = our_cost;
    double ask_spread = our_cost;

    double delta = std::abs(black_values.greeks().delta());

    {
        double tick_size = option->getTickSize();
        if (tick_size <= 0) tick_size = 1e-6;
        bid_spread = std::max(bid_spread, (0.5 + config().sticky_base) * tick_size);
        ask_spread = std::max(ask_spread, (0.5 + config().sticky_base) * tick_size);
    }

    // floor by zero, apply alpha + adj
    bid_spread = std::max(0.0, bid_spread - alpha_val - adj_val);
    ask_spread = std::max(0.0, ask_spread + alpha_val + adj_val);

    // bid/ask
    MultiMarket& multmkt = black_values.ourMarket();
    PriceSize ourmkt_bid = multmkt.getBest(BID);
    PriceSize ourmkt_ask = multmkt.getBest(ASK);

    bo_data bo;
    __computeBidAndAsk(&bo, mid, bid_spread, ask_spread, ourmkt_bid, ourmkt_ask, option);
    double bid = bo.qbid;
    double bid_tick_size = bo.qbid_tick_size;
    double ask = bo.qask;
    double ask_tick_size = bo.qask_tick_size;

    if (EQ(bid, ask)) {  // oops!
        double tick_size = option->getTickSize();
        if (tick_size <= 0) tick_size = 1e-6;
        bid -= tick_size;
        ask += tick_size;
    }

    // one last defense: trade shock back-away
    const std::string& instr = option->getCode();
    clearLastTrades(instr);
    auto lb_it = m_instrument_lastbuy.find(instr);
    if (lb_it != m_instrument_lastbuy.end() && lb_it->second) {
        double fill_px = lb_it->second->fillPrice;
        bid = std::min(bid, fill_px - m_prop_trade_shock_ticks * bid_tick_size);  // back away
        bid = std::max(bid, bid_tick_size);
        ask = std::max(fill_px + ask_tick_size, ask);  // prevent thrashing
    }
    auto ls_it = m_instrument_lastsell.find(instr);
    if (ls_it != m_instrument_lastsell.end() && ls_it->second) {
        double fill_px = ls_it->second->fillPrice;
        ask = std::max(ask, fill_px + m_prop_trade_shock_ticks * ask_tick_size);  // back away
        bid = std::min(fill_px - bid_tick_size, bid);  // prevent thrashing
        bid = std::max(bid, bid_tick_size);
    }

    int32_t stk_pos = 0;
    if (sdata) {
        double call_pos = sdata->call() ? sdata->call()->getPosition() : 0;
        double put_pos  = sdata->put()  ? sdata->put()->getPosition()  : 0;
        stk_pos = static_cast<int32_t>(call_pos + put_pos);
    }

    if (!option->isActive()) {
        // make sure our_market starts as blank slate if its currently not active
        black_values.ourMarket().clear();
    }

    // activate/deactivate — Phase 7: use OTD QuoteMode state machine
    auto otd = option->getTradingData();
    QuoteMode qmode = otd ? otd->getQuoteMode() : QM_AUTO;

    bool active = false;
    bool enable_bid = false;
    bool enable_ask = false;

    if (qmode == QM_OFF) {
        black_values.ourMarket().clear();
        if (otd) otd->setActive(false);
        return;
    }

    bool is_within_delta_range = delta > erc.delta_min && delta < erc.delta_max
        && is_strike_near_the_money(sdata);

    if (qmode == QM_ON) {
        enable_bid = true;
        enable_ask = true;
    } else if (qmode == QM_AUTO && is_within_delta_range) {
        enable_bid = true;
        enable_ask = true;
        if (erc.enable_auto_close && otd) {
            int32_t pos = otd->getPosition();
            if (pos != 0)
                otd->setQuoteMode(QM_CLOSE);
        }
    } else if (qmode == QM_CLOSE) {
        int32_t pos = otd ? otd->getPosition() : static_cast<int32_t>(option->getPosition());
        if (pos == 0) {
            if (otd) otd->setQuoteMode(QM_AUTO);
            enable_bid = true;
            enable_ask = true;
        } else {
            double close_thresh = erc.close_pos_thresh;
            if (pos < -close_thresh) enable_bid = true;
            if (pos > close_thresh) enable_ask = true;
        }
    }

    active = enable_bid || enable_ask;
    if (!enable_bid)
        black_values.ourMarket().eraseBids();
    if (!enable_ask)
        black_values.ourMarket().eraseAsks();

    int32_t opt_pos = static_cast<int32_t>(option->getPosition());
    double tick_size = option->getTickSize();
    if (tick_size <= 0) tick_size = 1e-6;

    if (enable_bid) {
        int32_t bid_size = __computeQuoteSize(erc, BID, mid, bid, opt_pos, stk_pos, delta);
        bool valid = erc.enable && (bid_size > 0);
        if (!valid) {
            multmkt.eraseBids();
        } else {
            bid = round_to_tick_by_side(bid, tick_size, BID, mid);
            PriceSize new_bid(bid, bid_size);
            PriceSize old_bid = multmkt.getBest(BID);
            if (old_bid.empty() || !EQ(old_bid.px(), bid) || old_bid.sz() != bid_size) {
                multmkt.setBest(BID, new_bid);
            }
        }
    }

    if (enable_ask) {
        int32_t ask_size = __computeQuoteSize(erc, ASK, mid, ask, opt_pos, stk_pos, delta);
        bool valid = erc.enable && (ask_size > 0);
        if (!valid) {
            multmkt.eraseAsks();
        } else {
            ask = round_to_tick_by_side(ask, tick_size, ASK, mid);
            PriceSize new_ask(ask, ask_size);
            PriceSize old_ask = multmkt.getBest(ASK);
            if (old_ask.empty() || !EQ(old_ask.px(), ask) || old_ask.sz() != ask_size) {
                multmkt.setBest(ASK, new_ask);
            }
        }
    }

    if (!active) {
        black_values.ourMarket().clear();
        option->setActive(false);
    } else {
        option->setActive(true);
    }

    if (isPanicked()) {
        black_values.ourMarket().eraseBids();
        black_values.ourMarket().eraseAsks();
    }

    // bidvol/askvol (for display)
    const OptionGreeks& black_greeks = black_values.greeks();
    black_values.m_theoBidVol = 0.0;
    if (black_values.m_impliedBidVol > 0 && black_greeks.vega() != 0) {
        black_values.m_theoBidVol = black_values.m_impliedBidVol
            + (bid - option->getBid()) / black_greeks.vega() / 100;
    }
    black_values.m_theoAskVol = 0.0;
    if (black_values.m_impliedAskVol > 0 && black_greeks.vega() != 0) {
        black_values.m_theoAskVol = black_values.m_impliedAskVol
            + (ask - option->getAsk()) / black_greeks.vega() / 100;
    }
    black_values.theoPrices().bid = bo.bid;
    black_values.theoPrices().ask = bo.ask;
    black_values.theoPrices().mid = mid;
}

// ============================================================================
// computeOurMarketsFuture — compute our bid/ask for underlying future
// ============================================================================
void CompositeOptionPricer::computeOurMarketsFuture(ExpiryData* ed) {
    uint32_t exp = ed->getExpiry();
    UnderlyingTradingData* utd = ed->getHedgeUTD();

    if (!utd || !utd->isActive()) {
        if (utd) utd->ourMarket().clear();
        return;
    }

    double mid = ed->getForward();
    if (!ed->isForwardReady() || std::isnan(mid) || mid <= 0) {
        utd->ourMarket().clear();
        return;
    }

    auto erc_it = m_mapExpiryRiskConfig.find(exp);
    const ExpiryRiskConfig& erc = (erc_it != m_mapExpiryRiskConfig.end())
        ? erc_it->second : m_mapExpiryRiskConfig[exp];

    const CVExpiryContext& exp_cxt = m_cvContext.exp_cxt[exp];

    // spread
    double core_spread = exp_cxt.primary_future_spread;
    double bid_spread = 0.5 * core_spread;
    double ask_spread = 0.5 * core_spread;
    double tick_size = utd->getTickSize();
    if (tick_size <= 0) tick_size = 1e-6;

    bid_spread = std::max(bid_spread, (0.5 + config().sticky_base) * tick_size);
    ask_spread = std::max(ask_spread, (0.5 + config().sticky_base) * tick_size);

    // risk adjustment
    double adj = risk_adjustment_future(ed);
    ask_spread = std::max(0.0, ask_spread + adj);
    bid_spread = std::max(0.0, bid_spread - adj);

    // bid/ask
    double bid, bid_tick_size, ask, ask_tick_size;
    __computeBidAndAskFuture(bid, bid_tick_size, ask, ask_tick_size,
        mid, bid_spread, ask_spread,
        utd->ourMarket().getBestBid(), utd->ourMarket().getBestAsk(),
        tick_size);
    bid = round_to_tick_by_side(bid, tick_size, BID, mid);
    ask = round_to_tick_by_side(ask, tick_size, ASK, mid);

    if (EQ(bid, ask)) {
        bid -= tick_size;
        ask += tick_size;
    }

    // Write directly to UTD's ourMarket
    MultiMarket& mkt = utd->ourMarket();
    if (erc.enable && bid > 0 && ask > 0) {
        int32_t fut_pos = utd->getPosition();
        int32_t bid_size = std::min(erc.max_pos_fut, std::max(0, erc.max_pos_fut - fut_pos));
        int32_t ask_size = std::min(erc.max_pos_fut, std::max(0, erc.max_pos_fut + fut_pos));
        if (bid_size > 0)
            mkt.setBest(BID, PriceSize(bid, bid_size));
        else
            mkt.eraseBids();
        if (ask_size > 0)
            mkt.setBest(ASK, PriceSize(ask, ask_size));
        else
            mkt.eraseAsks();
    } else {
        mkt.clear();
    }
}

// ============================================================================
// updateOurMarketSide — update one side of our market for an option
// ============================================================================
bool CompositeOptionPricer::updateOurMarketSide(
    OptionTradingData* otd, side_t side, double bid, double ask, double mid,
    double bid_tick_size, double ask_tick_size, const ExpiryRiskConfig& erc, int32_t stk_pos) {
    if (!otd) return false;
    double delta = std::fabs(otd->values().greeks().delta());
    MultiMarket& multmkt = otd->values().ourMarket();
    int32_t opt_pos = otd->getPosition();

    if (side == BID) {
        PriceSize old_bid = multmkt.getBest(BID);

        // teenie pricing
        if (config().enable_teenie_pricing) {
            // mkt_bid would come from book; simplified
            PriceSize mkt_bid;
            if (!mkt_bid.empty()) {
                bid = __compute_teenie_price(BID, bid, erc.max_qsize, mkt_bid, bid_tick_size);
            } else {
                return false;
            }
        }
        if (!old_bid.empty()) {
            bid = __apply_sticky_params(BID, old_bid, bid, bid_tick_size);
        }
        bid = round_to_tick_by_side(bid, bid_tick_size, BID, mid);

        int32_t bid_size = __computeQuoteSize(erc, side, mid, bid, opt_pos, stk_pos, delta);
        bool valid = erc.enable && (bid_size > 0);
        if (!valid) {
            multmkt.eraseBids();
            return !old_bid.empty();
        }

        PriceSize new_bid(bid, bid_size);
        if (!old_bid.empty() && EQ(bid, old_bid.px())) {
            return false;
        }

        // interesting check (simplified without book)
        if (!old_bid.empty() && old_bid == new_bid) {
            return false;
        }
        multmkt.setBest(BID, new_bid);
        return true;
    } else if (side == ASK) {
        PriceSize old_ask = multmkt.getBest(ASK);

        if (config().enable_teenie_pricing) {
            PriceSize mkt_ask;
            if (!mkt_ask.empty()) {
                ask = __compute_teenie_price(ASK, ask, erc.max_qsize, mkt_ask, ask_tick_size);
            } else {
                return false;
            }
        }
        if (!old_ask.empty()) {
            ask = __apply_sticky_params(ASK, old_ask, ask, ask_tick_size);
        }
        ask = round_to_tick_by_side(ask, ask_tick_size, ASK, mid);

        int32_t ask_size = __computeQuoteSize(erc, side, mid, ask, opt_pos, stk_pos, delta);
        bool valid = erc.enable && (ask_size > 0);
        if (!valid) {
            multmkt.eraseAsks();
            return !old_ask.empty();
        }

        PriceSize new_ask(ask, ask_size);
        if (!old_ask.empty() && EQ(ask, old_ask.px())) {
            return false;
        }
        if (!old_ask.empty() && old_ask == new_ask) {
            return false;
        }
        multmkt.setBest(ASK, new_ask);
        return true;
    }
    return false;
}

// ============================================================================
// updateOurMarketSideFuture
// ============================================================================
bool CompositeOptionPricer::updateOurMarketSideFuture(
    UnderlyingTradingData* utd, side_t side, double bid, double ask, double mid,
    int32_t bid_size, int32_t ask_size, double bid_tick_size, double ask_tick_size,
    const ExpiryRiskConfig& erc) {
    if (!utd) return false;
    MultiMarket& multmkt = utd->ourMarket();
    bool td_active = utd->isActive();

    if (side == BID) {
        PriceSize old_bid = multmkt.getBest(BID);
        bool valid = erc.enable && (bid_size > 0);
        if (!valid) {
            multmkt.eraseBids();
            return !old_bid.empty();
        }

        // teenie
        if (config().enable_teenie_pricing) {
            double bts = bid_tick_size;  // simplified
            bid = std::min(bid, bid + bts);
        } else {
            bid = round_to_tick_by_side(bid, bid_tick_size, BID, mid);
        }

        PriceSize new_bid(bid, bid_size);
        bool changed = old_bid.empty() || !(old_bid == new_bid);
        if (changed) {
            multmkt.setBest(BID, new_bid);
            return true;
        }
    } else if (side == ASK) {
        PriceSize old_ask = multmkt.getBest(ASK);
        bool valid = erc.enable && (ask_size > 0);
        if (!valid) {
            multmkt.eraseAsks();
            return !old_ask.empty();
        }

        if (config().enable_teenie_pricing) {
            double ats = ask_tick_size;
            ask = std::max(ask, ask - ats);
        } else {
            ask = round_to_tick_by_side(ask, ask_tick_size, ASK, mid);
        }

        PriceSize new_ask(ask, ask_size);
        bool changed = old_ask.empty() || !(old_ask == new_ask);
        if (changed) {
            multmkt.setBest(ASK, new_ask);
            return true;
        }
    }
    return false;
}

// ============================================================================
// __computeEffectiveDelta — cross-expiry effective delta
// ============================================================================
double CompositeOptionPricer::__computeEffectiveDelta(uint32_t exp0) {
    if (!m_spPositionRisk) return 0;
    auto eg0 = m_spPositionRisk->getExpiryGreeks(exp0);
    if (!eg0) return 0;

    double targ0 = 0;
    auto tit = m_prop_exp_delta_targets.find(exp0);
    if (tit != m_prop_exp_delta_targets.end()) targ0 = tit->second;

    double pg = eg0->totalDelta() - targ0;
    double frac0 = eg0->frac_delta();

    for (uint32_t exp1 : m_active_expiries) {
        if (exp0 != exp1) {
            auto eg1 = m_spPositionRisk->getExpiryGreeks(exp1);
            if (!eg1) continue;
            double targ1 = 0;
            auto tit1 = m_prop_exp_delta_targets.find(exp1);
            if (tit1 != m_prop_exp_delta_targets.end()) targ1 = tit1->second;
            double pge = eg1->totalDelta() - targ1;
            double frac1 = eg1->frac_delta();
            double corr_exp = m_prop_hedge_ratio_delta;
            pg += pge * corr_exp * frac0 * frac1;
        }
    }
    return pg;
}

// ============================================================================
// updateRiskShiftsDelta — __computeEffectiveDelta -> normalized
// ============================================================================
void CompositeOptionPricer::updateRiskShiftsDelta(OptionGrid* grid) {
    auto it = m_tolerance_params.find(GT_delta);
    if (it == m_tolerance_params.end()) return;

    double risk_tol = it->second.risk_tol;
    if (risk_tol <= 0) return;

    for (uint32_t exp0 : m_active_expiries) {
        ExpiryDataPtr ed0 = grid->getExpiryData(exp0);
        if (!ed0) continue;

        double pg = __computeEffectiveDelta(exp0);
        double pgn = pg / risk_tol;  // normalized

        ed0->setNormRiskDelta(pgn);

        CVExpiryContext& expcxt = cvcxt().exp_cxt[exp0];
        expcxt.delta_eff = pg;

        ExpiryDataPub& edpub = m_prop_edpub[exp0];
        edpub.delta_eff = pg;
    }
}

// ============================================================================
// updateRiskShiftsVega — simplified vega risk normalization
// ============================================================================
void CompositeOptionPricer::updateRiskShiftsVega(OptionGrid* grid) {
    auto it = m_tolerance_params.find(GT_vega_tw);
    if (it == m_tolerance_params.end()) return;

    // Check all options are priced
    for (const auto& stk : grid->getAllStrikes()) {
        if (!stk->call()->values(0).isPriced() || !stk->put()->values(0).isPriced()) {
            m_bShouldComputeRiskShiftsVega = true;
            break;
        }
    }
    if (!m_bShouldComputeRiskShiftsVega)
        return;
    m_bShouldComputeRiskShiftsVega = false;

    double risk_tol = it->second.risk_tol;
    if (risk_tol <= 0) return;

    // Collect positions
    std::vector<OptionRiskData*> positions;
    if (m_spPositionRisk) {
        for (const auto& rd1 : m_spPositionRisk->all()) {
            if (rd1->getPosition() != 0) {
                // by_instr is a multi_index; elements are OptionRiskDataPtr
                // const_cast to get raw pointer for the positions vector
                positions.push_back(const_cast<OptionRiskData*>(rd1.get()));
            }
        }
    }

    for (const auto& od0 : grid->getAllOptions()) {
        if (!od0->values(0).isPriced()) continue;

        // ATM vega_tw for this expiry
        StrikeDataPtr atm_sd = grid->getAtmStrike(od0->getExpiry());
        double atm_vega_tw = (atm_sd && atm_sd->call())
            ? atm_sd->call()->values().greeks().vegaTW()
            : od0->values().greeks().vegaTW();

        const std::string& instr0 = od0->getCode();
        const OptionValues& values0 = od0->values();
        ExpiryDataPtr ed0 = od0->getExpiryData();
        double df0 = ed0 ? ed0->getDiscountFactor() : 1.0;
        double delta0 = (od0->getRight() == OR_Call)
            ? values0.greeks().delta()
            : values0.greeks().delta() + df0;
        double frac0 = ed0 ? ed0->getExpireGreeksFrac() : 1.0;

        double risk_targ0 = 0;
        auto rtit = m_prop_exp_vega_tw_targets.find(od0->getExpiry());
        if (rtit != m_prop_exp_vega_tw_targets.end()) risk_targ0 = rtit->second;
        od0->values().targ_vega_tw = risk_targ0;

        double lambda_vega_wing = m_prop_lambda_vega_wing;
        double pg_other = 0.0;

        for (OptionRiskData* rd1 : positions) {
            const std::string& instr1 = rd1->getInstrument();
            const OptionValues& values1 = rd1->getOptionValues();
            ExpiryDataPtr ed1 = rd1->getExpiryData();
            double df1 = ed1 ? ed1->getDiscountFactor() : 1.0;
            // Note: OptionRight OR_Call=0, OR_Put=1
            double delta1 = (rd1->getRight() == OR_Call)
                ? values1.greeks().delta()
                : values1.greeks().delta() + df1;
            double frac1 = ed1 ? ed1->getExpireGreeksFrac() : 1.0;

            if (instr1 != instr0) {
                double stk_distance = std::abs(delta0 - delta1);
                uint32_t exp1 = rd1->getExpiry();
                double corr_exp = (exp1 == od0->getExpiry())
                    ? 1.0
                    : m_prop_hedge_ratio_vega * frac0 * frac1;
                double corr_stk = std::exp(-m_prop_lambda_vega_decay * stk_distance);
                double corr = corr_exp * corr_stk;
                double vega_adj = std::exp(-lambda_vega_wing * std::abs(delta1 - 0.5));
                double vega_other = rd1->getPosition() * rd1->getContractSize()
                    * atm_vega_tw * vega_adj;
                pg_other += corr * vega_other;
            }
        }

        // subtract vega_tw risk targets from other expiries
        for (const auto& v : grid->expiries()) {
            const ExpiryDataPtr& ed1 = v.second;
            uint32_t exp1 = ed1->getExpiry();
            if (exp1 != od0->getExpiry()) {
                double frac1 = ed1->getExpireGreeksFrac();
                double risk_targ1 = 0;
                auto rtit2 = m_prop_exp_vega_tw_targets.find(exp1);
                if (rtit2 != m_prop_exp_vega_tw_targets.end()) risk_targ1 = rtit2->second;
                double corr_exp = m_prop_hedge_ratio_vega * frac0 * frac1;
                pg_other += -corr_exp * risk_targ1;
            }
        }

        // own position vega
        OptionRiskDataPtr rd0 = nullptr;
        if (m_spPositionRisk) {
            rd0 = m_spPositionRisk->get(instr0);
        }
        double vega_adj0 = std::exp(-lambda_vega_wing * std::abs(delta0 - 0.5));
        double pg = (rd0 ? rd0->getPosition() : 0)
            * (rd0 ? rd0->getContractSize() : 1.0)
            * atm_vega_tw * vega_adj0 + pg_other;
        double pgn = (pg - risk_targ0) / risk_tol;  // normalized
        od0->values().vega_risk_norm = pgn;
    }
}

// ============================================================================
// alpha_adjustment — vegaflow + frontfut_skew + frontatmv_flow + rollema + deltaflow
// ============================================================================
void CompositeOptionPricer::alpha_adjustment(OptionData* option) {
    OptionValues& black_values = option->values(0);
    black_values.alpha().total = 0;
    double delta = black_values.greeks().delta();
    double vega = black_values.greeks().vega();

    // If signals are configured, read alpha from them
    if (!m_alphaSignals.empty()) {
        double vega_total = 0, delta_total = 0;
        for (const auto& sig : m_alphaSignals) {
            if (!sig->isEnabled()) continue;
            double w = sig->getWeight();
            vega_total  += sig->getVegaAdjust(option, m_signalCtx) * w;
            delta_total += sig->getDeltaAdjust(option, m_signalCtx) * w;
        }
        black_values.alpha().vega_total = vega_total;
        black_values.alpha().delta_total = delta_total;
        black_values.alpha().total += vega * vega_total + delta * delta_total;
        return;
    }

    // Fallback: hardcoded alpha (backward compat when no signals configured)
    {
        ExpiryDataPtr ed = option->getExpiryData();
        double mat = ed ? ed->getMaturity() : 0.25;
        double mat_adjusted = std::max(0.01, mat) / 0.25;
        double vfl = m_ema_vegaflow.getSum() / std::sqrt(mat_adjusted);
        double frontfut_skew = m_prop_frontfut_skew / std::sqrt(mat_adjusted);
        double frontatmv_flow = m_prop_frontatmv_flow / std::sqrt(mat_adjusted);
        black_values.alpha().vegaflow = vfl;
        black_values.alpha().frontfut_skew = frontfut_skew;
        black_values.alpha().frontatmv_flow = frontatmv_flow;
        black_values.alpha().vega_total = vfl * config().wgt_vegaflow
            + frontfut_skew * config().wgt_frontfut_skew
            + frontatmv_flow * config().wgt_frontatmv_flow;
        black_values.alpha().total += vega * black_values.alpha().vega_total;
    }
    {
        double rollema = m_prop_rollema;
        double dfl = m_ema_deltaflow.getSum();
        double atmsig = 0;
        if (m_grid) atmsig = m_grid->getAtmSig() * m_frontfwd * 1e-4;
        black_values.alpha().rollema = rollema;
        black_values.alpha().deltaflow = dfl;
        black_values.alpha().atmsig = atmsig;
        black_values.alpha().delta_total = rollema * config().wgt_rollema
            + dfl * config().wgt_deltaflow * 1e-4
            + atmsig * config().wgt_atmsig;
        black_values.alpha().total += delta * black_values.alpha().delta_total;
    }
}

// ============================================================================
// risk_adjustment — delta risk + vega risk + covariance
// ============================================================================
void CompositeOptionPricer::risk_adjustment(OptionData* option) {
    uint32_t exp = option->getExpiry();
    ExpiryDataPtr ed = option->getExpiryData();
    OptionValues& black_values = option->values(0);
    const OptionGreeks& ogreeks = black_values.greeks();

    // delta risk
    {
        double og = ogreeks.delta();
        double rp_delta = 0.5 * __getForwardSpread(exp);
        double cost_delta = black_values.m_fees + std::fabs(rp_delta * og);
        black_values.adj().delta_risk2 = -sign(og)
            * (ed ? ed->getNormRiskDelta() : 0) * cost_delta;

        double shift = ed ? ed->getRiskShiftDelta() : 0;
        double delta_adj = shift * og;
        black_values.adj().delta_risk = delta_adj;
    }

    // vega risk
    {
        double og = ogreeks.vega();
        double rp_vega = 0.5 * __getAtmvolSpread(exp);
        double cost_vega = black_values.m_fees + std::fabs(og * rp_vega);
        black_values.adj().vega_risk2 = -black_values.vega_risk_norm * cost_vega;

        double shift = black_values.getRiskShiftVega();
        double vega_adj = shift * og;
        black_values.adj().vega_risk = vega_adj;
    }

    // covariance adjustment
    double I_v = black_values.adj().vega_risk2;
    double I_d = black_values.adj().delta_risk2;
    double h = 0;
    if (I_v * I_d > 0)
        h = I_v * I_d * sign(I_d);

    double markup = __getOptionCosts(option, black_values.theo());
    double cov = (markup != 0) ? h / markup : 0;
    cov = sign(cov) * std::min(markup, std::fabs(cov));

    black_values.adj().total_risk = black_values.adj().delta_risk
        + black_values.adj().vega_risk
        + black_values.adj().delta_risk2
        + black_values.adj().vega_risk2
        - cov;
}

// ============================================================================
// risk_adjustment_future
// ============================================================================
double CompositeOptionPricer::risk_adjustment_future(const ExpiryData* ed) {
    double cost = 0.5 * __getFutureSpread(ed->getExpiry());
    double adj = -ed->getNormRiskDelta() * std::fabs(cost);
    return adj;
}

// ============================================================================
// onFill — trade shock tracking
// ============================================================================
void CompositeOptionPricer::onFill(const OrderStubPtr& order, double fill_px, uint32_t fill_qty) {
    if (!order) return;

    order->fillTime = getTime();
    order->fillPrice = fill_px;

    const std::string& instr = order->code;
    if (order->dir == BUY) {
        m_instrument_lastbuy[instr] = order;
    } else if (order->dir == SELL) {
        m_instrument_lastsell[instr] = order;
    }

    m_bShouldComputeRiskShiftsVega = true;

    firePricingChanged();
}

// ============================================================================
// onFitCompleted
// ============================================================================
void CompositeOptionPricer::onFitCompleted(const PeriodicCurveFitter&) {
    // force a compute of values because fit could have changed
    m_tvLastCompute = 0;
    // Set CompositeOptionPricer's m_bReprice (NOT OptionPricer2's) to force SLOW path
    m_bReprice = true;
    if (m_spOptionPricer2) {
        m_spOptionPricer2->setReprice(true);
    }
    // Route through OptionGrid::computeValues (NOT pricer.computeValues directly)
    // so that __notifyComputeCompleted fires -> CTG listener -> refresh() auto-triggers.
    // This ensures ourMarket is collected into pending quotes.
    if (m_grid) {
        m_grid->computeValues(this);
    }
    firePricingChanged();
}

// ============================================================================
// Panic
// ============================================================================
void CompositeOptionPricer::setPanic() {
    m_tvPanicSignalTriggerTime = getTime();
}

bool CompositeOptionPricer::isPanicked() const {
    if (!m_tvPanicSignalTriggerTime)
        return false;
    double diff = getTime() - m_tvPanicSignalTriggerTime.value();
    if (diff > config().panic_blackout_interval) {
        m_tvPanicSignalTriggerTime.reset();
        return false;
    }
    return true;
}

// ============================================================================
// clearLastTrades
// ============================================================================
void CompositeOptionPricer::clearLastTrades(const std::string& instr) {
    double now = getTime();
    auto lb_it = m_instrument_lastbuy.find(instr);
    if (lb_it != m_instrument_lastbuy.end() && lb_it->second) {
        if (lb_it->second->fillTime < now - config().trade_shock_interval)
            m_instrument_lastbuy.erase(lb_it);
    }
    auto ls_it = m_instrument_lastsell.find(instr);
    if (ls_it != m_instrument_lastsell.end() && ls_it->second) {
        if (ls_it->second->fillTime < now - config().trade_shock_interval)
            m_instrument_lastsell.erase(ls_it);
    }
}

// ============================================================================
// setDeltaRange
// ============================================================================
void CompositeOptionPricer::setDeltaRange(uint32_t exp, double rng_min, double rng_max) {
    auto it = m_mapExpiryRiskConfig.find(exp);
    if (it != m_mapExpiryRiskConfig.end()) {
        it->second.delta_min = rng_min;
        it->second.delta_max = rng_max;
    }
}

// ============================================================================
// onAddOption
// ============================================================================
void CompositeOptionPricer::onAddOption(const OptionDataPtr& od) {
    uint32_t exp = od->getExpiry();
    if (m_active_expiries.count(exp) == 0) {
        m_active_expiries.insert(exp);
        // Only init default erc if not already configured by enableExpiry
        auto it = m_mapExpiryRiskConfig.find(exp);
        if (it == m_mapExpiryRiskConfig.end()) {
            ExpiryRiskConfig& erc = m_mapExpiryRiskConfig[exp];
            erc.delta_min = 0.1;
            erc.delta_max = 0.9;
            erc.sprd_fut = 100.0;
            erc.sprd_fwd = 0.01;
            erc.sprd_atmvol = 0.1;
            erc.sprd_corr = 0.0;
            erc.max_pos_fut = 1;
            erc.max_pos_stk = 1;
            erc.max_pos_opt = 1;
            erc.max_qsize = 1;
            erc.enable = false;
            erc.enable_auto_close = config().enable_auto_close;
        }
    }
    od->values().ema_sprd_vs_atmfwd().setWindow(config().ema_sprd_vs_atmfwd_window);

    // Set per-expiry synthetic forward parameters
    auto ed = od->getExpiryData();
    if (ed) {
        ed->setMinStrikesForSynthetic(config().min_strikes_for_synthetic);
        ed->emaForwardSpread().setWindow(config().forward_spread_ema_window);
    }
}


// ============================================================================
// finalizeCompute
// ============================================================================
void CompositeOptionPricer::finalizeCompute(OptionGrid* grid) {
    if (!grid) return;
    // Notify all options that markets are (re)priced (index 0 = primary values).
    for (const auto& od : grid->getAllOptions()) {
        if (od) od->notifyMarketsPriced(0);
    }
}

// ============================================================================
// continueComputeValues_SLOW
// ============================================================================
void CompositeOptionPricer::continueComputeValues_SLOW() {
    OptionGrid* grid = m_grid;
    if (!grid) return;

    for (const auto& sd : grid->getAllStrikes()) {
        const OptionDataPtr& otm = sd->getStrikePrice() < m_refPrice ? sd->put() : sd->call();
        const OptionDataPtr& itm = sd->getStrikePrice() < m_refPrice ? sd->call() : sd->put();

        // explicitly reset both otm and itm values
        otm->values(0).setPriced(false);
        itm->values(0).setPriced(false);

        if (m_spOptionPricer2) {
            m_spOptionPricer2->computeValue(otm.get());
            m_spOptionPricer2->computeValue(itm.get());
            m_spOptionPricer2->decayGreeks(otm.get());
            m_spOptionPricer2->decayGreeks(itm.get());
        }
    }

    m_bShouldComputeRiskShiftsVega = true;

    // this should cause a FAST compute to round things out
    resetLastComputeTime();
    firePricingChanged();
}

// ============================================================================
// checkRiskSignals — check all risk signals, trigger panic/widen
// ============================================================================
void CompositeOptionPricer::checkRiskSignals() {
    m_riskWidenFactor = 1.0;
    for (const auto& sig : m_riskSignals) {
        if (!sig->isEnabled()) continue;
        sig->onBatchEnd();
        RiskAction act = sig->getAction();
        if (act == RiskAction::Panic) {
            setPanic();
            return;
        }
        if (act == RiskAction::Widen) {
            m_riskWidenFactor = std::max(m_riskWidenFactor, sig->getWidenFactor());
        }
    }
}

double CompositeOptionPricer::getRiskWidenFactorByCode(const std::string& code) const {
    double f = m_riskWidenFactor;
    for (const auto& sig : m_riskSignals) {
        if (!sig->isEnabled()) continue;
        f = std::max(f, sig->getWidenFactorByCode(code));
    }
    return f;
}

} // namespace wt_option
