/*!
 * \file StrategyCoordinator.h
 * \brief Strategy Coordinator for High-Frequency Market Making
 */
#pragma once

#include "ApplicationDeps.h"  // 19.33: 统一聚合
#include <string>
#include <cstdint>
#include <memory>
#include <chrono>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include "../Includes/WTSMarcos.h"
#include "../Includes/FasterDefs.h"

// 必须包含此头文件以获取 PortfolioContext 和 ModuleParams 的定义
#include "SpreadOptimizer.h"
#include "AsyncArbitrageExecutor.h"
#include "TradingState.h"
#include "FutuRiskMonitor.h"
#include "RiskLiquidator.h"
#include "RiskCoordinator.h"
#include "SessionPhaseManager.h"
#include "FutuPortfolio.h"
#include "QuotePolicyChain.h"
#include "SpinLockGuard.h"
#include "EventDispatcher.h"  // C10: synchronous event dispatcher

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
    double last_px; ///< 最新成交价 (L0 触板判定, 0=无成交)
    uint64_t timestamp;
    uint32_t time_hms;
    uint32_t date;

    bool is_trading_session;
    bool market_state_paused;
    bool toxicity_paused;
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
        : bid_px(0), ask_px(0), mid(0), last_px(0), timestamp(0), time_hms(0), date(0),
          is_trading_session(false),
          market_state_paused(false), toxicity_paused(false), tick_size(0), upper_limit(0),
          lower_limit(0)
    {}
};

/// Processing result
struct ProcessingResult
{
    bool processed;
    bool quote_placed;
    bool order_canceled;
    bool params_updated;
    bool closeout_executed;
    bool market_state_cancelled;
    uint64_t processing_time_ns;

    ProcessingResult()
        : processed(false), quote_placed(false), order_canceled(false), params_updated(false),
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
    // B1(2026-08-24②): 原 auto_cancel_price_deviation 删除 —— 其唯一消费者
    //   tracker::checkPriceDeviation 为零调用死接口; STALE 延寿判定改用独立键,
    //   默认 2.0 = 原隐式值 quoter.sticky_threshold(1.0)×2, 行为保持。
    double stale_extension_ticks = 2.0;
    // B+: 撤单重试 (pending_cancel_timeout_ms 语义已改为重试间隔)
    uint32_t cancel_retry_interval_ms = 300; ///< 撤单 ack 超时重发间隔 (ms)
    uint32_t cancel_max_retries = 3;         ///< 撤单最大重试次数, 达到后 zombie 升级

    // F-core Z3(2026-09-15): zombie 闩锁限时重探 + 遥测 (2026-09-15 13:30 双合约
    //   88min 停报实证: 无限等 zombie 清账不可接受, 需 bounded 恢复)
    uint32_t zombie_latch_reprobe_ms = 45000;   ///< 闩锁持有超此时长则 cancelAll 重探一次
    uint32_t zombie_latch_telemetry_ms = 60000; ///< 闩锁持有期遥测打点间隔
    // F-core B2(2026-09-16): 闩锁续期封顶 —— 2026-09-16 14:13 双合约 136s 停报实证:
    //   闩锁期间 cancelAll 风暴新造 ZOMBIE 会把重探计时反复重置, 停报时长无上界.
    //   持有超此值即停止重探风暴 (保持停报 + 一次性高优告警), 恢复走 zombie 清账路径.
    uint32_t zombie_latch_max_hold_ms = 120000; ///< 闩锁持有上限 (ms), 0=不封顶
    // F-core S1(2026-09-15): hedge_ratio 慢化 EMA 窗口 (skew driver 纯化 --
    //   hr 逐 tick 价格重算污染 driver, 翻转-成交相关仅 0-9%, 见当日评估)
    uint32_t hr_slow_window_ms = 300000;        ///< hedge_ratio 慢化 EMA 窗口 (5min)

    // AdaptiveParam (仍需保留)

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

    uint32_t param_update_interval;
    uint32_t closeout_minutes_before;
    uint32_t close_time;
    bool closeout_flatten_position;
    uint32_t night_close_time;     // 夜盘收盘时间 (HHMM格式，0=无夜盘)
    uint32_t night_minutes_before; // 夜盘收盘前N分钟触发
    // 2026-09-19③ 批次B: closeout 重试参数纳入 FutuConfigMap 唯一映射链 (原 assembler 直读
    // FutuMmConfig.closeout 形成第二填充链, 与 CloseoutTrigger/SessionPhase 判定存在分叉风险)
    uint32_t closeout_max_retries;       // closeout 最大重试次数
    uint32_t closeout_retry_interval_ms; // closeout 重试间隔

    bool perf_enabled;
    uint32_t perf_log_interval;          // propagated from config.yaml via FutuMmConfig
    uint32_t perf_warn_threshold_ns;     // propagated from config.yaml via FutuMmConfig
    uint32_t perf_critical_threshold_ns; // propagated from config.yaml via FutuMmConfig
    // F-core P2(2026-09-17): 告警日志同通道限频 (ms, 默认 5min)
    uint32_t perf_alert_throttle_ms;

    // perf fields: propagated from config.yaml via FutuMmConfig, not read by StrategyCoordinator itself
    uint64_t perf_monitor_latency_threshold;

    // v7.1 taker 紧急减仓 (主动吃单; 应对大量成交突然穿仓)
    double taker_reduce_threshold = 1.3;       ///< 合约 util ≥ 此值触发 taker 减仓 (0=禁用)
    double taker_reduce_target_util = 0.8;     ///< 减仓目标: 平到 target × maxPos
    uint32_t taker_reduce_cooldown_ms = 30000; ///< 每合约 taker 减仓限频

    // v7.1 成交后立即重挂 (做市义务: 单边成交侵蚀深度 → 撤剩余单重新挂单)
    uint32_t requote_after_fill_min_interval_ms = 200; ///< 每合约成交重挂最小间隔(ms, 0=禁用)

    // v7.9 session 休息段暂停 (每节收盘前 N 秒撤单+暂停报价/套利;
    // 每日最后一节由 closeout 处理, 不在此列). 0=禁用, 默认10s
    uint32_t section_break_seconds_before = 10;

    // 双边统计输出链路：周期输出间隔(秒，0=禁用) 与持久化目录
    uint32_t bilateral_stats_log_interval_sec = 300;
    std::string bilateral_stats_log_dir = "./Logs";

    ModuleParams modules;
    wtp::WTSVariant* _raw_variant = nullptr;

    CoordinatorConfig()
        : use_market_making(true), use_spread_arbitrage(false), use_signal_aggregator(true),
          use_toxicity_detector(true), use_spread_optimizer(true), use_self_trade_prevention(true),
          param_update_interval(100), closeout_minutes_before(5), close_time(150000),
          closeout_flatten_position(true), night_close_time(0), night_minutes_before(5),
          closeout_max_retries(3), closeout_retry_interval_ms(5000), perf_enabled(true),
          perf_log_interval(1000), perf_warn_threshold_ns(10000), perf_critical_threshold_ns(50000),
          perf_alert_throttle_ms(300000), perf_monitor_latency_threshold(100000),
          bilateral_stats_log_interval_sec(300),
          bilateral_stats_log_dir("./Logs")
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
    // 19.33: CoordinatorDeps 统一为 ApplicationDeps 别名 (单一聚合权威, 定义不再重复)
    using CoordinatorDeps = ApplicationDeps;

    /// B7: Wire all dependencies in one call (replaces 15 individual setters).
    /// Must be called after all deps are created, before first tick.
    void wireDeps(const CoordinatorDeps& deps);

    /// C12: Pre-populate _last_mid from _quoters (call after wireDeps sets _quoters)
    void initLastMid();

    /// B7: Fail-fast validation - logs errors and returns false if required deps missing.
    bool validateDeps() const;

    /// 19.34: wireDeps 填充后的子组件聚合 (CloseoutOrchestrator 接线用, 含回调)
    const ApplicationDeps& wiredDeps() const { return _wired_deps; }

        void setConfig(const CoordinatorConfig& cfg)
    {
        _cfg = cfg;
        syncPhaseConfig();
    }
    const CoordinatorConfig& getConfig() const { return _cfg; }
    EventDispatcher& eventDispatcher() { return _event_dispatcher; }  // C10
    void setAlphaSensitivity(double val) { _cfg.modules.alpha_sensitivity = val; }
    void setPortfolioMaxDelta(double val) { _cfg.modules.portfolio_max_delta = val; }

    /// v7.1: 注入 replay 时间 (tick actiondate/actiontime 推出, 跨日单调).
    /// 回测中墙钟节流随机器速度漂移 → 订单序列不可复现;
    /// hedge/taker/requote 等节流统一改用 replay 时钟 (0=未注入, 回退墙钟).
    void setExchangeTime(uint64_t ms) { _last_exchange_time_ms = ms; }

    /// V10-R9: 注入 live 墙钟 (HHMM) —— 实盘会话门 (isOrderSubmissionAllowed)
    /// 必须用本地墙钟: ctx->stra_get_time() 取自最后 tick, 休市期冻结 → 休市窗口
    /// 被判为可交易 (2026-09-01 10:15 风暴根因之一)。回测不注入 → 保持 tick 钟
    /// 复现性。参数化以便单测注入伪时钟。
    void setLiveWallclock(std::function<uint32_t()> fn) { _wallclock_hhmm = std::move(fn); }

    /// V10-R9: 补挂限频判定 (纯函数, 单测锁定)。单向窗口 now < last+interval → 拦截;
    /// 等值/冻结/回拨时钟均拦截。interval_ms=0 → 禁用 (恒放行)。
    static bool requoteThrottleAllow(uint64_t& last, uint64_t now_ms, uint32_t interval_ms);

    bool loadConfig(const std::string& config_file);
    void loadConfigFromVariant(wtp::WTSVariant* cfg);
    void initialize();

public:
    /// 19.34: 双边统计定点 flush — 公开供 assembler 填 deps.flush_bilateral_stats 回调
    void flushBilateralStats(wtp::IUftStraCtx* ctx, uint32_t hhmm, uint32_t secs);

private:
    /// 5A-1: _cfg → _phase_mgr 配置同步 (loadConfig/setConfig 后调用)
    void syncPhaseConfig();
    /// F-core D2(2026-09-16): 排空周期行 I/O 积压 (段末 + 每 5min 周期采点各一次)
    void flushBilateralIoBacklog();
    void logBilateralStatsPeriodic(wtp::IUftStraCtx* ctx, const TickContext& tc);

public:
    /// V10-R8-a: 策略层恢复状态挂钩注入 (须在 wireDeps 之前调用)
    void setResumeStateHooks(std::atomic<uint64_t>* quoting_paused_since, std::atomic<uint32_t>* order_error_count)
    {
        _resume_quoting_paused_since = quoting_paused_since;
        _resume_order_error_count = order_error_count;
    }

    void setSessionInfo(const std::string& code, wtp::WTSSessionInfo* sessInfo)
    {
        _phase_mgr.setSessionInfo(code, sessInfo);
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
    /// 从当日持久化文件恢复各 quoter 的双边统计（重启续算）
    void seedBilateralStatsFromFile(uint32_t tdate);
    /// wall 日期 → trading date：HHMM >= 2100 时归属次一交易日（夜盘），否则当日
    static uint32_t tradingDateOf(uint32_t wallDate, uint32_t hhmm);
    bool preCheck(wtp::IUftStraCtx* ctx, TickContext& tc, wtp::WTSTickData* tick);
    void updateMarketData(wtp::IUftStraCtx* ctx, TickContext& tc, wtp::WTSTickData* tick);
    void updateSignals(wtp::IUftStraCtx* ctx, const TickContext& tc, wtp::WTSTickData* tick);
    bool processQuoting(wtp::IUftStraCtx* ctx, TickContext& tc, wtp::WTSTickData* tick);
    bool processAutoCancel(wtp::IUftStraCtx* ctx, const TickContext& tc);
    /// v7.1: taker 紧急减仓 — 合约 util ≥ taker_reduce_threshold 时 FAK 对手价
    /// 平掉 (|pos| - target×maxPos) 超出部分, 每合约 cooldown 限频.
    /// 应对"大量成交突然穿仓"场景: 被动减仓太慢时主动吃单, 报价永不停.
    /// v7.1: 成交后立即重挂 — 单边成交把挂单深度侵蚀到 min_valid_qty 以下时,
    /// 用最近一个 tick 的报价参数立即撤剩余单+重新挂单, 恢复双边做市义务,
    /// 不再等下一个 tick。requote_after_fill_min_interval_ms 限频防 churn。
    /// @param from_fill true=成交触发(发 FillReceived 事件); false=B+ 撤单终态回报
    ///        触发的 live 事件驱动补挂 (不发 FillReceived, 其余守卫/限频完全一致)
    bool requoteAfterFill(wtp::IUftStraCtx* ctx, const std::string& code, uint64_t now_ms, bool from_fill = true);

    /// V10-R8-b: 挂单面时段语义单一出口 —— 交易时段内 且 非集合竞价撮合分钟
    /// (isInTradingTime 对竞价分钟按设计返回 true, 与交易所挂单接受面错位;
    /// 无 session 信息时返回 true 保持旧行为)
    bool isOrderSubmissionAllowed(wtp::IUftStraCtx* ctx, const char* code);
    // attemptPositionReduction removed — replaced by enhanced skew (clamp + inventory_skew_gain, delta 口径)

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
    // setTradingHalted removed — use setQuotingPhase(RISK_HALTED) or enterCloseout()
    void resetSession();
    void resetDaily();

    /// 外部恢复路径 (UftFutuMmStrategy::on_trade / on_channel_ready) 绕过
    /// coordinator 的 checkRisk 自动恢复, 需调用本方法同步重置协调器本地风控状态,
    /// 否则风险加宽倍数残留 → 恢复后报价宽度被永久放大 ×1.5/×2.0.
    void onExternalResumeFromRisk() { _quote_chain.riskWiden().reset(); }

private:
    CoordinatorConfig _cfg;
    std::string _loaded_from;  ///< 19.36: 配置文件路径 (审计源名, 双策略分源)
    FutuPortfolio* _portfolio = nullptr;
    UnifiedOrderTracker* _order_tracker = nullptr;
    FutuRiskMonitor* _risk_monitor = nullptr;
    ToxicFlowDetector* _toxicity = nullptr;
    PerformanceMonitor* _perf_monitor = nullptr;
    CloseoutOrchestrator* _closeout_orch = nullptr;
    ApplicationDeps _wired_deps;  ///< 19.34: wireDeps 时存储 (子组件回调单填充源) // 19.34: 收盘触发决策+编排 (Trigger 已并入, wireDeps 注入)
    RiskCoordinator _risk_coord; // P1.3 Step2a: 风控协调器 (checkTakerReduce)
    SelfTradeCalibrator* _self_trade_calibrator = nullptr;
    CorrelationManager* _correlation_manager = nullptr;
    AsyncArbitrageExecutor* _arb_executor = nullptr;
    SpreadArbitrageManager* _arb_manager = nullptr; // B2: 平仓 intent 查询 (可空)
    OrderRouter* _order_router = nullptr;
    std::atomic<uint64_t>* _resume_quoting_paused_since = nullptr; // V10-R8-a
    std::atomic<uint32_t>* _resume_order_error_count = nullptr;    // V10-R8-a

    wtp::wt_hashmap<std::string, std::unique_ptr<FutuQuoter>>* _quoters = nullptr;
    wtp::wt_hashmap<std::string, std::unique_ptr<SpreadOptimizer>>* _spread_opts = nullptr;
    std::unordered_map<std::string, std::unique_ptr<MarketDataContext>>* _market_data = nullptr;
    std::unordered_map<std::string, std::unique_ptr<SignalAggregator>>* _signal_aggregators = nullptr;

    /// Unified trading state (replaces _trading_halted, _quoting_paused, etc.)
    TradingState* _trading_state = nullptr; // Shared pointer — owned by UftFutuMmStrategy

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

public:
    /// V8-R4: _last_mid 唯一属主 (策略壳副本已删) -- 策略壳/TdSpi 路径经此读取
    double getLastMid(const std::string& code) const
    {
        auto it = _last_mid.find(code);
        return (it != _last_mid.end() && it->second) ? it->second->v.load(std::memory_order_acquire) : 0.0;
    }

private:

    // 减仓防重复触发 — removed (attemptPositionReduction deleted)

    // 日志限频
    uint64_t _last_perf_ms = 0;                 // v7.7 C3: perf 统计节流 (原 static, 跨实例共享)
    uint64_t _last_summary_ms = 0;              // v7.7 C3: 60s 摘要节流 (原 static)
    // F-core P2(2026-09-17): 5min 健康巡检行节流 + realized 滞后计数基线
    uint64_t _last_kpi_ms = 0;
    uint64_t _last_reallag = 0;
    // F-core P1-6 阶段 1(2026-09-19): halt 原因位图影子对比 (1min 节拍; 行为零变更)
    uint64_t _last_halt_shadow_ms = 0;
    uint64_t _halt_shadow_mismatch = 0;      ///< 累计不一致次数 (入 KPI)
    uint64_t _last_halt_shadow_bits = 0;     ///< 上次位图 (仅用于抑制重复 WARN)
    uint64_t _last_halt_shadow_legacy = 0;   ///< 上次 legacy 集合 (同上)
    uint64_t _nan_tick_cnt = 0;                 // v7.7 C3: nan tick 计数 (原 static thread_local)
    uint64_t _limit_skip_cnt = 0;               // 锁板单边盘口 skip 计数 (日志节流)
    uint64_t _last_pause_diag_ms = 0;           // 上次shouldPause诊断日志时间戳(ms)

    // V10-R10: 毒性单层语义 — "检测即双边暂停"(TOXICITY 相位), 协调器单点属主
    // _toxic_resume_time: 最后一次 is_toxic tick + cooloffMs (恢复保持期, 防阈值震荡抖动)
    // _toxic_pause_active: 当前毒性暂停态 (= use_toxicity_detector && now < resume_time),
    //   由 updateSignals Step7 每 tick 重算, 同 tick 内 gates 块与 checkRisk 消费 (Md 线程)
    uint64_t _toxic_resume_time = 0;
    bool _toxic_pause_active = false;

    // V10-R11 Step-1: 毒性 episode 观测（零行为影响, 日志专用）
    // 相位实际进入 TOXICITY 的边沿开 episode, hold 期满/会话切换收尾;
    // peak 由 Step7 逐 tick 跟踪 (episode 期间最大 toxic_score)
    bool _toxic_episode_open = false;
    uint64_t _toxic_episode_start_ms = 0;
    double _toxic_episode_peak = 0.0;

    // 5A-2: 报价决策链 (GLFT 后的调整阶段; V10-R10 起 ToxicityPolicy 已删,
    //   毒性暂停由协调器 TOXICITY 相位承担)
    QuotePolicyChain _quote_chain;

    // v7.1: taker 减仓限频状态 (每合约上次触发时间戳 ms)

    // v7.1: 成交重挂 — 最近一个 tick 的最终报价参数缓存 (processQuoting 末尾写入)
    struct CachedQuote
    {
        double mid = 0, l0_bid = 0, l0_ask = 0, spread_mult = 1.0;
        bool allow_bid = true, allow_ask = true;
        PreTradeDecision decision;  ///< verdict(风控闸门) + strategy(策略输入)
        double upper_limit = 0, lower_limit = 0, best_bid = 0, best_ask = 0;
        uint64_t timestamp = 0;
        bool valid = false;
    };
    // v7.6 阶段2: 小锁 — processQuoting(MdSpi) 写 / requoteAfterFill(TdSpi) 读
    mutable RecursiveSpinLock _last_quote_lock;
    std::unordered_map<std::string, bool> _halt_quoting_state; ///< per-contract HALT_QUOTING state for rate-limited logging
    std::unordered_map<std::string, CachedQuote> _last_quote_params;
    std::unordered_map<std::string, uint64_t> _last_requote_ms;
    std::unordered_map<std::string, uint64_t> _last_bilateral_log_ms;
    std::unordered_map<std::string, int> _last_bilateral_section;  ///< 19.26 F3②: 上次统计flush所属交易节(-1=未知)
    uint64_t _last_bilateral_silence_warn_ms{0}; ///< 19.26 F3②: 统计沉默告警节流(10min)

    // F-core Z3(2026-09-15): zombie 闩锁生命周期观测与限时重探
    //   _zombie_latch_ms: code -> 本进程首次置位时刻(ms); isZombieHalted==false 即清除
    //   (Md 线程单写: processAutoCancel 为唯一触碰点, 无需加锁)
    std::unordered_map<std::string, uint64_t> _zombie_latch_ms;
    // F-core B1(2026-09-16): 闩锁单调起点 (emplace 首次置位, 永不重置) —— 遥测/封顶以此计时;
    //   _zombie_latch_ms 退化为"重探倒计时". capped 集合保证 EXCEEDED 告警每闩锁一次.
    std::unordered_map<std::string, uint64_t> _zombie_latch_since_ms;
    std::unordered_set<std::string> _zombie_latch_capped;
    uint64_t _last_zombie_tele_ms = 0;
    // F-core S1(2026-09-15): hedge_ratio 慢化 EMA (skew driver 纯化, Md 线程单写)
    std::unordered_map<std::string, double> _hr_slow;
    std::unordered_map<std::string, uint64_t> _hr_slow_last_ms;

    // V10-R9: live 墙钟提供器 (HHMM) —— 空则回退 ctx tick 钟 (回测/默认)
    std::function<uint32_t()> _wallclock_hhmm;

    // v7.1: replay 时钟 (策略每 tick 注入; 节流判定统一时间基准, 0=未注入回退墙钟)
    uint64_t _last_exchange_time_ms = 0;

    EventDispatcher _event_dispatcher;  // C10: synchronous event dispatcher (listeners future)

    // v7.1: session 休息段状态
    bool _section_break_active = false;

    // V8-R6/R6-b: 双边统计文件落盘积压 —— 周期 live 行不再逐条同步 open/write/close
    // (MdSpi 热路径 µs 级抖动源), 改内存积压, 于 section-break/收盘 flush 定点
    // (安静期) 一次性写盘。仅 MdSpi 线程触碰, 无需加锁。
    std::vector<std::pair<uint32_t, std::string>> _bilateral_io_backlog;

    // V8-R6/P2-2 + V10-R10: 暂停边沿触发 —— 仅进入任一暂停(MARKET/TOXICITY)的首 tick
    // 撤一次全单; 两源皆消复位。(旧实现 qphase==MARKET 期间每 tick 重发 cancelAll,
    // 首轮后槽内全为 pendingCancel, 后续每 tick 白白抢 quoter 自旋锁+遍历)
    bool _pause_cancel_active = false;
};

} // namespace futu
