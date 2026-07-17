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
    bool    vol_fitting_to_all_expiries = false;
    int32_t trace_level              = 0;
    bool    use_parallel_for          = false;  // P1: enable via config "use_parallel"
    bool    use_sprd_vs_atmfwd       = true;
    double  slow_compute_interval    = 0.1;   // seconds
    int32_t expire_greeks_window_bdays = 0;
    double  bid_offer_vol_spread_cap = 0.1;  // max IV bid-ask spread (10 vol points), matches quantbox default
    double  volcurve_weight          = 0.0;

    static const char* name() { return "BlackPricer"; }
};
using OptionPricer2ConfigPtr = std::shared_ptr<OptionPricer2Config>;

// ----------------------------------------------------------------------------
// OptionPricer2
// ----------------------------------------------------------------------------
class OptionPricer2 : public IOptionPricer
{
public:
    // fitCompleted event callback (replaces ListenerRelay<...>)
    typedef std::function<void(const PeriodicCurveFitter&)> fitCompletedEventSink;
    typedef std::vector<fitCompletedEventSink> fitCompletedEventSource;

    class ExpiryInfo
    {
    public:
        ExpiryInfo(uint32_t exp);

        void setATMV(double atmv);
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

    // 暴露 doFit 给定时器调用 (路径1: 独立定时 doFit)
    bool hasFitter() const { return m_spFitter != nullptr; }
    bool triggerDoFit();  // 实现在 .cpp 中 (需要 PeriodicCurveFitter 完整类型)

private:
    friend class CompositeOptionPricer;
    friend class CompositeSpreadPricer;
    friend class CompositeOtcPricer;

    const OptionPricer2Config& config() const { return m_config; }

    void setGvvWeight(uint32_t exp, double wgt) { m_gvv_weight[exp] = wgt; }
    void updateGvvParams(uint32_t exp, IVolCurvePtr curve) const;
    void computeTheoreticalValues(OptionData* option, std::shared_ptr<BlackCalculatorInfo> bci);
    bool updateTheoreticalValues(OptionData* option);
    void decayGreeks(OptionData* option);
    void computeImpliedValues(OptionData* option, const BlackCalculatorInfo& bci);

    const BlackCalculatorInfoPtr& __setupBlackCalculatorInfo(OptionData* option);
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
    double m_defaultVolatility = 0.1;
    std::map<uint32_t, double> m_gvv_weight;
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
