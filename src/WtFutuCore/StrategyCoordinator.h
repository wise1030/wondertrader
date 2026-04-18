/*!
 * \file StrategyCoordinator.h
 * \brief Strategy Coordinator for High-Frequency Market Making
 */
#pragma once

#include <string>
#include <cstdint>
#include <memory>
#include <chrono>
#include <unordered_map>
#include "../Includes/WTSMarcos.h"
#include "../Includes/FasterDefs.h"

// 必须包含此头文件以获取 PortfolioContext 和 ModuleParams 的定义
#include "SpreadOptimizer.h"
#include "AsyncArbitrageExecutor.h"

NS_WTP_BEGIN
class IUftStraCtx;
class WTSTickData;
class WTSVariant;
class WTSSessionInfo;
NS_WTP_END

namespace futu {

// Forward declarations
class FutuPortfolio;
class FutuQuoter;
class FutuRiskMonitor;
class ToxicFlowDetector;
class UnifiedOrderTracker;
class CorrelationManager;
class MarketDataContext;
class SelfTradeCalibrator;
class PerformanceMonitor;
class AdaptiveParamManager;
class SignalAggregator;
struct ContractState;

/// Processing context for a single tick
struct TickContext
{
    std::string code;
    double bid_px;
    double ask_px;
    double mid;
    uint64_t timestamp;
    uint32_t time_hms;
    uint32_t date;
    
    bool is_trading_session;
    bool market_state_paused;
    bool toxicity_paused;
    bool risk_halted;
    double tick_size;
    
    TickContext()
        : bid_px(0), ask_px(0), mid(0), timestamp(0), time_hms(0), date(0)
        , is_trading_session(false), market_state_paused(false)
        , toxicity_paused(false), risk_halted(false), tick_size(0) {}
};

/// Processing result
struct ProcessingResult
{
    bool processed;
    bool quote_placed;
    bool order_canceled;
    bool hedge_triggered;
    bool params_updated;
    bool closeout_executed;
    bool market_state_cancelled;
    uint64_t processing_time_ns;
    
    ProcessingResult()
        : processed(false), quote_placed(false), order_canceled(false)
        , hedge_triggered(false), params_updated(false)
        , closeout_executed(false), market_state_cancelled(false)
        , processing_time_ns(0) {}
};

/// Module parameters (read from coordinator.yaml)
struct ModuleParams
{
    double toxicity_vpin_threshold = 0.7;
    uint32_t toxicity_window = 50;
    uint32_t toxicity_cooloff_ms = 5000;
    
    double spread_vol_sensitivity = 1.0;
    double spread_depth_sensitivity = 0.5;
    uint32_t spread_vol_window = 100;
    double spread_min_mult = 0.5;
    double spread_phi = 0.01;
    double spread_portfolio_skew_weight = 0.5;
    double spread_min_correlation = 0.5;
    double spread_max_skew = 5.0;  // 新增：最大偏置限制
    
    double portfolio_max_delta = 0.0;
    double delta_skew_threshold = 0.1;  // 从 0.3 降到 0.1，更早触发 skew
    double delta_skew_factor = 3.0;      // 从 2.0 提升到 3.0，增强 skew 效果
    
    double market_vol_threshold = 0.003;
    double market_move_threshold = 0.005;
    double market_spread_threshold = 5.0;
    double market_volume_threshold = 10.0;
    uint32_t market_lookback_ticks = 50;
    uint32_t market_cooldown_ticks = 20;
    
    uint32_t auto_cancel_max_age_ms = 5000;
    double auto_cancel_price_deviation = 3.0;
    bool auto_cancel_on_state_change = true;
    bool auto_cancel_on_inventory_limit = true;
    uint32_t auto_cancel_inventory_cooldown_ms = 2000;
    
    bool stp_enabled = true;
    double stp_min_price_gap = 1.0;
    bool stp_allow_same_price = false;
    double stp_price_adjust_ticks = 1.0;
    
    bool synthetic_enabled = true;
    double synthetic_tick_weight = 0.4;
    double synthetic_book_weight = 0.4;
    double synthetic_self_trade_weight = 0.2;
    uint32_t synthetic_min_samples = 5;
    
    uint32_t calibrator_lookback_trades = 50;
    uint32_t calibrator_toxicity_window_ms = 5000;
    double calibrator_adverse_threshold = 0.6;
    
    uint32_t inferer_imbalance_window_ms = 5000;
    double inferer_large_trade_threshold = 50.0;
    double inferer_min_confidence = 0.3;
    
    uint32_t adaptive_update_interval = 60;
    double adaptive_learning_rate = 0.01;
    double adaptive_min_phi = 0.001;
    double adaptive_max_phi = 0.05;
    
    uint32_t correlation_window_size = 100;
    double correlation_min_correlation = 0.5;
    double correlation_spread_z_threshold = 2.0;

    // SignalAggregator 信号聚合器配置
    uint32_t signal_volatility_window = 100;
    uint32_t signal_ofi_window = 50;
    uint32_t signal_trade_flow_window = 100;
    uint32_t signal_momentum_window = 50;
    uint32_t signal_lead_lag_window = 50;
    
    // 信号源开关
    bool signal_use_volatility = true;
    bool signal_use_ofi = true;
    bool signal_use_trade_flow = true;
    bool signal_use_book_imbalance = true;
    bool signal_use_momentum = true;
    bool signal_use_lead_lag = false;
    
    // 信号权重
    double signal_ofi_weight = 0.35;
    double signal_trade_weight = 0.25;
    double signal_book_imbalance_weight = 0.20;
    double signal_momentum_weight = 0.15;
    double signal_lead_lag_weight = 0.05;
    
    // 信号阈值
    double signal_strong_threshold = 0.7;
    double signal_vol_threshold = 0.003;
    double signal_spread_threshold = 5.0;
    double signal_book_imbalance_threshold = 0.2;
    double signal_large_trade_threshold = 50.0;
    double signal_momentum_ema_alpha = 0.1;
    
    // Alpha 影响
    double alpha_sensitivity = 5.0;  ///< Alpha impact on fair value (ticks per alpha unit)
};

/// Coordinator configuration
struct CoordinatorConfig
{
    bool use_market_making;
    bool use_spread_arbitrage;
    bool use_signal_aggregator;
    // use_alpha_engine 已移除 - Alpha 信号由 SignalAggregator 管理
    bool use_toxicity_detector;
    bool use_spread_optimizer;
    // use_market_state 已移除 - 市场状态由 SignalAggregator 管理
    bool use_auto_cancel;
    bool use_self_trade_prevention;
    bool use_synthetic_transaction;
    bool use_adaptive_params;
    
    uint32_t param_update_interval;
    uint32_t closeout_minutes_before;
    uint32_t close_time;
    bool closeout_flatten_position;
    uint32_t toxicity_cooloff_ms;
    
    bool perf_enabled;
    uint32_t perf_log_interval;
    uint32_t perf_warn_threshold_ns;
    uint32_t perf_critical_threshold_ns;
    
    ModuleParams modules;
    
    CoordinatorConfig()
        : use_market_making(true), use_spread_arbitrage(false)
        , use_signal_aggregator(true), use_toxicity_detector(true)
        , use_spread_optimizer(true)
        , use_auto_cancel(true), use_self_trade_prevention(true)
        , use_synthetic_transaction(true), use_adaptive_params(false)
        , param_update_interval(100), closeout_minutes_before(5), close_time(150000)
        , closeout_flatten_position(true), toxicity_cooloff_ms(30000)
        , perf_enabled(true), perf_log_interval(1000)
        , perf_warn_threshold_ns(10000), perf_critical_threshold_ns(50000) {}
};

class StrategyCoordinator
{
public:
    StrategyCoordinator();
    ~StrategyCoordinator();
    
    void setConfig(const CoordinatorConfig& cfg) { _cfg = cfg; }
    const CoordinatorConfig& getConfig() const { return _cfg; }
    
    bool loadConfig(const std::string& config_file);
    void loadConfigFromVariant(wtp::WTSVariant* cfg);
    void initialize();
    
    void setPortfolio(FutuPortfolio* portfolio) { _portfolio = portfolio; }
    void setOrderTracker(UnifiedOrderTracker* tracker) { _order_tracker = tracker; }
    void setRiskMonitor(FutuRiskMonitor* monitor) { _risk_monitor = monitor; }
    void setToxicityDetector(ToxicFlowDetector* detector) { _toxicity = detector; }
    void setArbExecutor(AsyncArbitrageExecutor* arb) { _arb_executor = arb; }
    void setPerformanceMonitor(PerformanceMonitor* monitor) { _perf_monitor = monitor; }
    void setSelfTradeCalibrator(SelfTradeCalibrator* calibrator) { _self_trade_calibrator = calibrator; }
    void setAdaptiveParamManager(AdaptiveParamManager* manager) { _param_manager = manager; }
    void setCorrelationManager(CorrelationManager* manager) { _correlation_manager = manager; }
    
    void setQuoters(wtp::wt_hashmap<std::string, std::unique_ptr<FutuQuoter>>* quoters) { _quoters = quoters; }
    void setSessionInfo(const std::string& code, wtp::WTSSessionInfo* sessInfo) { _session_info[code] = sessInfo; }
    wtp::WTSSessionInfo* getSessionInfo(const std::string& code) const;
    
    void setSpreadOptimizers(wtp::wt_hashmap<std::string, std::unique_ptr<SpreadOptimizer>>* opts) { _spread_opts = opts; }
    void setOrderBooks(std::unordered_map<std::string, std::unique_ptr<MarketDataContext>>* books) { _market_data = books; }
    void setSignalAggregators(std::unordered_map<std::string, std::unique_ptr<SignalAggregator>>* aggregators) { _signal_aggregators = aggregators; }

    ProcessingResult processTick(wtp::IUftStraCtx* ctx, const char* stdCode, wtp::WTSTickData* tick);
    
    bool processCloseout(wtp::IUftStraCtx* ctx, TickContext& tc);
    bool preCheck(wtp::IUftStraCtx* ctx, TickContext& tc, wtp::WTSTickData* tick);
    void updateMarketData(wtp::IUftStraCtx* ctx, const TickContext& tc, wtp::WTSTickData* tick);
    void updateSignals(wtp::IUftStraCtx* ctx, const TickContext& tc, wtp::WTSTickData* tick);
    bool checkRisk(wtp::IUftStraCtx* ctx, const TickContext& tc);
    bool processQuoting(wtp::IUftStraCtx* ctx, const TickContext& tc, wtp::WTSTickData* tick);
    bool processAutoCancel(wtp::IUftStraCtx* ctx, const TickContext& tc);
    bool checkAndHedge(wtp::IUftStraCtx* ctx);
    void updateAdaptiveParams(wtp::IUftStraCtx* ctx, const TickContext& tc);
    
    inline bool isTradingHalted() const { return _trading_halted; }
    inline bool isQuotingPaused() const { return _quoting_paused; }
    inline bool isLongBlocked() const { return _long_blocked; }
    inline bool isShortBlocked() const { return _short_blocked; }
    inline bool isMarketStatePaused() const { return _market_state_paused; }
    inline bool isToxicityPaused() const { return _toxicity_paused; }
    
    void setTradingHalted(bool halted) { _trading_halted = halted; }
    void resetSession();
    void resetDaily();
    
private:
    CoordinatorConfig _cfg;
    FutuPortfolio* _portfolio = nullptr;
    UnifiedOrderTracker* _order_tracker = nullptr;
    FutuRiskMonitor* _risk_monitor = nullptr;
    ToxicFlowDetector* _toxicity = nullptr;
    PerformanceMonitor* _perf_monitor = nullptr;
    SelfTradeCalibrator* _self_trade_calibrator = nullptr;
    AdaptiveParamManager* _param_manager = nullptr;
    CorrelationManager* _correlation_manager = nullptr;
    AsyncArbitrageExecutor* _arb_executor = nullptr;

    wtp::wt_hashmap<std::string, std::unique_ptr<FutuQuoter>>* _quoters = nullptr;
    wtp::wt_hashmap<std::string, std::unique_ptr<SpreadOptimizer>>* _spread_opts = nullptr;
    std::unordered_map<std::string, std::unique_ptr<MarketDataContext>>* _market_data = nullptr;
    std::unordered_map<std::string, std::unique_ptr<SignalAggregator>>* _signal_aggregators = nullptr;

    bool _trading_halted = false;
    bool _quoting_paused = false;
    bool _long_blocked = false;
    bool _short_blocked = false;
    bool _market_state_paused = false;
    bool _toxicity_paused = false;
    bool _channel_ready = true;
    
    uint64_t _toxicity_resume_time = 0;
    uint64_t _tick_count = 0;
    
    // P0-2.3: Global cache for portfolio metrics
    PortfolioContext _global_portfolio_ctx;
    bool _portfolio_ctx_dirty = true;
    
    std::unordered_map<std::string, wtp::WTSSessionInfo*> _session_info;
    wtp::wt_hashmap<std::string, double> _last_mid;
};

} // namespace futu
