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
#include "TradingState.h"
#include "FutuRiskMonitor.h"
#include "RiskLiquidator.h"
#include "CloseoutTrigger.h"
#include "RiskCoordinator.h"
#include "SessionPhaseManager.h"
#include "FutuPortfolio.h"
#include "QuotePolicyChain.h"
#include "SpinLockGuard.h"

NS_WTP_BEGIN
class IUftStraCtx;
class WTSTickData;
class WTSVariant;
class WTSSessionInfo;
NS_WTP_END

namespace futu
{

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
class SignalAggregator;
struct ContractState;
class OrderRouter;
class SpreadArbitrageManager;
// TradingState is included via TradingState.h

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
    double upper_limit; ///< P0-2: 涨停价
    double lower_limit; ///< P0-2: 跌停价

    // 每 tick 一次性解析的组件指针 (processTick 入口解析, 各 Stage 复用,
    // 消除每 tick 对 unordered_map<std::string> 的重复字符串哈希查找)
    class SignalAggregator* aggregator = nullptr;
    class MarketDataContext* book = nullptr;
    class FutuQuoter* quoter = nullptr;
    class SpreadOptimizer* spread_opt = nullptr;
    // F7: session 指针缓存 (processSectionBreak 写入, preCheck 复用,
    //   消除每 tick 第 2 次 session 表查找)
    wtp::WTSSessionInfo* session = nullptr;

    // v7.7 性能#1: 合约状态快照 (preCheck 入口一次性 getContractSnapshot,
    //   各 Stage 复用, 消除每 tick 3+ 次递归锁+ContractState 拷贝。
    //   等价性: 后续 Stage 仅读不变量字段 (tick_size/multiplier/hedge_ratio/
    //   contract_max_delta); position/avg_cost 仅 Stage 1 隔夜判据使用)
    ContractState cs;
    bool cs_valid = false;

    // perf#4: portfolio 级聚合值缓存 (updateMarketData 写入, 后续 Stage 复用,
    // 消除 checkRisk/processAutoCancel 中重复的 O(n) getTotalDelta/getTotalExposure
    // 全合约扫描. processTick 内持仓不变 — 仅 on_trade 回调改持仓, 故单 tick 内有效)
    double total_delta = 0.0;
    double total_exposure = 0.0;

    TickContext()
        : bid_px(0), ask_px(0), mid(0), timestamp(0), time_hms(0), date(0), is_trading_session(false),
          market_state_paused(false), toxicity_paused(false), risk_halted(false), tick_size(0), upper_limit(0),
          lower_limit(0)
    {}
};

/// Processing result
struct ProcessingResult
{
    bool processed;
    bool quote_placed;
    bool order_canceled;
    bool reduce_triggered; // (deprecated: position reduction now via skew)
    bool params_updated;
    bool closeout_executed;
    bool market_state_cancelled;
    uint64_t processing_time_ns;

    ProcessingResult()
        : processed(false), quote_placed(false), order_canceled(false), reduce_triggered(false), params_updated(false),
          closeout_executed(false), market_state_cancelled(false), processing_time_ns(0)
    {}
};

/// Module parameters (read from coordinator.yaml)
struct ModuleParams
{
    // SpreadOptimizer: 已迁移到 GLFTParams::fromVariant
    double portfolio_max_delta = 0.0;

    // ToxicFlowDetector: 已迁移到 ToxicityParams::fromVariant
    uint32_t toxicity_cooloff_ms = 5000;

    // AutoCancel (仍需保留)
    uint32_t auto_cancel_max_age_ms = 10000;
    double auto_cancel_price_deviation = 3.0;
    uint32_t auto_cancel_inventory_cooldown_ms = 2000;

    // AdaptiveParam (仍需保留)
    uint32_t adaptive_update_interval = 100;
    double adaptive_min_phi = 0.001;
    double adaptive_max_phi = 0.1;

    // Alpha influence (仍需保留，由 StrategyCoordinator 使用)
    double alpha_sensitivity = 2.0;
    double cold_start_confidence_factor = 0.005;
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
    bool use_self_trade_prevention;
    bool use_adaptive_params;

    uint32_t param_update_interval;
    uint32_t closeout_minutes_before;
    uint32_t close_time;
    bool closeout_flatten_position;
    uint32_t night_close_time;     // 夜盘收盘时间 (HHMM格式，0=无夜盘)
    uint32_t night_minutes_before; // 夜盘收盘前N分钟触发

    bool perf_enabled;
    uint32_t perf_log_interval;          // propagated from config.yaml via FutuMmConfig
    uint32_t perf_warn_threshold_ns;     // propagated from config.yaml via FutuMmConfig
    uint32_t perf_critical_threshold_ns; // propagated from config.yaml via FutuMmConfig

    // perf fields: propagated from config.yaml via FutuMmConfig, not read by StrategyCoordinator itself
    uint64_t perf_monitor_latency_threshold;

    // v7.1 taker 紧急减仓 (主动吃单; 应对大量成交突然穿仓)
    double taker_reduce_threshold = 1.3;       ///< 合约 util ≥ 此值触发 taker 减仓 (0=禁用)
    double taker_reduce_target_util = 0.8;     ///< 减仓目标: 平到 target × maxPos
    uint32_t taker_reduce_cooldown_ms = 30000; ///< 每合约 taker 减仓限频

    // v7.1 成交后立即重挂 (做市义务: 单边成交侵蚀深度 → 撤剩余单重新挂单)
    uint32_t requote_after_fill_min_interval_ms = 200; ///< 每合约成交重挂最小间隔(ms, 0=禁用)

    // v7.1 session 休息段暂停 (每节收盘前 N 分钟撤单+暂停报价/套利;
    // 每日最后一节由 closeout 处理, 不在此列). 0=禁用
    uint32_t section_break_minutes_before = 1;

    ModuleParams modules;
    wtp::WTSVariant* _raw_variant = nullptr;

    CoordinatorConfig()
        : use_market_making(true), use_spread_arbitrage(false), use_signal_aggregator(true),
          use_toxicity_detector(true), use_spread_optimizer(true), use_self_trade_prevention(true),
          use_adaptive_params(false), param_update_interval(100), closeout_minutes_before(5), close_time(150000),
          closeout_flatten_position(true), night_close_time(0), night_minutes_before(5), perf_enabled(true),
          perf_log_interval(1000), perf_warn_threshold_ns(10000), perf_critical_threshold_ns(50000),
          perf_monitor_latency_threshold(100000)
    {}
};

class StrategyCoordinator
{
public:
    StrategyCoordinator();
    ~StrategyCoordinator();

    //==========================================================================
    // B7: CoordinatorDeps - consolidated dependency injection (replaces 15 setters)
    //==========================================================================
    struct CoordinatorDeps
    {
        // Core deps (required, non-null after wireDeps)
        FutuPortfolio* portfolio = nullptr;
        UnifiedOrderTracker* order_tracker = nullptr;
        FutuRiskMonitor* risk_monitor = nullptr;
        OrderRouter* order_router = nullptr;
        TradingState* trading_state = nullptr;  ///< Shared, owned by UftFutuMmStrategy

        // Borrowed container pointers (lifetime: owned by UftFutuMmStrategy, must outlive coordinator)
        wtp::wt_hashmap<std::string, std::unique_ptr<FutuQuoter>>* quoters = nullptr;
        wtp::wt_hashmap<std::string, std::unique_ptr<SpreadOptimizer>>* spread_opts = nullptr;
        std::unordered_map<std::string, std::unique_ptr<MarketDataContext>>* market_data = nullptr;
        std::unordered_map<std::string, std::unique_ptr<SignalAggregator>>* signal_aggregators = nullptr;

        // Optional deps (nullable)
        ToxicFlowDetector* toxicity = nullptr;
        PerformanceMonitor* perf_monitor = nullptr;
        SelfTradeCalibrator* self_trade_calibrator = nullptr;
        CorrelationManager* correlation_manager = nullptr;
        AsyncArbitrageExecutor* arb_executor = nullptr;  ///< nullable (arb disabled)
        SpreadArbitrageManager* arb_manager = nullptr;   ///< nullable (arb disabled)
    };

    /// B7: Wire all dependencies in one call (replaces 15 individual setters).
    /// Must be called after all deps are created, before first tick.
    void wireDeps(const CoordinatorDeps& deps);

    /// C12: Pre-populate _last_mid from _quoters (call after wireDeps sets _quoters)
    void initLastMid();

    /// B7: Fail-fast validation - logs errors and returns false if required deps missing.
    bool validateDeps() const;

        void setConfig(const CoordinatorConfig& cfg)
    {
        _cfg = cfg;
        syncPhaseConfig();
    }
    const CoordinatorConfig& getConfig() const { return _cfg; }
    void setAlphaSensitivity(double val) { _cfg.modules.alpha_sensitivity = val; }
    void setPortfolioMaxDelta(double val) { _cfg.modules.portfolio_max_delta = val; }

    /// v7.1: 注入 replay 时间 (tick actiondate/actiontime 推出, 跨日单调).
    /// 回测中墙钟节流随机器速度漂移 → 订单序列不可复现;
    /// hedge/taker/requote 等节流统一改用 replay 时钟 (0=未注入, 回退墙钟).
    void setExchangeTime(uint64_t ms) { _last_exchange_time_ms = ms; }

    bool loadConfig(const std::string& config_file);
    void loadConfigFromVariant(wtp::WTSVariant* cfg);
    void initialize();

private:
    /// 5A-1: _cfg → _phase_mgr 配置同步 (loadConfig/setConfig 后调用)
    void syncPhaseConfig();

public:
    void setPortfolio(FutuPortfolio* portfolio) { _portfolio = portfolio; }
    void setOrderTracker(UnifiedOrderTracker* tracker) { _order_tracker = tracker; }
    void setRiskMonitor(FutuRiskMonitor* monitor) { _risk_monitor = monitor; }
    void setToxicityDetector(ToxicFlowDetector* detector) { _toxicity = detector; }
    void setArbExecutor(AsyncArbitrageExecutor* arb) { _arb_executor = arb; }
    /// B2: 注入套利管理器 (查询平仓 intent / 聚合 z-score; 生命周期由 UftFutuMmStrategy 保证)
    void setArbManager(SpreadArbitrageManager* mgr) { _arb_manager = mgr; }
    void setPerformanceMonitor(PerformanceMonitor* monitor) { _perf_monitor = monitor; }
    void setSelfTradeCalibrator(SelfTradeCalibrator* calibrator) { _self_trade_calibrator = calibrator; }
    void setCorrelationManager(CorrelationManager* manager) { _correlation_manager = manager; }
    void setOrderRouter(OrderRouter* router) { _order_router = router; }

    /// Get trading state (read-only for external queries)
    const TradingState& tradingState() const { return *_trading_state; }
    /// Get trading state (mutable for direct manipulation by risk/toxicity modules)
    TradingState& tradingStateMut() { return *_trading_state; }
    /// Set shared trading state pointer (owned by UftFutuMmStrategy)
    void setTradingState(TradingState* state); // P1.3: impl in .cpp (wires _closeout_trigger)

    void setQuoters(wtp::wt_hashmap<std::string, std::unique_ptr<FutuQuoter>>* quoters) { _quoters = quoters; }
    void setSessionInfo(const std::string& code, wtp::WTSSessionInfo* sessInfo)
    {
        _phase_mgr.setSessionInfo(code, sessInfo);
    }

    void setSpreadOptimizers(wtp::wt_hashmap<std::string, std::unique_ptr<SpreadOptimizer>>* opts)
    {
        _spread_opts = opts;
    }
    void setOrderBooks(std::unordered_map<std::string, std::unique_ptr<MarketDataContext>>* books)
    {
        _market_data = books;
    }
    void setSignalAggregators(std::unordered_map<std::string, std::unique_ptr<SignalAggregator>>* aggregators)
    {
        _signal_aggregators = aggregators;
    }

    /// @param tsc_tick0 P0: on_tick 入口的 rdtsc 计数 (0=不测量), 用于
    ///   tick-to-quote 全链路延迟 (含策略层 preamble), 比 chrono 低 3 倍开销
    ProcessingResult processTick(wtp::IUftStraCtx* ctx,
                                 const char* stdCode,
                                 wtp::WTSTickData* tick,
                                 uint64_t now_ms = 0,
                                 uint64_t tsc_tick0 = 0);

    /// v7.1: session 休息段检查 — 每节收盘前 section_break_minutes_before 分钟
    /// 进入休息段: 撤全部报价+arb在途单, 停报价/套利; 下一节开始自动恢复。
    /// 每日最后一节跳过 (由 closeout 状态机处理).
    bool processSectionBreak(wtp::IUftStraCtx* ctx,
                             TickContext& tc); // v7.7 A4: 非 const (F7 session 缓存写入, 消除 const_cast)
    /// 当前是否处于 session 休息段 (供策略层门控 arb tick 喂入)
    bool isSectionBreakActive() const { return _section_break_active; }
    bool preCheck(wtp::IUftStraCtx* ctx, TickContext& tc, wtp::WTSTickData* tick);
    void updateMarketData(wtp::IUftStraCtx* ctx, TickContext& tc, wtp::WTSTickData* tick);
    void updateSignals(wtp::IUftStraCtx* ctx, const TickContext& tc, wtp::WTSTickData* tick);
    bool processQuoting(wtp::IUftStraCtx* ctx, const TickContext& tc, wtp::WTSTickData* tick);
    bool processAutoCancel(wtp::IUftStraCtx* ctx, const TickContext& tc);
    /// v7.1: taker 紧急减仓 — 合约 util ≥ taker_reduce_threshold 时 FAK 对手价
    /// 平掉 (|pos| - target×maxPos) 超出部分, 每合约 cooldown 限频.
    /// 应对"大量成交突然穿仓"场景: 被动减仓太慢时主动吃单, 报价永不停.
    /// v7.1: 成交后立即重挂 — 单边成交把挂单深度侵蚀到 min_valid_qty 以下时,
    /// 用最近一个 tick 的报价参数立即撤剩余单+重新挂单, 恢复双边做市义务,
    /// 不再等下一个 tick。requote_after_fill_min_interval_ms 限频防 churn。
    bool requoteAfterFill(wtp::IUftStraCtx* ctx, const std::string& code, uint64_t now_ms);
    // attemptPositionReduction removed — replaced by enhanced skew (clamp + inventory_skew_scale)
    void updateAdaptiveParams(wtp::IUftStraCtx* ctx, const TickContext& tc);

    inline bool isTradingHalted() const
    {
        return _trading_state ? _trading_state->qphase == QuotingPhase::RISK_HALTED : false;
    }
    inline bool isQuotingPaused() const
    {
        return _trading_state ? _trading_state->qphase == QuotingPhase::ERROR : false;
    }
    inline bool isLongBlocked() const
    {
        return _trading_state ? _trading_state->long_blocked.load(std::memory_order_acquire) : false;
    }
    inline bool isShortBlocked() const
    {
        return _trading_state ? _trading_state->short_blocked.load(std::memory_order_acquire) : false;
    }
    inline bool isMarketStatePaused() const
    {
        return _trading_state ? _trading_state->qphase == QuotingPhase::MARKET : false;
    }
    inline bool isToxicityPaused() const
    {
        return _trading_state ? _trading_state->qphase == QuotingPhase::TOXICITY : false;
    }
    // setTradingHalted removed — use setQuotingPhase(RISK_HALTED) or enterCloseout()
    void resetSession();
    void resetDaily();

    /// 外部恢复路径 (UftFutuMmStrategy::on_trade / on_channel_ready) 绕过
    /// coordinator 的 checkRisk 自动恢复, 需调用本方法同步重置协调器本地风控状态,
    /// 否则风险加宽倍数残留 → 恢复后报价宽度被永久放大 ×1.5/×2.0.
    void onExternalResumeFromRisk() { _quote_chain.riskWiden().reset(); }

private:
    CoordinatorConfig _cfg;
    FutuPortfolio* _portfolio = nullptr;
    UnifiedOrderTracker* _order_tracker = nullptr;
    FutuRiskMonitor* _risk_monitor = nullptr;
    ToxicFlowDetector* _toxicity = nullptr;
    PerformanceMonitor* _perf_monitor = nullptr;
    CloseoutTrigger _closeout_trigger; // P1.3 Step1: 收盘触发/状态机 (从 processCloseout 拆出)
    RiskCoordinator _risk_coord; // P1.3 Step2a: 风控协调器 (checkTakerReduce)
    SelfTradeCalibrator* _self_trade_calibrator = nullptr;
    CorrelationManager* _correlation_manager = nullptr;
    AsyncArbitrageExecutor* _arb_executor = nullptr;
    SpreadArbitrageManager* _arb_manager = nullptr; // B2: 平仓 intent 查询 (可空)
    OrderRouter* _order_router = nullptr;

    wtp::wt_hashmap<std::string, std::unique_ptr<FutuQuoter>>* _quoters = nullptr;
    wtp::wt_hashmap<std::string, std::unique_ptr<SpreadOptimizer>>* _spread_opts = nullptr;
    std::unordered_map<std::string, std::unique_ptr<MarketDataContext>>* _market_data = nullptr;
    std::unordered_map<std::string, std::unique_ptr<SignalAggregator>>* _signal_aggregators = nullptr;

    /// Unified trading state (replaces _trading_halted, _quoting_paused, etc.)
    TradingState* _trading_state = nullptr; // Shared pointer — owned by UftFutuMmStrategy
    bool _channel_ready = true;

    uint64_t _tick_count = 0;

    // P0-2.3: Global cache for portfolio metrics
    PortfolioContext _global_portfolio_ctx;
    bool _portfolio_ctx_dirty = true;

    // 5A-1: 会话阶段统一判定 (session 表/休息窗口/closeout 窗口单一事实来源)
    SessionPhaseManager _phase_mgr;
    // C12: atomic MidSlot (was naked double map - cross-thread R/W without sync)
    // MdSpi writes (updateMarketData), TdSpi reads (requoteAfterFill).
    // Map structure immutable after initLastMid() -> unique_ptr avoids moving atomics.
    struct MidSlot { std::atomic<double> v{0.0}; };
    wtp::wt_hashmap<std::string, std::unique_ptr<MidSlot>> _last_mid;

    // 减仓防重复触发 — removed (attemptPositionReduction deleted)

    // 日志限频
    uint64_t _last_perf_ms = 0;                 // v7.7 C3: perf 统计节流 (原 static, 跨实例共享)
    uint64_t _last_summary_ms = 0;              // v7.7 C3: 60s 摘要节流 (原 static)
    uint64_t _nan_tick_cnt = 0;                 // v7.7 C3: nan tick 计数 (原 static thread_local)
    uint64_t _last_pause_diag_ms = 0;           // 上次shouldPause诊断日志时间戳(ms)

    // 5A-2: 报价决策链 (GLFT 后的 6 个调整阶段; 软风控倍数/毒性冷却
    //   状态由 RiskWidenPolicy/ToxicityPolicy 持有)
    QuotePolicyChain _quote_chain;

    // v7.1: taker 减仓限频状态 (每合约上次触发时间戳 ms)

    // v7.1: 成交重挂 — 最近一个 tick 的最终报价参数缓存 (processQuoting 末尾写入)
    struct CachedQuote
    {
        double mid = 0, l0_bid = 0, l0_ask = 0, spread_mult = 1.0;
        bool allow_bid = true, allow_ask = true;
        double long_util = 0, short_util = 0;
        bool force_ask_obligation = false, force_bid_obligation = false;
        bool hard_block_bid = false, hard_block_ask = false;
        double upper_limit = 0, lower_limit = 0, best_bid = 0, best_ask = 0;
        uint64_t timestamp = 0;
        bool valid = false;
    };
    // v7.6 阶段2: 小锁 — processQuoting(MdSpi) 写 / requoteAfterFill(TdSpi) 读
    mutable RecursiveSpinLock _last_quote_lock;
    std::unordered_map<std::string, CachedQuote> _last_quote_params;
    std::unordered_map<std::string, uint64_t> _last_requote_ms;

    // v7.1: replay 时钟 (策略每 tick 注入; 节流判定统一时间基准, 0=未注入回退墙钟)
    uint64_t _last_exchange_time_ms = 0;

    // v7.1: session 休息段状态
    bool _section_break_active = false;
};

} // namespace futu
