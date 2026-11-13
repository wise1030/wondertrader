/*!
 * \file HftOptionStrategy.h
 * \brief Option MM Strategy - HFT framework
 *
 * Connects: OptionGrid -> CompositeOptionPricer -> ControllableTradingGrid -> IHftStraCtx
 * Full chain: on_tick -> grid.onTick(discovery) -> computeValues -> CTG.refresh -> quote/buy/sell
 */
#pragma once

#include "../Includes/HftStrategyDefs.h"
#include "../Includes/IHftStraCtx.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "OptionAsyncEventProcessor.h"
#include "OptionTimeService.h"
#include "OptionConfigLoader.h"
#include "LatencyHistogram.h"
#include "AsyncVolFitter.h"
#include "OrderGateway.h"
#include "PositionBook.h"
#include "OptionQuoteManager.h"
#include "Scanners/IScanModule.h"
#include "OptionScanCoordinator.h"
#include "OptionHedgeCoordinator.h"
#include "OptionExpiryLifecycle.h"
#include "OptionCommandHandlers.h"
#include "OptionPricerMode.h"
#include "AttributePublisher.h"
#include "ExpirationSimulator.h"
#include "OptionValueWriter.h"
#include "Predictor.h"
#include "ComboOrders.h"
#include "RiskFilterChain.h"
#include "PositionOffsetMgr.h"
#include "PositionGuard.h"
#include "FillPriceChecker.h"
#include "RiskLimitsEx.h"
#include "QuoteStatistics.h"
#include "GammaScalpOptionPricer.h"
#include "IssuedOrderRegistry.h"
#include "TradingState.h"
#include "CommandBus.h"
#include "DeltaHedgeExecutor.h"
#include "PredictionEvaluator.h"
#include "ConfigValidator.h"
#include "StressAnalyzer.h"

#include <string>
#include <memory>
#include <atomic>
#include <set>
#include <unordered_set>
#include <mutex>
#include <unordered_map>
#include <map>
#include <vector>
#include <utility>
#include <chrono>

namespace wt_option {
class OptionGrid;
class OptionTradingGrid;
class CompositeOptionPricer;
class ControllableTradingGrid;
class OptionRisk;
class OptionQuoteManager;
struct OptionTraderContext;
using OptionGridPtr = std::shared_ptr<OptionGrid>;
using OptionTradingGridPtr = std::shared_ptr<OptionTradingGrid>;
using CompositeOptionPricerPtr = std::shared_ptr<CompositeOptionPricer>;
using GammaScalpOptionPricerPtr = std::shared_ptr<GammaScalpOptionPricer>;
using ControllableTradingGridPtr = std::shared_ptr<ControllableTradingGrid>;
using OptionRiskPtr = std::shared_ptr<OptionRisk>;
class OptionPricer2;
class IScanModule;
}

class HftOptionStrategy : public HftStrategy
{
public:
    HftOptionStrategy(const char* name);
    virtual ~HftOptionStrategy();
    void shutdown();  // explicit teardown: stop async + remove listeners

    virtual const char* getName() override { return "OptionMM"; }
    virtual const char* getFactName() override { return "OptionStraFact"; }

    virtual bool init(WTSVariant* cfg) override;
    virtual void on_init(IHftStraCtx* ctx) override;
    virtual void on_tick(IHftStraCtx* ctx, const char* code, WTSTickData* newTick) override;
    virtual void on_trade(IHftStraCtx* ctx, uint32_t localid, const char* stdCode,
                          bool isBuy, double vol, double price, const char* userTag) override;
    virtual void on_order(IHftStraCtx* ctx, uint32_t localid, const char* stdCode,
                          bool isBuy, double totalQty, double leftQty,
                          double price, bool isCanceled, const char* userTag) override;
    virtual void on_position(IHftStraCtx* ctx, const char* stdCode, bool isLong,
                             double prevol, double preavail, double newvol, double newavail) override;
    virtual void on_channel_ready(IHftStraCtx* ctx) override;
    virtual void on_channel_lost(IHftStraCtx* ctx) override;
    virtual void on_entrust(uint32_t localid, bool bSuccess, const char* message, const char* userTag) override;
    virtual void on_params_updated() override;
    virtual void on_session_begin(IHftStraCtx* ctx, uint32_t uTDate) override;
    virtual void on_session_end(IHftStraCtx* ctx, uint32_t uTDate) override;

    /// C6: raw exchange option-code filter — accepts BOTH 4-digit-month codes
    /// (CFFEX/SHFE/INE/GFEX, e.g. IO2301-P-4000) and CZCE's 3-digit-month
    /// convention (e.g. TA308P4000).  Static + self-contained so unit tests
    /// can pin both conventions (previously a \\d{4} regex silently dropped
    /// EVERY CZCE option).
    static bool isRawOptionCode(const std::string& rawCode);

    /// Level-triggered quotable-zone query (pure — unit tested).  A section
    /// is quotable while curSec (mapped into the section timeline) is past
    /// its open+resumeLag and before its close-stopLead.  Preconditions:
    /// sections sorted by start, end > start (cross-midnight already
    /// normalized by adding 86400), gaps between sections larger than
    /// stopLead + resumeLag (true for exchange session files).
    static bool isInQuotableZone(uint32_t curSec,
        const std::vector<std::pair<uint32_t, uint32_t>>& secSecs,
        int32_t stopLeadSec, int32_t resumeLagSec);

private:
    // Async event processor
    wt_option::OptionAsyncEventProcessorPtr _async;

    // Continuous per-strategy event clock; prevents midnight wrap from
    // breaking duration-based timers on night sessions.
    wt_option::OptionTimeService _timeService;

    // Authoritative long/short position book.  `_positions` below is a net
    // projection retained for legacy consumers/session accounting.
    wt_option::PositionBook _positionBook;

    // Core components
    wt_option::OptionGridPtr _grid;
    wt_option::OptionTradingGridPtr _otg;       // middle layer
    wt_option::IOptionPricerPtr _pricer;          // generic pricer interface
    std::shared_ptr<wt_option::OptionPricer2> _blackPricer;
    wt_option::AsyncVolFitter _asyncVolFitter;
    /// P1-a: "async" (default, AsyncVolFitter) | "sync" (PeriodicCurveFitter)
    std::string _volFitMode{"async"};
    double _lastVolFitSubmitTime = -1e9;   // P1-a: fitter.period throttle
    wt_option::CompositeOptionPricerPtr _compositePricer;  // MM pricer (null when gammascalp)
    wt_option::GammaScalpOptionPricerPtr _gammaPricer;    // gamma scalp pricer (null when composite_mm)
    wt_option::ControllableTradingGridPtr _ctg;
    wt_option::OptionRiskPtr _risk;              // risk instance
    std::shared_ptr<wt_option::OptionTraderContext> _traderCtx;
    wt_option::OrderGatewayPtr _orderGateway;    // narrow production order出口

    // Config
    wt_option::OptionConfigLoader::Result _configLoad;
    std::string _underlyingCode;
    std::string _optionProduct;       // option product for contract discovery (e.g. "ag_o", "IO", "SRP")
    std::string _futuresProduct;      // futures product (e.g. "ag", "IF", "SR")
    std::string _exchange;
    std::string _underlyingType;      // "future" | "index"
    std::vector<std::string> _optionPids;  // option product IDs to discover (e.g. ["SRP","SRC"] for CZCE)
    double _riskFreeRate = 0.03;
    int32_t _maxTPS = 50;
    // alpha + pricing params from config
    double _wgt_vegaflow = 0, _wgt_frontfut_skew = 0, _wgt_deltaflow = 0;
    double _wgt_atmsig = 0, _wgt_rollema = 0;
    double _sticky_base = 0.5, _improve_retreat = 3.0;

    // Pricer strategy type
    std::string _pricerType = "composite_mm";
    wt_option::OptionPricerMode _pricerMode = wt_option::OptionPricerMode::CompositeMM;

    // Config pointer saved for setupPricer
    wtp::WTSVariant* cfgPtr_ = nullptr;

    // OQM configs (per instrument type)
    wt_option::OptionQuoteManager::Config _optionOqmCfg;
    wt_option::OptionQuoteManager::Config _futureOqmCfg;

    // RiskFilterChain configs (per instrument type)
    struct RiskFilterConfig {
        bool enabled = false;
        uint32_t max_order_size = 100;
        bool max_order_size_reject = false;
        double min_sell_price = 1e-6;
        uint32_t max_position = 0;  // 0 = use per-expiry max_pos_opt/max_pos_fut
        int max_position_mode = 0;  // 0=reject, 1=allow, 2=modify
        int32_t max_cancel_soft = 100;
        int32_t max_cancel_hard = 200;
        int32_t max_new_orders_hard_flat = 100;
        int32_t max_new_orders_reject = 200;
    };
    RiskFilterConfig _optionFilterCfg;
    RiskFilterConfig _futureFilterCfg;
    wt_option::RiskFilterChainPtr _optionFilterChain;
    wt_option::RiskFilterChainPtr _futureFilterChain;

    // Expiry configs
    struct ExpiryConfig {
        bool enable = true;
        std::string underlyingCode;  // per-expiry pricing underlying (empty=use global)
        std::string hedgeCode;       // tradeable hedge (empty=same as underlyingCode)
        std::vector<std::string> secondaryHedgeCodes;
        double delta_min = 0.05, delta_max = 0.95;
        double sprd_fwd = 0.01, sprd_atmvol = 0.1, sprd_corr = 0.0;
        int32_t max_pos_fut = 1, max_pos_stk = 50, max_pos_opt = 50, max_qsize = 5;
        bool enable_auto_close = false;
        double close_pos_thresh = 0;
        bool include_future = true;          // include future mid in synthetic forward (quantbox design)
        int min_strikes_for_synthetic = 1;  // min valid contributors for forward (1=any single source)
    };
    std::map<uint32_t, ExpiryConfig> _expiryConfigs;
    std::set<std::string> _hedgeCodes;  // distinct hedge codes (excluding underlying)
    std::set<std::string> _pnlPendingInit;  // codes awaiting PnL init (position known, waiting for preClose)
    // P1-5: last seen preClose per code (worker thread).  Lets session-begin
    // seed carried positions immediately instead of leaving the trackers
    // flat until the first tick of the day (fills in that window realized 0).
    std::map<std::string, double> _preCloseCache;

    // State
    IHftStraCtx* _ctx = nullptr;
    std::atomic<bool> _initialized{false};
    // B2-2.4: channel readiness lives solely in _tstate.channelReady (worker
    // single-writer via the async channel callback); the former engine-thread
    // atomic mirror was a double-write hazard.

    // Position tracking (strategy local)
    std::unordered_map<std::string, double> _positions;

    // Tick counter (for diagnostics)
    std::atomic<uint64_t> _tickCount{0};

    // Compute scheduling: underlying-driven (quantbox design).
    // Only recompute when the underlying (front-month) book changes.
    // Option ticks update market snapshots but do NOT trigger compute.
    bool _underlyingChanged = false;    // set by on_tick when underlying tick arrives
    bool _needsRefresh = false;         // set by on_trade/on_position/on_order to force refresh
    double _minComputeInterval = 0.02;  // min seconds between computes (debounce)
    double _lastComputeTime = 0;

    // B4: Session-based mid-day stop/resume scheduling
    wtp::WTSSessionInfo* _sessionInfo = nullptr;
    int32_t _stopLeadSeconds = 30;      // stop this many seconds before section end
    int32_t _resumeLagSeconds = 0;      // resume this many seconds after section start
    // B4 level-triggered state: quoting-stop state is DERIVED from absolute
    // time on every timer event (see isInQuotableZone), not from window-edge
    // detection — a delayed/dropped timer can no longer lose a whole stop
    // window.  _b4Primed guards startup: the CTG has been quoting since
    // on_init, and tradingStopMidDay/resumeTrading are not idempotent (they
    // fire the session-stop state machine), so the first event only
    // reconciles a genuinely-stopped state instead of calling both ways.
    bool _b4Primed = false;
    /// Trading sections in normalized seconds-of-day; cross-midnight
    /// sections have end > 86400 (e.g. 2100-0100 -> [75600, 90000]).
    /// Built once from WTSSessionInfo on the first timer event.
    std::vector<std::pair<uint32_t, uint32_t>> _secSecs;
    bool _sessionStopped = false;       // currently in session-stop state

    // B2: Risk-free rate curve
    std::vector<std::pair<double, double>> _rateCurve; // (days, rate) sorted

    // A6: Scanners
    wt_option::OptionScanCoordinator _scanCoord;

    // Scanner execution context (bridges to IHftStraCtx)
    wt_option::ComboExecContext _comboCtx;
    // Active combo orders (for timeout checking and fill routing)
    std::vector<wt_option::ComboOrderPtr> _activeCombos;

    // B7: Attribute publisher for monitoring
    wt_option::AttributePublisherPtr _attrPub;

    // B9: Expiration simulator (session-level PnL)
    wt_option::ExpirationSimulatorPtr _expSim;

    // B10: Option value writer (records to CSV)
    wt_option::OptionValueWriterPtr _valWriter;
    double _ovwStartTime = 0;
    double _ovwEndTime = 86400;
    double _ovwPeriod = 5.0;
    bool _ovwEnabled = false;

    // B11: Predictor infrastructure
    wt_option::IPredictorPtr _predictor;
    wt_option::TriggerEnginePtr _triggerEngine;

    // Enhancement modules (borrowed from quantbox)
    wt_option::RiskLimitsEx _riskLimitsEx;                          // Extended risk limits
    wt_option::FillPriceCheckerPtr _fillPriceChecker;                // Fill price deviation monitor
    std::unordered_map<std::string, wt_option::PositionGuardPtr> _positionGuards;  // Per-contract position guards
    std::unordered_map<std::string, wt_option::PositionOffsetMgrPtr> _positionOffsets; // Per-contract offset managers
    wt_option::QuoteStatistics _quoteStats;                          // Quote statistics (session-level)
    uint64_t _tickTimestampUs = 0;                                   // Last tick arrival time (for latency measurement)

    // B2/3.3: single per-order registry — exchange-report anomaly protection
    // (IssuedOrderTracker absorption) + fill-deviation inputs + 3.6
    // fill-quality attribution source.  Replaces the former parallel
    // OrderAnomalyGuard::Rec / FillPriceChecker::IssueInfo tables.
    wt_option::IssuedOrderRegistry _orderRegistry;

    // 3.6 fill-quality attribution configuration
    bool _attrEnable = false;
    std::string _attrOutDir = "outputs_bt";
    uint32_t _attrKeepMaxRecs = 20000;  // size-bound registry purge (live+backtest safe)
    uint32_t _attrGraceSec = 300;       // done records younger than this are never purged
    uint32_t _curSessionDate = 0;       // owning session for new registry records

    // Scanner throttle (D) — see setupScanners / ControllableTradingGrid
    double _scanThrottleCooldown = 5.0;   // per-code hit cooldown (seconds)
    double _scanThrottleGrowth = 0.25;    // refire early only if signal grew >= 25%
    int32_t _scanMaxActiveCombos = 3;     // concurrent scanner combo cap

    // P2: quoting-duty metric — enabled sim-seconds vs the session's
    // tick-active window (first→last timer payload time).  ALL references
    // are enqueue-side payload times (secOfDay + midnight-wrap base):
    // processing-time clocks jitter run-to-run and made the ratio
    // irreproducible (observed 25.8% vs 100% same-day variance), and the
    // scheduled section sum is wrong here — one trading-date session spans
    // night+day (~18h wall) so enabled can exceed it.
    double _dutyAccumSec = 0;
    double _dutyEpochBase = 0;      // +86400 per midnight wrap (payload clock)
    double _dutyLastSimT = 0;       // last payload sim-time (accumulation ref)
    double _dutyWinStartT = 0;      // first timer payload time of the session
    double _dutyWinEndT = 0;        // last timer payload time of the session
    uint32_t _dutyLastSecOfDay = 0; // last payload secOfDay (wrap detector)
    // AEP session-begin snapshot of queueDrops for the per-session delta in
    // the session-end metrics line (T0-3, audit 2026-11-13 v2).
    uint64_t _aepDropsSessionStart = 0;

    /// Market-readiness gate (user-approved design): readiness is a SINGLE
    /// flag owned by ExpiryData::isFitReady — session open clears it, a
    /// successful fit publish sets it (OptionPricer2::publishFittedCurves),
    /// and a sustained point drought (> MIN_FIT_AGE_BEFORE_REBLOCK since the
    /// last publish) clears it again.  The pricer turns !fitReady into
    /// priced=false, so CTG quoting and scanner dispatch are gated by the
    /// existing isPriced() checks — no parallel block set here.
    /// Hysteresis: time of the last fit publish per expiry — a fresh curve
    /// survives transient point droughts (seconds of one-sided quotes)
    /// without yanking resting orders.  Cleared at session open.
    std::unordered_map<uint32_t, double> _expFitPublishT;
    static constexpr double MIN_FIT_AGE_BEFORE_REBLOCK = 30.0;  // seconds
    /// Strategy-side re-block (session open / sustained drought).  The
    /// unblock side lives in OptionPricer2::publishFittedCurves.  Logs
    /// transitions only.
    void setExpiryFitReady(uint32_t exp, bool ready, const char* reason);

    // R1: latest exchange price limits per code (from tick feed)
    struct PriceLimit { double upper = 0; double lower = 0; };
    std::unordered_map<std::string, PriceLimit> _priceLimits;

    // R5: timer-event dedup state (suppress the per-tick timer storm)
    uint32_t _lastTimerDate = 0;
    double   _lastTimerSecOfDay = -1e9;
    // Worker减负: options changed by tick/trade/position in the current batch.
    std::unordered_set<std::string> _dirtyOptions;
    // Baseline worker latency from on_tick_batch to on_batch_complete.
    std::chrono::steady_clock::time_point _batchStartSteady{};
    bool _batchStartValid = false;
    wt_option::LatencyHistogram _batchLatencyUs;
    // Per-instance batch counter; a function-static here races when multiple
    // strategy workers are running in the same process.
    uint64_t _batchCount = 0;

    // Market freshness gate.  0 disables the check.
    uint64_t _marketStaleTimeoutMs = 3000;
    bool _marketStaleActive = false;
    bool isMarketFresh(uint64_t nowMs) const;

    // R8: unified trading state — single writer for enabled/panicked
    wt_option::TradingState _tstate;
    // Fail-safe fence (plan D): set IMMEDIATELY on the engine callback thread
    // when the channel is lost; cleared by the worker only after the recovery
    // sweep completes.  All send paths check it before touching the wire, so
    // a lost channel stops quoting instantly instead of waiting for the
    // queued worker callback (which may lag or, pre-fix, could be dropped).
    std::atomic<bool> _channelDown{false};
    void setState(wt_option::TradingStateType t, const std::string& reason);
    bool _tradingStateEnabled() const;
    void applyTradingState();

    // R9: structured hot-command bus (replaces raw manual_order parsing)
    wt_option::CommandBus _cmdBus;
    wt_option::OptionCommandHandlers _cmdHandlers;
    void registerCommands();
    // Hot params are applied on the async worker, never on the framework
    // callback thread that invokes on_params_updated().
    std::atomic<bool> _hotUpdatePending{false};
    void applyHotParams();

    // R10: portfolio delta-hedge executor (default off)
    bool   _deltaHedgeEnable = false;
    std::string _primaryHedgeCode;
    wt_option::DeltaHedgeExecutor::Config _hedgeCfg;
    wt_option::OptionHedgeCoordinator _hedgeCoord;   // owns in-flight + cooldown
    bool _deltaHedgeLotWarned = false;
    void checkDeltaHedge(double nowSec);
    /// Resolve delta-per-lot from the hedge contract multiplier; <=0 means unknown.
    double resolveDeltaPerLot();

    /// Close-phase delta flatten (user-approved design): close times are
    /// derived from the FRAMEWORK session info (WTSSessionInfo trading
    /// sections) — a section end that starts an inter-section gap >= the
    /// median gap is a daily close (SHFE: day 15:00 / night 01:00; lunch
    /// breaks are below the median and never qualify).  The strategy layer
    /// configures only how long before the close to start
    /// (delta_hedge.flatten_lead_seconds).  While active, the hedge channel
    /// drives portfolio delta to ~0 (half-lot deadband); option legs
    /// (vega/theta) stay.  Disarmed on resume / session begin.
    std::set<uint32_t> _closeSectionEnds;          // seconds-of-day closes
    bool   _closeSectionEndsComputed = false;
    double _closeFlattenLead = 60.0;               // start this many sec before the close
    bool   _closeFlattenActive = false;
    double _flattenAggrTicks = 2.0;   // close-flatten price aggression (ticks beyond touch)
    void disarmCloseFlatten();        // cancel in-flight hedge orders + disarm
    void computeCloseSectionEnds();   // median-gap rule over framework session sections

    // R-P1: prediction accuracy loop closure
    wt_option::PredictionEvaluator _predEvaluator;

    // Phase 2: scenario stress (delta-gamma-vega-theta) on the portfolio book
    wt_option::StressAnalyzer _stress;
    double _lastStressTime = -1e9;
    double _stressLimit = 0.0;   // 0 = alert-only; >0 = Panic if worst < -limit

    // B1: estimated-margin guard (config-driven; exchange does not publish
    // margins via commInfo, so rates are calibrated in config)
    struct MarginConfig {
        bool enabled = false;
        double futRate = 0.10;          // per-unit notional rate for futures leg
        double optShortRate = 0.12;     // per-unit UNDERLYING notional rate for short options
        double maxMargin = 0;           // account limit (0 = disabled)
        double warnRatio = 0.9;
        double checkPeriodSec = 5.0;
    } _marginCfg;
    double _lastMarginCheckTime = 0;
    std::atomic<bool> _limitsBreached{false};   // Greeks/daily-loss/margin breach latch
    bool _fifoPnlMode = false;                  // B5

    void checkMarginLimits(double nowSec);       // B1
    void checkRiskLimitsEx();                    // A2 (Greeks / daily loss)
    bool submitAsyncVolFit();                    // Phase 9 async fit snapshot
    void publishAsyncVolFit();                   // worker batch-boundary publish

    // Option contracts to subscribe (from config)
    std::vector<std::string> _optionCodes;

    // P10: Hot-update params (sync_param pointers, nullptr in backtest)
    struct HotParams {
        double* wgt_vegaflow = nullptr;
        double* wgt_frontfut_skew = nullptr;
        double* wgt_deltaflow = nullptr;
        double* wgt_atmsig = nullptr;
        double* wgt_rollema = nullptr;
        double* sticky_base = nullptr;
        double* improve_retreat_ratio = nullptr;
        double* trade_shock_ticks = nullptr;
        int32_t* max_tps = nullptr;
        int32_t* command = nullptr;   // 0=normal, 1=stop, 2=panic, 3=resume
        int32_t* qmode_override = nullptr;  // 0=no override, 1=ON, -1=OFF, 2=CLOSE
        // B8: Manual order params (set via sync_param string "manual_order")
        // Format: "B,code,price,qty" (buy) or "S,code,price,qty" (sell) or "C,code" (cancel)
        const char* manual_order = nullptr;
        uint32_t manual_order_counter = 0;  // track changes
    } _hot;

    double hotVal(double* ptr, double fallback) const {
        return ptr ? *ptr : fallback;
    }
    int32_t hotVal(int32_t* ptr, int32_t fallback) const {
        return ptr ? *ptr : fallback;
    }

    // Setup helpers
    void setupGrid();
    void setupPricer();
    void setupCTG();
    void setupAsyncCallbacks();
    void setupSignals();
    void setupScanners();
    void wireStrikeFinders();                        // R3
    bool withinPriceLimits(const std::string& code, double price);  // R1

    // Order executors (bridge to IHftStraCtx)
    int32_t executeQuote(const std::string& code, double bidP, uint32_t bidQ,
                         double askP, uint32_t askQ);
    std::pair<uint32_t, uint32_t> executeQuoteIds(const std::string& code,
                                                  double bidP, uint32_t bidQ,
                                                  double askP, uint32_t askQ);
    int32_t executeOrder(const std::string& code, bool isBuy, double price, uint32_t qty);
    /// Submit a single order and return every exchange localid produced by the
    /// framework action policy.  Callers that track in-flight state (hedge,
    /// combo) must use this instead of the success-count wrapper below.
    OrderIDs executeOrderIds(const std::string& code, bool isBuy,
                             double price, uint32_t qty);
    void registerIssuedOrderIds(const std::string& code, const OrderIDs& ids,
                                double price, uint32_t qty, bool isBuy);
    /// 3.6: session-end export of fill_attribution.csv + aggregate log.
    void exportFillAttribution(uint32_t tdate);
    bool executeCancel(uint32_t localid);
    int32_t executeCancel(const std::string& code);
};
