/*!
 * \file CompositeOptionPricer.h
 * \brief Composite option pricer with FAST/SLOW compute + ourMarkets logic
 *        (migrated from quantbox optioncore/CompositeOptionPricer, 388h + 2647cc)
 *
 * This is the heart of the option market-making pricer. Every business method
 * is preserved 1:1; only longbeach framework dependencies are replaced.
 *
 * Original dependencies removed:
 *  - longbeach::clientcore::ClockMonitor / TickProvider / IBook / ICandlestickListener
 *  - longbeach::stratlib::{CommandServicesHelper, notifiable, PropertyManager,
 *    MarketUpdateLog, MultiMarket}
 *  - longbeach::trading::IFillListener / Order
 *  - longbeach::signals::ISignal
 *  - longbeach::optioncore::CompositeOptionPricerParams_autogen (luabind codegen)
 *  - longbeach::clientcore::ClientContext
 *  - longbeach::math::EMAFilter
 *
 * Migration:
 *  - namespace longbeach::optioncore -> wt_option
 *  - base MMOptionPricer -> IOptionPricer (CommandServicesHelper dropped)
 *  - trading::IFillListener / ITickListener / OptionGridListener kept as
 *    abstract bases (wt_option versions)
 *  - ClientContext/ClockMonitor removed; getTime() reads m_time (double seconds)
 *  - OptionPricer2Params_autogen -> plain CompositeOptionPricerConfig struct
 *  - notifiable<T> -> plain T member (notify plumbing removed)
 *  - boost::shared_ptr -> std::shared_ptr
 *  - boost::function -> std::function
 *  - boost::optional<timeval_t> -> std::optional<double>
 *  - instrument_t -> std::string; expiry_t -> uint32_t
 *  - EMAFilter -> wt_option::EMAFilter (from OptionValues.h)
 *  - math::sign -> local helper
 *  - LpSolver::str2greek kept as a static helper (GreekType mapping)
 *  - PeriodicCurveFitter forward-declared; onFitCompleted takes const ref
 *
 * NOT-YET-MIGRATED siblings referenced via forward declarations:
 *  - OptionTradingData / OptionTradingDataPtr
 *  - UnderlyingTradingData / UnderlyingTradingDataPtr
 *  - ExpiryTradingData / ExpiryTradingDataPtr
 *  - OptionRiskData / OptionRiskDataPtr
 *  - OptionExpiryGreeks / OptionExpiryGreeksPtr
 *  - InstrumentMDContext / InstrumentMDContextPtr
 *  - OptionOrderInfo / FutureOrderInfo
 *  - MarketLevel / Market / PriceSize (Partial: PriceSize in OptionValues.h)
 *  - LpSolver
 *  These are stubbed where touched by computeOurMarkets; the host will
 *  provide concrete implementations.
 *
 * All computeOurMarkets sub-logic preserved:
 *  - computeValues FAST/SLOW scheduling
 *  - updateOurMarketSide / __computeBidAndAsk / __computeQuoteSize
 *  - __compute_interesting / __compute_teenie_price
 *  - risk_adjustment / alpha_adjustment
 *  - updateRiskShiftsDelta / updateRiskShiftsVega
 *  - updateDistortValues / updateTheoreticalValuesFuture
 *  - distort / riskShift / alpha / sticky / teenie / interesting / back-away
 */
#pragma once

#include "IOptionPricer.h"
#include "IVolCurve.h"
#include "OptionValues.h"
#include "optioncoretypes.h"
#include "IOptionGridListener.h"
#include "OptionPricer2.h"
#include "Signals/ISignal.h"

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <cstdint>

namespace wt_option {

// Forward declarations of not-yet-migrated siblings -------------------------
class OptionGrid;
class OptionRisk;
class OptionPricer2;
class PeriodicCurveFitter;
class StrikeData;
class OptionData;
class ExpiryData;
class OptionGreeks;
using OptionGreeksCPtr = std::shared_ptr<const OptionGreeks>;

class OptionTradingData;        using OptionTradingDataPtr = std::shared_ptr<OptionTradingData>;
class UnderlyingTradingData;    using UnderlyingTradingDataPtr = std::shared_ptr<UnderlyingTradingData>;
class ExpiryTradingData;        using ExpiryTradingDataPtr = std::shared_ptr<ExpiryTradingData>;
class OptionRiskData;           using OptionRiskDataPtr = std::shared_ptr<OptionRiskData>;
class OptionExpiryGreeks;       using OptionExpiryGreeksPtr = std::shared_ptr<OptionExpiryGreeks>;
class InstrumentMDContext;      using InstrumentMDContextPtr = std::shared_ptr<InstrumentMDContext>;
class LpSolver;

// OrderStub for fill tracking (trade-shock back-away) ------------------------
struct OrderStub {
    double fillPrice = 0;
    double fillTime = 0;
    std::string code;
    int dir = 0; // 0=buy, 1=sell
};
using OrderStubPtr = std::shared_ptr<OrderStub>;

// Side / direction enums (replaces longbeach side_t / dir_t) -----------------
enum side_t { BID = 0, ASK = 1 };
enum dir_t  { BUY = 0, SELL = 1 };
inline int dir2sign(dir_t d) { return d == BUY ? 1 : -1; }

// QuoteMode constants (mirror longbeach ON/OFF/AUTO/CLOSE/FLAT)
constexpr int QM_OFF_val   = -1;
constexpr int QM_AUTO_val  = 0;
constexpr int QM_ON_val    = 1;
constexpr int QM_CLOSE_val = 2;
constexpr int QM_FLAT_val  = 3;

// ----------------------------------------------------------------------------
// CompositeOptionPricerConfig (replaces CompositeOptionPricerParams_autogen)
// ----------------------------------------------------------------------------
class CompositeOptionPricerConfig : public IOptionPricer::ConfigBase
{
public:
    CompositeOptionPricerConfig();

    // black pricer sub-config
    OptionPricer2ConfigPtr black_params;

    // alpha weights
    double wgt_vegaflow        = 0.0;
    double wgt_frontfut_skew   = 0.0;
    double wgt_frontatmv_flow  = 0.0;
    double wgt_rollema         = 0.0;
    double wgt_deltaflow       = 0.0;
    double wgt_atmsig          = 0.0;
    double wgt_sizebias        = 0.0;

    // sticky / teenie
    double sticky_base         = 0.0;
    double improve_retreat_ratio = 1.0;
    bool   enable_teenie_pricing = false;

    // EMA windows (seconds)
    double ema_roll_front_fut_window = 120.0;
    double vegaflow_window           = 120.0;
    double frontatmv_flow_window     = 120.0;
    double frontfut_skew_window      = 120.0;
    double deltaflow_window          = 120.0;
    double ema_sprd_vs_atmfwd_window = 120.0;
    double forward_spread_ema_window  = 120.0;
    int    min_strikes_for_synthetic = 5;

    // compute cadence
    double slow_compute_interval   = 15.0;
    double trade_shock_interval    = 60.0;
    double panic_blackout_interval = 300.0;

    // risk premia (commented in original; defaults 0)
    double risk_prem_opt = 0.0;
    double risk_prem_fut = 0.0;
    double rp_quote      = 0.0;

    // close
    bool    enable_auto_close = false;
    int32_t close_pos_thresh = 1;
    int32_t reduce_large_size = 100;
    int32_t interesting_ticks_opt = 5;
    int32_t interesting_ticks_fut = 5;

    // hedge ratios
    double hedge_ratio_delta = 0.0;
    double hedge_ratio_vega  = 0.0;
    double lambda_vega_decay = 0.0;
    double lambda_vega_wing  = 0.0;

    // scanner bookahead thresholds
    int32_t max_size_ahead         = 100;
    int32_t max_size_ahead_inside  = 50;

    int32_t trace_level = 0;

    // GVV curve blend weight (0=use fitted curve only, 1=use GVV parametric only)
    double volcurve_weight = 0.0;
};
using CompositeOptionPricerConfigPtr = std::shared_ptr<CompositeOptionPricerConfig>;

// ----------------------------------------------------------------------------
// CompositeOptionPricer
// ----------------------------------------------------------------------------
class CompositeOptionPricer
    : public IOptionPricer
    , public OptionGridListener
{
public:
    struct bo_data
    {
        double bid = 0;
        double ask = 0;
        double qbid = 0;
        double qask = 0;
        double qbid_tick_size = 0;
        double qask_tick_size = 0;
    };

public:
    CompositeOptionPricer(const CompositeOptionPricerConfig& c,
                          OptionGrid* grid,
                          OptionRisk* risk);
    virtual ~CompositeOptionPricer();

    const OptionPricer2Ptr& getBlackPricer() { return m_spOptionPricer2; }
    void setBlackPricer(const OptionPricer2Ptr& p) { m_spOptionPricer2 = p; }

    // Set expiry risk config (called by strategy to enable quoting)
    void enableExpiry(uint32_t exp, double deltaMin = 0.0, double deltaMax = 1.0);
    void setMaxPosQty(uint32_t exp, int32_t maxQsize, int32_t maxPosStk, int32_t maxPosOpt);
    void setExpiryCloseParams(uint32_t exp, bool autoClose, int32_t closeThresh);

    // --- IOptionPricer ---
    virtual IVolCurvePtr getVolCurve(uint32_t exp) const override;
    virtual IVolCurvePtr getVolCurve2(uint32_t exp) const override;
    virtual double getVol(uint32_t expiry, strike_t strike) const;
    virtual IVolCurvePtr getFwdCurve(uint32_t exp) const override;
    virtual double getATMForward(uint32_t exp) const override;
    virtual double getMaturity(uint32_t exp) const override;
    virtual void   setATMVol(uint32_t exp, double atmvol) override;
    virtual double getATMVol(uint32_t exp) const override;

    virtual void setReprice(bool bReprice) override { m_bReprice = bReprice; }

    virtual bool computeValues(OptionGrid* grid) override;
    virtual bool computeImpliedValues(OptionGrid* grid) override;

    virtual bool initValuesCompute(OptionGrid* grid) override;
    virtual void computeValue(OptionData* option) override;
    void decayGreeks(OptionData* option);
    virtual void computeOurMarkets(OptionData* option, StrikeData* sdata);
    void         computeOurMarketsFuture(ExpiryData* ed);
    virtual void finalizeCompute(OptionGrid* grid) override;

    virtual bool isPanicked() const override;
    virtual void setTraceLevel(int32_t i) override { m_traceLevel = i; }
    virtual int32_t getTraceLevel() const override { return m_traceLevel; }

    void setDeltaRange(uint32_t exp, double rng_min, double rng_max);
    void setInstrumentLastBuy(const std::string& instr, const OrderStubPtr& order) { m_instrument_lastbuy[instr] = order; }
    void setInstrumentLastSell(const std::string& instr, const OrderStubPtr& order) { m_instrument_lastsell[instr] = order; }

    void resetLastComputeTime();

    // time (replaces ClockMonitor)
    double getTime() const { return m_time; }
    void setTime(double t) {
        m_time = t;
        if (m_spOptionPricer2)
            m_spOptionPricer2->setTime(t);  // 同步给 OptionPricer2
    }

private:
    const CompositeOptionPricerConfig& config() const { return m_config; }

    void setupActiveExpiries();
    void setupRiskTolerances();

    void setPanic();
    void onPanic();   // original took const signals::ISignal&; signal removed

    bool updateDistortValues(OptionData* option);
    bool updateDistortValuesFuture(ExpiryData* ed);
    bool updateTheoreticalValuesFuture(ExpiryData* ed);

    void __computeBidAndAskFuture(double& bid, double& bid_tick_size, double& ask, double& ask_tick_size
        , double mid, double bid_spread, double ask_spread
        , const PriceSize& ourmkt_bid, const PriceSize& ourmkt_ask
        , double tick_size = 1.0);
    void __computeBidAndAsk(bo_data* out
        , double mid, double bid_spread, double ask_spread
        , const PriceSize& ourmkt_bid, const PriceSize& ourmkt_ask
        , OptionData* od);

    class ExpiryRiskConfig;
    bool updateOurMarketSide(OptionTradingData* otd
        , side_t side, double bid, double ask, double mid
        , double bid_tick_size, double ask_tick_size
        , const ExpiryRiskConfig& erc, int32_t stk_pos);
    bool updateOurMarketSideFuture(UnderlyingTradingData* utd
        , side_t side
        , double bid, double ask, double mid, int32_t bid_size, int32_t ask_size
        , double bid_tick_size, double ask_tick_size
        , const ExpiryRiskConfig& erc);

    void clearLastTrades(const std::string& instr);

    void updateRiskShiftsDelta(OptionGrid* grid);
    void updateRiskShiftsVega(OptionGrid* grid);

    void alpha_adjustment(OptionData* option);
    void risk_adjustment(OptionData* option);
    double risk_adjustment_future(const ExpiryData* ed);

    void onFill(const OrderStubPtr& order, double fill_px, uint32_t fill_qty);

    // Public access for strategy to trigger onFill from on_trade callback
public:
    void triggerOnFill(const OrderStubPtr& order, double fill_px, uint32_t fill_qty) {
        onFill(order, fill_px, fill_qty);
    }

    // Fit interval config (fit triggered by OptionPricer2::triggerDoFit)
    void setFitInterval(double seconds) { m_fitInterval = seconds; }

    // Signal management
    void setAlphaSignals(std::vector<IAlphaSignal::Ptr> sigs) { m_alphaSignals = std::move(sigs); }
    void setRiskSignals(std::vector<IRiskSignal::Ptr> sigs) { m_riskSignals = std::move(sigs); }
    const std::vector<IAlphaSignal::Ptr>& getAlphaSignals() const { return m_alphaSignals; }
    const std::vector<IRiskSignal::Ptr>& getRiskSignals() const { return m_riskSignals; }
    double getRiskWidenFactor() const { return m_riskWidenFactor; }
    double getRiskWidenFactorByCode(const std::string& code) const;
    void checkRiskSignals();

private:
    void onFitCompleted(const PeriodicCurveFitter& fitter);

    void computeValues_FAST(OptionGrid* grid);
    void computeValues_SLOW(OptionGrid* grid);
    void continueComputeValues_SLOW();

    virtual void onAddOption(const OptionDataPtr& od) override;

    static bool __compute_interesting(
        side_t s, double new_px, double mkt_px
        , double interesting_cutoff
        , const CompositeOptionPricerConfig& config);
    static double __compute_teenie_price(
        side_t dir, double our_px, int32_t our_sz, const PriceSize& mkt, double tick_size);

private:
    double __apply_sticky_params(
        side_t s, const PriceSize& our_q, double new_px, double tick_size);
    double __getOptionCosts(OptionData* option, double mid);
    double __getFutureMarkup(uint32_t exp, double mid);
    double __getForwardSpread(uint32_t exp);
    double __getFutureSpread(uint32_t exp);
    double __getAtmvolSpread(uint32_t exp);
    void   __setShouldComputeRiskShiftsVega(bool b) { m_bShouldComputeRiskShiftsVega = b; }
    int32_t __computeQuoteSize(const ExpiryRiskConfig& erc
        , side_t side, double mid, double px
        , int32_t opt_pos, int32_t stk_pos, double delta);
    double __computeEffectiveDelta(uint32_t exp0);

private:
    const CompositeOptionPricerConfig m_config;
    OptionGrid*   m_grid = nullptr;
    OptionRisk*   m_spPositionRisk = nullptr;
    OptionGreeksCPtr m_spPositionGreeks;
    OptionPricer2Ptr m_spOptionPricer2;

    bool m_bReprice = true;
    // Fit interval (fit triggered by OptionPricer2::triggerDoFit, not COP)
    double m_fitInterval = 60.0;  // seconds (default 1 min, matching quantbox)
    double m_refPrice = 0;
    double m_tvLastCompute = 0;
    double m_tvLastSlowCompute = 0;
    double m_time = 0;
    int32_t m_traceLevel = 0;

    UnderlyingTradingDataPtr m_frontUnderlier;
    double m_frontfut = 0;
    double m_frontfwd = 0;
    double m_frontatmv = 0;
    EMAFilter m_ema_roll_front_fut;
    EMAFilter m_ema_vegaflow;
    EMAFilter m_ema_frontatmv;
    EMAFilter m_ema_frontfut;
    EMAFilter m_ema_deltaflow;

    std::set<uint32_t> m_active_expiries;

    struct ToleranceParams {
        double risk_tol = 0;
    };
    std::map<GreekType, ToleranceParams> m_tolerance_params;
    std::map<uint32_t, double> m_prop_exp_delta_targets;
    std::map<uint32_t, double> m_prop_exp_vega_tw_targets;

    std::map<std::string, OrderStubPtr> m_instrument_lastbuy;
    std::map<std::string, OrderStubPtr> m_instrument_lastsell;
    int32_t m_prop_trade_shock_ticks = 1;
    int32_t m_prop_interesting_ticks_option = 5;
    int32_t m_prop_interesting_ticks_future = 5;

    int32_t m_numcxl = 0, m_numrej = 0, m_numpos = 0, m_numfil = 0;
    std::map<uint32_t, int32_t> m_exp_numcxl, m_exp_numrej, m_exp_numpos, m_exp_numfil;
    int32_t m_prop_numcxl = 0, m_prop_numrej = 0, m_prop_numpos = 0, m_prop_numfil = 0;

    double m_prop_pfdelta = 0;
    double m_prop_pfgamma = 0;
    int32_t m_prop_pfvega = 0;
    double m_prop_vegaflow = 0;
    double m_prop_frontatmv_flow = 0;
    double m_prop_frontfut_skew = 0;
    double m_prop_deltaflow = 0;
    double m_prop_rollema = 0;

    double m_prop_hedge_ratio_delta = 0;
    double m_prop_hedge_ratio_vega = 0;
    double m_prop_lambda_vega_decay = 0;
    double m_prop_lambda_vega_wing = 0;
    int32_t m_prop_close_pos_thresh = 1;
    uint32_t m_prop_primary_exp = 0;

    bool m_bShouldComputeRiskShiftsVega = true;

    mutable std::optional<double> m_tvPanicSignalTriggerTime;

    class ExpiryRiskConfig
    {
    public:
        void init(uint32_t exp, const CompositeOptionPricerConfig& config, bool bEnable);

        bool    enable = false;
        bool    enable_auto_close = false;
        int32_t close_pos_thresh = 0;
        double  delta_min = 0;
        double  delta_max = 0;

        int32_t max_pos_fut = 0;
        int32_t max_pos_stk = 0;
        int32_t max_pos_opt = 0;
        int32_t max_qsize = 0;

        int32_t numcxl = 0, numrej = 0, numpos = 0, numfil = 0;

        double  sprd_fut = 0;
        double  sprd_fwd = 0;
        double  sprd_atmvol = 0;
        double  sprd_corr = 0;
    };
    std::unordered_map<uint32_t, ExpiryRiskConfig> m_mapExpiryRiskConfig;

    class CVExpiryContext
    {
    public:
        double pos_delta = 0;
        double pos_vega = 0;
        double expire_frac = 0;
        double delta_eff = 0;
        double primary_future_spread = 0;
    };
    class CVContext
    {
    public:
        std::map<uint32_t, CVExpiryContext> exp_cxt;
    } m_cvContext;
    CVContext& cvcxt() { return m_cvContext; }

    // ExpiryDataPub (display properties stripped to plain values)
    struct ExpiryDataPub {
        int32_t delta = 0;
        int32_t delta_eff = 0;
        int32_t vega_tw = 0;
        int32_t numcxl = 0, numrej = 0, numpos = 0, numfil = 0;
    };
    std::map<uint32_t, ExpiryDataPub> m_prop_edpub;

    // Signals
    std::vector<IAlphaSignal::Ptr> m_alphaSignals;
    std::vector<IRiskSignal::Ptr>  m_riskSignals;
    SignalContext m_signalCtx;
    double m_riskWidenFactor = 1.0;
};
using CompositeOptionPricerPtr = std::shared_ptr<CompositeOptionPricer>;

} // namespace wt_option
