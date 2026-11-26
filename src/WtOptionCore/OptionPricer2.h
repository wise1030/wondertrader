/*!
 * \file OptionPricer2.h
 * \brief Black76 / GVV option pricer (migrated from quantbox optioncore/OptionPricer2)
 *
 * Original: longbeach::optioncore::OptionPricer2 inherited MMOptionPricer
 * (which inherited IOptionPricer + CommandServicesHelper), depended on
 * longbeach ClockMonitor, IPriceProvider, ClientContext, CommandServices,
 * PeriodicCurveFitter, GvvVolCurve, OptionPricer2Params_autogen (luabind),
 * notifiable<>, PropertyManager, PerfCounter, ListenerRelay, db::Connection.
 *
 * Migration (dependency-replacement only, business logic preserved):
 *  - namespace longbeach::optioncore -> wt_option
 *  - base class MMOptionPricer -> IOptionPricer (we drop CommandServicesHelper,
 *    which is a longbeach services/plumbing helper; its trace/property plumbing
 *    is collapsed into simple members here)
 *  - longbeach::timeval_t / getTime() -> uint64_t m_time (set externally via
 *    setTime()), since ClockMonitor is gone. getTime() now returns double seconds.
 *  - IPriceProvider removed: forward/refPrice are read directly from ExpiryData
 *    (ExpiryData::getForward()/getMaturity()) as instructed.
 *  - OptionPricer2Params_autogen (luabind codegen) replaced by a plain Config
 *    struct with the same field names used by the .cc logic.
 *  - notifiable<T> -> plain T member (notify plumbing removed; values still
 *    readable/writable).
 *  - PerfCounter -> local PerfCounter (time accumulation in seconds).
 *  - boost::shared_ptr -> std::shared_ptr; boost::make_shared -> std::make_shared.
 *  - boost::bind/tbb kept (wondertrader has tbb in deps).
 *  - MySQL DB read in updateGvvParams() is removed (no db dep); we leave a
 *    stub that callers can override. This is the only behavioural change and
 *    it only affects runtime parameter loading, not pricing math.
 *  - LuaState/registerScripting removed (no luabind).
 *  - friend class CompositeOptionPricer/CompositeSpreadPricer/CompositeOtcPricer
 *    kept as forward declarations.
 *
 * ExpiryInfo::computeForwardPrice now reads ExpiryData::getForward() directly
 * (ExpiryData is set externally) instead of grid->getAtmForward().
 * ExpiryInfo::computeMaturity reads ExpiryData::getMaturity().
 */
#pragma once

#include "IOptionPricer.h"
#include "BlackCalc.h"
#include "OptionGrid.h"
#include "IVolCurve.h"
#include "OptionValues.h"
#include "ExpiryData.h"

#include <map>
#include <memory>
#include <vector>
#include <cstdint>

namespace wt_option {

class OptionGrid;
class OptionRisk;
class StrikeData;
class PeriodicCurveFitter;
class PeriodicCurveFitter;
using PeriodicCurveFitterPtr = std::shared_ptr<PeriodicCurveFitter>;
class GvvVolCurve;

// ----------------------------------------------------------------------------
// Simple PerfCounter (replaces longbeach PerfCounter; accumulates seconds)
// ----------------------------------------------------------------------------
class PerfCounter
{
public:
    PerfCounter() : m_totalTime(0.0), m_running(false), m_start(0.0) {}
    void start() { m_running = true; m_start = 0.0; }
    void stop()  { m_running = false; }
    double getTime() const { return m_totalTime; }
    double getTotalTime() const { return m_totalTime; }
private:
    double m_totalTime;
    bool   m_running;
    double m_start;
};

// PROFILE macro replacement: a RAII guard that calls start/stop.
struct PerfGuard
{
    PerfCounter& pc;
    PerfGuard(PerfCounter& p) : pc(p) { pc.start(); }
    ~PerfGuard() { pc.stop(); }
};
#define PROFILE(pc) PerfGuard __pg_##pc(pc)

// ----------------------------------------------------------------------------
// OptionPricer2::Config  (replaces OptionPricer2Params_autogen)
// ----------------------------------------------------------------------------
class OptionPricer2;

class OptionPricer2Config : public IOptionPricer::ConfigBase
{
public:
    OptionPricer2Config();

    // vol curves (configs). Plain structs; constructed directly.
    IVolCurve::ConfigBasePtr volcurve;
    IVolCurve::ConfigBasePtr volcurve2;
    IVolCurve::ConfigBasePtr fwdcurve;

    // Parameters used by OptionPricer2.cc logic (same names)
    bool    enable_fitter            = false;
    double  vol_fitting_start_time   = 0.0;   // seconds from midnight
    double  vol_fitting_end_time     = 86400.0;
    std::map<int32_t, double> vol_fitting_good_points_thresh;
    double  vol_fitting_decay_window = 3600.0;
    double  vol_fitting_period       = 60.0;
    double  vol_fitting_threshold    = 0.0;
    /// P1: fit-candidate selection — the N VALID points NEAREST to the
    /// forward are fitted (proximity, NOT a delta band: a curve-derived delta
    /// gate is self-referential and converged to an ATM-flat attractor in
    /// replay at every tested threshold).  Dead wings never enter because
    /// only the densest region around the forward is taken; the good-points
    /// gate (need) still decides market readiness.  <= 0 means "no cap".
    int32_t vol_fitting_point_count   = 15;
    bool    vol_fitting_to_all_expiries = false;
    int32_t trace_level              = 0;
    bool    use_parallel_for          = false;  // P1: enable via config "use_parallel"

    /// R2/2026-11-26: dual-curve design, chosen purely by CONFIG —
    ///   curve1 (main): `type` = "gvv" | "power"      (weight=1 pricing)
    ///   curve2:        `curve2_type` = "linear"       (weight=0 pricing)
    /// 0<w<1 blends both.  There is NO runtime fallback: a curve the current
    /// weight requires must fit, or the expiry stays unpublished/not ready.
    /// "constant" is retired (it was the silent fallback stub).
    std::string curve2_type          = "linear";
    /// P1-b: main vol-curve type. "gvv" (default) | "power".
    /// GVV fits a parametric smile (robust for liquid expiries); power fits
    /// the empirical wing law. linear/constant are NOT valid main types.
    std::string volcurve_type        = "gvv";
    // P1 (2026-11-18): WLS wing-weight decay (w = exp(-beta*x^2/(atmvol^2*T))).
    // 0 = equal weights (legacy; deep-wing noise dominates the fit); ~1 lets
    // the ATM neighbourhood drive skew and damps wings to ~0.4% at |x|=15%.
    double      volcurve_beta        = 0.0;
    /// Audit 11-22 (round 3): GVV alpha search-range override (PriceError
    /// weight x*v^(alpha-1), kurt scaled by atmvol^(-2a)). Negative keeps
    /// the GvvVolCurve::Config default [GVV_MINALPHA, GVV_MAXALPHA].
    double      volcurve_alpha_dn    = -1.0;
    double      volcurve_alpha_up    = -1.0;
    /// P2 startup protection (2026-11-26): a newly published curve only
    /// unlocks quoting after this many consecutive accepted publishes
    /// (>= 2 = the first quote always waits one extra fit period, so a
    /// single poisoned snapshot can never arm quoting on its own).
    int32_t volcurve_startup_confirm_versions = 2;
    /// P1 hot-failure protection (2026-11-26): if no accepted publish for an
    /// expiry for longer than this many seconds (grid compute clock), the
    /// curves are no longer "fresh" and readiness drops (stale smile must not
    /// price forever).  0 disables.
    double  volcurve_max_age         = 900.0;
    /// P1-④ (audit 2026-11-18): the ATM vol bootstrap only ran while
    /// curveVersion==0, and publishFittedCurves wrote the snapshot value
    /// back verbatim — the ATM anchor froze at first-fit market level and
    /// drifted from the market afterwards (2701/2702 medDev −2~−4.6% with
    /// wings −54~−79%).  Re-anchor period (seconds, 0 = disabled) and the
    /// max relative step applied per re-anchor toward the fresh median IV.
    double  atm_reanchor_period      = 300.0;
    double  atm_reanchor_max_step    = 0.05;
    bool    use_sprd_vs_atmfwd       = true;
    double  slow_compute_interval    = 0.1;   // seconds
    // FAST incremental update: second-order gamma correction is enabled by
    // default.  A positive forward-step threshold forces full Black repricing
    // when the underlying move is too large for local approximation.
    bool    fast_use_gamma_correction = true;
    double  fast_full_reprice_forward_step = 0.0; // 0 = disabled
    int32_t expire_greeks_window_bdays = 0;
    double  bid_offer_vol_spread_cap = 0.1;  // max IV bid-ask spread (10 vol points), matches quantbox default
    // P1 (2026-11-20): 0.0 = quantbox production default (curve2/linear is
    // the baseline, GVV is an optional per-expiry smoothing layer). COP
    // overwrites this per pass from its own config — keep both aligned.
    double  volcurve_weight          = 0.0;

    static const char* name() { return "BlackPricer"; }
};
using OptionPricer2ConfigPtr = std::shared_ptr<OptionPricer2Config>;

/// P1-b: curve factory shared by OptionPricer2 (initial curves) and
/// AsyncVolFitter (fitted curves).  Unknown type falls back to GVV.
/// powerParams selects the PowerVolCurve parameter set (SPOTVOL/RHO/...).
/// gvvAlphaDn/Up < 0 keep the GvvVolCurve::Config default search range.
IVolCurvePtr createVolCurveByType(const std::string& type, int powerParams = 0,
                                  double gvvBeta = 0.0,
                                  double gvvAlphaDn = -1.0, double gvvAlphaUp = -1.0);

// ----------------------------------------------------------------------------
// OptionPricer2
// ----------------------------------------------------------------------------
class OptionPricer2 : public IOptionPricer
{
public:
    /// Immutable fitted curve set produced by AsyncVolFitter and published by
    /// the single worker thread at a batch boundary.  A null curve (with the
    /// matching version left 0) means "this curve failed / is not required at
    /// the submit-time weight" — the pricer keeps the previous instance of
    /// that curve and never installs over it.
    struct FittedCurveSet {
        uint32_t expiry = 0;
        IVolCurvePtr volCurve;
        IVolCurvePtr volCurve2;
        double atmVol = 0.0;
        uint64_t version = 0;    ///< main curve version (0 = no main update)
        uint64_t version2 = 0;   ///< curve2 version   (0 = no curve2 update)
    };

    // fitCompleted event callback (replaces ListenerRelay<...>)
    typedef std::function<void(const PeriodicCurveFitter&)> fitCompletedEventSink;
    typedef std::vector<fitCompletedEventSink> fitCompletedEventSource;

    class ExpiryInfo
    {
    public:
        ExpiryInfo(uint32_t exp);

        void setATMV(double atmv);
        /// Scale-correction variant (2026-11-26): same as setATMV but the
        /// stored curve normalizations are re-based by oldAnchor/newAnchor —
        /// used ONLY by the bootstrap seed, where the anchor move corrects
        /// our estimate rather than tracking the market.
        void setATMVReseed(double atmv);
        // Reads ExpiryData::getForward() directly (no IPriceProvider / grid->getAtmForward)
        void computeForwardPrice(OptionGrid* grid, const ExpiryDataPtr& ed);
        void computeMaturity(const ExpiryData* ed);

        uint32_t    m_expiry;
        IVolCurvePtr m_spVolCurve;
        IVolCurvePtr m_spVolCurve2;
        IVolCurvePtr m_spFwdCurve;

        ExpiryData* m_spExpiryData = nullptr;   // non-owning back-pointer (optional)
        double      m_atmforward;
        double      m_atmvol;
        double      m_maturity;
        double      m_settleFrac;

        double      m_futsprd;
        double      m_atmvolsprd;
        // notifiable<double> replaced by plain double
        double      m_prop_atmv;
        double      m_prop_atmfwd;
    };
    using ExpiryInfoPtr = std::shared_ptr<ExpiryInfo>;

    class BlackCalculatorInfo;
    using BlackCalculatorInfoPtr = std::shared_ptr<BlackCalculatorInfo>;

    // --- Construction ---
    // ClientContext / CommandServices removed. Takes grid + risk + config only.
    OptionPricer2(const OptionPricer2Config& c,
                  OptionGrid* grid,
                  OptionRisk* risk);
    virtual ~OptionPricer2();

    const ExpiryInfoPtr& getExpiryInfo(uint32_t exp) const;
    virtual IVolCurvePtr getVolCurve(uint32_t exp) const override;
    virtual IVolCurvePtr getVolCurve2(uint32_t exp) const override;
    virtual IVolCurvePtr getFwdCurve(uint32_t exp) const override;

    virtual double getMaturity(uint32_t exp) const override;
    virtual double getATMForward(uint32_t exp) const override;

    virtual void   setATMVol(uint32_t exp, double atmvol) override;
    // v3-GARCH-lite: blend a second ATM-vol anchor with the fitted atmVol.
    // weight=0 (default) = fitted only; vol<=0 = estimator warming up → fitted.
    void setGarchBlend(double weight, double vol) {
        m_garchWeight = weight;
        m_garchVol = vol;
    }
    virtual double getATMVol(uint32_t exp) const override;

    virtual double getFutSprd(uint32_t exp) const override;
    virtual double getATMVolSprd(uint32_t exp) const override;
    virtual void   setReprice(bool bReprice) override { m_bReprice = bReprice; }

    virtual bool computeValues(OptionGrid* grid) override;
    virtual bool computeImpliedValues(OptionGrid* grid) override;

    virtual bool initValuesCompute(OptionGrid* grid) override;
    virtual void computeValue(OptionData* option) override;
    virtual void finalizeCompute(OptionGrid* grid) override;

    // IOptionPricer leftovers
    virtual bool isPanicked() const override { return false; }
    virtual void setTraceLevel(int32_t i) override { m_traceLevel = i; }
    virtual int32_t getTraceLevel() const override { return m_traceLevel; }

    // Time (replaces ClockMonitor). Set externally each tick.
    double getTime() const { return m_time; }
    void   setTime(double t) { m_time = t; }

    fitCompletedEventSource& fitCompletedEvent() { return m_fitCompletedEvent; }

    /// Publish async-fit curves at a worker batch boundary.  Per-curve
    /// version gating: each curve installs only if its own version advances
    /// and the pointer is non-null; the ATM anchor is re-based (never
    /// reverted) when the snapshot was taken at a different anchor.
    /// This is the only curve mutation path for AsyncVolFitter.
    void publishFittedCurves(const std::vector<FittedCurveSet>& fitted);
    uint64_t getCurveVersion(uint32_t exp) const;
    uint64_t getCurve2Version(uint32_t exp) const;
    /// max(main, curve2) — "has this expiry ever accepted ANY fitted curve".
    /// Drives the ATM bootstrap / re-anchor branch selection: a weight=0
    /// expiry whose main curve never fits must still leave the bootstrap
    /// window once its linear curve publishes.
    uint64_t getAnyCurveVersion(uint32_t exp) const;

    /// Effective blend weight for an expiry (per-expiry override wins over
    /// the config default).  Read by the vol-fit submit path.
    double getGvvWeight(uint32_t exp) const;

    /// Bootstrap-seed the ATM anchor with curve re-basing: this path corrects
    /// OUR scale estimate (the market did not move), so stored curve
    /// multipliers are rescaled by old/new to keep the absolute vol they
    /// encode unchanged.  Contrast with setATMVol (re-anchor), which follows
    /// the market and deliberately moves theo with the anchor.
    void setATMVolReseed(uint32_t exp, double v);

    /// Hot blend-weight entry (SETPARAM volcurve_weight / per-expiry
    /// override): re-evaluates dynamic readiness immediately, so a switch to
    /// a weight whose required curve is missing/never-fitted drops the expiry
    /// out of quoting at once.
    void setGvvWeight(uint32_t exp, double wgt);

    // 暴露 doFit 给定时器调用 (路径1: 独立定时 doFit)
    bool hasFitter() const { return m_spFitter != nullptr; }
    bool triggerDoFit();  // 实现在 .cpp 中 (需要 PeriodicCurveFitter 完整类型)
    const OptionPricer2Config& config() const { return m_config; }

private:
    friend class CompositeOptionPricer;
    friend class CompositeSpreadPricer;
    friend class CompositeOtcPricer;

    void updateGvvParams(uint32_t exp, IVolCurvePtr curve) const;
    /// Dynamic readiness (L4): required-curves-available && startup-confirm &&
    /// publish-freshness, evaluated against the CURRENT blend weight.  Writes
    /// ExpiryData::curvesReady; safe to call repeatedly.
    void refreshCurvesReady(uint32_t exp);
    bool hasRequiredCurves(uint32_t exp) const;
    void computeTheoreticalValues(OptionData* option, std::shared_ptr<BlackCalculatorInfo> bci);
    bool updateTheoreticalValues(OptionData* option);
    void decayGreeks(OptionData* option);
    void computeImpliedValues(OptionData* option, const BlackCalculatorInfo& bci);

    const BlackCalculatorInfoPtr& __setupBlackCalculatorInfo(OptionData* option);
    /// P1-c: per-strike forward spread vs the expiry ATM forward.  Resolution
    /// order: externally fitted fwd-spread curve (m_spFwdCurve) → per-strike
    /// parity synthetic forwards from the grid → this option's own EMA.
    /// Returns 0 when use_sprd_vs_atmfwd is disabled or nothing is available.
    double __forwardSpreadFor(OptionData* option, double atmForward) const;
    /// P1-④ (audit 2026-11-18): median near-ATM (±3% moneyness) market IV
    /// shared by the curveVersion==0 bootstrap and the periodic re-anchor.
    /// Returns NAN when fewer than 3 valid IVs are available.
    double __bootstrapMedianIV(const ExpiryDataPtr& ed, OptionGrid* grid);
    void __doImplied_imp(OptionData* od);
    void __doImplied(const OptionDataPtr& od);
    void __doImpliedStrikes(const StrikeDataPtr& sd);
    void __doImpliedVec(const std::vector<OptionData*>& v, int32_t i);
    void __doImpliedStrikeVec(const std::vector<StrikeData*>& v, int32_t i);

    void __computeValue(OptionData* otm, OptionData* itm);
    void __doComputeValueVec(const std::vector<OptionData*>& v, int32_t i);
    void __doComputeValueStrikeVec(const std::vector<StrikeData*>& v, double refpx, int32_t i);

private:
    OptionPricer2Config m_config;
    PerfCounter m_perfCounter;
    OptionGrid*   m_grid = nullptr;
    OptionRisk*   m_spPositionRisk = nullptr;
    mutable std::map<uint32_t, ExpiryInfoPtr> m_expiryInfoTable;
    /// Per-curve publish versions (2026-11-26 decoupling): a weight=0 expiry
    /// can publish linear-only for hours while the main curve never fits.
    std::map<uint32_t, uint64_t> m_curveVersion;    ///< main curve
    std::map<uint32_t, uint64_t> m_curve2Version;   ///< curve2
    /// Accepted-publish counter / last publish time per expiry — drive the
    /// startup-confirm and max-age readiness terms.
    std::map<uint32_t, uint64_t> m_publishCount;
    std::map<uint32_t, double> m_publishTime;
    /// P1-④: per-expiry last ATM re-anchor time (grid compute-time seconds;
    /// absent entry = anchor at next check, i.e. wait one full period first).
    std::map<uint32_t, double> m_lastReanchor;
    double m_defaultVolatility = 0.1;
    std::map<uint32_t, double> m_gvv_weight;
    // v3-GARCH-lite blend state (worker-owned)
    double m_garchWeight = 0.0;
    double m_garchVol = 0.0;
    double m_prop_atmsig = 0.0;
    int32_t m_traceLevel = 0;

    bool   m_bReprice = true;
    double m_tvLastCompute = 0.0;
    double m_tvLastSlowCompute = 0.0;
    double m_tvLastComputeImplied = 0.0;
    double m_time = 0.0;   // current wall-clock seconds, set by setTime()

    std::shared_ptr<PeriodicCurveFitter> m_spFitter;
    fitCompletedEventSource m_fitCompletedEvent;

    // valuesIndex for double-buffered OptionValues; COP uses 0.
    int32_t m_valuesIndex = 0;
    int32_t getValuesIndex() const { return m_valuesIndex; }
public:
    void setValuesIndex(int32_t i) { m_valuesIndex = i; }
};
using OptionPricer2Ptr = std::shared_ptr<OptionPricer2>;

// ----------------------------------------------------------------------------
// BlackCalculatorInfo (nested; defined here for shared_ptr completeness)
// ----------------------------------------------------------------------------
class OptionPricer2::BlackCalculatorInfo
{
public:
    OptionType   m_optionType = OT_Call;
    double       m_strike = 0.0;
    double       m_forward = 0.0;
    double       m_maturity = 0.0;
    double       m_settleFrac = 0.0;
    double       m_discount = 1.0;
    OptionPricer2::ExpiryInfoPtr ei;
};

} // namespace wt_option
