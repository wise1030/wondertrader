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
#include "OptionQuoteManager.h"
#include "Scanners/IScanModule.h"
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
#include "OrderAnomalyGuard.h"

#include <string>
#include <memory>
#include <atomic>
#include <set>
#include <unordered_map>
#include <map>

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

private:
    // Async event processor
    wt_option::OptionAsyncEventProcessorPtr _async;

    // Core components
    wt_option::OptionGridPtr _grid;
    wt_option::OptionTradingGridPtr _otg;       // middle layer
    wt_option::IOptionPricerPtr _pricer;          // generic pricer interface
    wt_option::CompositeOptionPricerPtr _compositePricer;  // MM pricer (null when gammascalp)
    wt_option::GammaScalpOptionPricerPtr _gammaPricer;    // gamma scalp pricer (null when composite_mm)
    wt_option::ControllableTradingGridPtr _ctg;
    wt_option::OptionRiskPtr _risk;              // risk instance
    std::shared_ptr<wt_option::OptionTraderContext> _traderCtx;

    // Config
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

    // State
    IHftStraCtx* _ctx = nullptr;
    std::atomic<bool> _initialized{false};
    std::atomic<bool> _channelReady{false};

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
    bool _sessionStopped = false;       // currently in session-stop state

    // B2: Risk-free rate curve
    std::vector<std::pair<double, double>> _rateCurve; // (days, rate) sorted

    // A6: Scanners
    std::vector<wt_option::IScanModulePtr> _scanners;

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

    // B2: exchange-report anomaly protection (IssuedOrderTracker absorption)
    wt_option::OrderAnomalyGuard _anomalyGuard;

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

    // Order executors (bridge to IHftStraCtx)
    int32_t executeQuote(const std::string& code, double bidP, uint32_t bidQ,
                         double askP, uint32_t askQ);
    int32_t executeOrder(const std::string& code, bool isBuy, double price, uint32_t qty);
    int32_t executeCancel(const std::string& code);
};
