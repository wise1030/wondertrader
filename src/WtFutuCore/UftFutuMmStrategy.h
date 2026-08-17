/*!
 * \file UftFutuMmStrategy.h
 * \brief GLFT+Alpha Market-Making Strategy as UFT Strategy
 *
 * 实现GLFT+Alpha做市框架：
 *   - GLFT模型：计算基础价差和库存偏移
 *   - Alpha预测：OFI + Trade Imbalance + Lead-Lag
 *   - 信号融合：Fair Value = Mid + η * α
 *   - 报价计算：P_bid = FairValue - δ/2 - Skew
 *               P_ask = FairValue + δ/2 - Skew
 *
 * 作为标准UFT策略运行，通过 WtUftRunner 启动
 */
#pragma once

#include "../Includes/UftStrategyDefs.h"
#include "../Includes/FasterDefs.h"
#include "SpreadArbitrageTypes.h"
#include "SpreadOptimizer.h"
#include "TradingState.h"
#include "FutuRiskMonitor.h"
#include "FutuHotParamManager.h"
#include "FutuHotParamWatcher.h"
#include "RiskLiquidator.h"
#include "TdSpiOffload.h"
#include <atomic>  // C11: TdSpi log offload queue
#include "CloseoutOrchestrator.h"
#include "ArbExecutionBridge.h"
#include "MonitorBridge.h"
#include "../WtUftCore/EventNotifier.h" // R1: 告警外发通道 (策略层作为组合根)
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_set>

NS_WTP_BEGIN
class WTSVariant;
class WTSCommodityInfo;
class WTSSessionInfo;
NS_WTP_END

namespace futu
{

// 业务模块
class FutuPortfolio;
class FutuQuoter;
class SpreadOptimizer;
class OrderRouter;

class MarketDataContext;
class FutuRiskMonitor;
class ToxicFlowDetector;
class CorrelationManager;
class PerformanceMonitor;
class PerformanceAnalyzer;

class SpreadArbitrageManager;
class SelfTradePrevention;
class StrategyCoordinator;
class SignalAggregator; // 新增：信号聚合器
class AsyncArbitrageExecutor;
class SpreadArbitrageManager;
class UnifiedOrderTracker;
class CloseoutExecutor;

struct RiskAlert; // R1: handleRiskAlert 参数前向声明 (定义在 SpreadRiskManager.h)

// 综合信号组件
class TickTransactionInferer;
class SelfTradeCalibrator;
class SyntheticSignalFusion;

// R3 v2: BilateralQuoteStats 已下放到 FutuQuoter 内部，本头文件不再前向声明

/// 合约信息缓存 (移到命名空间级, 供 FutuConfigLoader 填充)
struct ContractInfo
{
    std::string code;
    double multiplier;
    double tick_size;
    double max_position;       // 单合约最大持仓（硬限制）
    double max_delta;          // 单合约 Delta 软限制（用于单合约 skew 计算）
    double target_position;    // 单合约目标持仓 (默认0，超过时主动平仓)
    uint32_t close_time;       // 全天收盘时间 (HHMMSS格式，白盘收盘)
    uint32_t night_close_time; // 夜盘收盘时间 (HHMM格式，0=无夜盘，如230=02:30, 100=01:00, 2300=23:00)
};

/// 期货做市策略配置
///
/// 注意：模块参数已移至独立配置文件管理：
///   - SpreadOptimizer, AlphaEngine, ToxicityDetector, MarketState, AutoCancel
///     参数在 coordinator.yaml 的 modules 节点
///   - SpreadArbitrage 参数在 spread_arbitrage.yaml
///   - SelfTradePrevention 参数在 coordinator.yaml 的 modules 节点
struct FutuMmConfig
{
    std::string anchor_code;
    std::string coordinator_config;
    std::string spread_arbitrage_config;
    bool is_backtest = false;

    struct Portfolio
    {
        double max_delta;
        double hedge_ratio;
        Portfolio() : max_delta(50.0), hedge_ratio(1.0) {}
    } portfolio;

    struct Quoting
    {
        uint32_t num_levels;
        double base_spread;
        double base_qty;
        double level_qty_multiplier;
        double level_step;
        double sticky_threshold;
        double improve_retreat_ratio;
        double max_price_deviation;
        bool price_protection;
        double protect_ticks;
        bool use_bilateral_quote;
        // v3 软风控参数
        double qty_decay_factor;
        double obligation_min_qty;
        double obligation_max_spread_ticks;
        // v7.2 scout 多层结构: 自由探测层(level<obligationLevel)居最优价小qty,
        //   义务层退居 obligationLevel 档大qty; scout 成交即撤同侧义务层
        uint32_t obligation_level;
        double scout_qty;
        Quoting()
            : num_levels(1), base_spread(2.0), base_qty(5.0), level_qty_multiplier(0.7), level_step(1.0),
              sticky_threshold(1.0), improve_retreat_ratio(2.0), max_price_deviation(20.0), price_protection(true),
              protect_ticks(1.0), use_bilateral_quote(false), qty_decay_factor(2.0), obligation_min_qty(10.0),
              obligation_max_spread_ticks(10.0), obligation_level(0),
              scout_qty(1.0)
        {}
    } quoting;

    struct Risk
    {
        double max_exposure;
        double max_daily_loss;
        uint32_t cooldown_ms;
        uint32_t check_interval_ms;
        double recovery_threshold;
        uint32_t max_recovery_count;
        double pnl_recovery_ratio;
        double max_loss_for_recovery;

        /// 频率/速率/仓位/delta 阈值 — 单一来源 (与 FutuRiskMonitor::RateLimits 共用)
        RiskRateLimits rate_limits;

        bool
            auto_clear_irreversible_on_reset; ///< v7.1: resetDaily 自动清除 IRREVERSIBLE halt (回测用, 模拟隔夜人工复核; 生产默认 false)
        Risk()
            : max_exposure(35000000.0), max_daily_loss(-200000.0), cooldown_ms(30000), check_interval_ms(5000),
              recovery_threshold(0.8), max_recovery_count(3), pnl_recovery_ratio(0.5), max_loss_for_recovery(0),
              auto_clear_irreversible_on_reset(false)
        {}
    } risk;

    struct Closeout
    {
        uint32_t minutes_before; // 全天收盘前N分钟触发平仓
        bool flatten_position;
        uint32_t max_retries;
        uint32_t retry_interval_ms;
        uint32_t close_time;           // 全天收盘时间 (HHMMSS格式，白盘收盘)
        uint32_t night_close_time;     // 夜盘收盘时间 (HHMM格式，0=无夜盘)
        uint32_t night_minutes_before; // 夜盘收盘前N分钟触发平仓 (默认同minutes_before)
        // CloseoutExecutor 参数
        uint32_t drain_timeout_ms;   // Phase1 drain 超时
        double depth_ratio_passive;  // 被动档深度比例
        double depth_ratio_mid;      // 中间档深度比例
        double depth_ratio_aggr;     // 主动档深度比例
        uint32_t sweep_threshold_ms; // 距收盘多少ms进入SWEEP
        uint32_t sweep_ticks;        // SWEEP档越过对手价tick数
        bool use_fak;                // 是否使用FAK下单
        Closeout()
            : minutes_before(5), flatten_position(true), max_retries(3), retry_interval_ms(5000), close_time(150000),
              night_close_time(0), night_minutes_before(5), drain_timeout_ms(3000), depth_ratio_passive(0.3),
              depth_ratio_mid(0.5), depth_ratio_aggr(0.8), sweep_threshold_ms(5000), sweep_ticks(3), use_fak(true)
        {}
    } closeout;

    struct Perf
    {
        uint64_t monitor_latency_threshold;
        bool enabled;
        uint32_t log_interval;
        uint32_t warn_threshold_ns;
        uint32_t critical_threshold_ns;
        Perf()
            : monitor_latency_threshold(100000), enabled(true), log_interval(1000), warn_threshold_ns(10000),
              critical_threshold_ns(50000)
        {}
    } perf;

    struct Modules
    {
        bool use_spread_optimizer;
        bool use_toxicity_detector;
        bool use_adaptive_param;
        bool use_performance_monitor;
        bool use_performance_analyzer;
        bool use_market_making;
        bool use_spread_arbitrage;
        bool use_async_arb_thread; // true=启动独立arb线程(实盘), false=主线程同步执行(回测)
        bool use_self_trade_prevention; ///< 自成交防护开关（唯一权威: coordinator.yaml modules.selfTradePrevention.enabled）
        double stp_min_price_gap;       ///< 自成交防护最小价差（唯一权威: coordinator.yaml modules.selfTradePrevention.stpMinPriceGap）
        Modules()
            : use_spread_optimizer(true), use_toxicity_detector(true), use_adaptive_param(false),
              use_performance_monitor(false), use_performance_analyzer(false), use_market_making(true),
              use_spread_arbitrage(false), use_async_arb_thread(true), use_self_trade_prevention(true),
              stp_min_price_gap(1.0)
        {}
    } modules;

    struct OrderControl
    {
        uint32_t order_error_threshold;
        uint32_t max_orders;
        double max_pending_per_side; ///< Per-side max pending qty (0=disabled). When exceeded, drain that side.
        OrderControl()
            : order_error_threshold(10), max_orders(32), max_pending_per_side(30.0)
        {}
    } order_control;

    struct Monitor
    {
        bool enabled;               ///< MonitorBridge 总开关 (WtMonSvr GUI 数据桥)
        uint32_t flush_interval_ms; ///< stradata 落盘节流间隔
        Monitor() : enabled(false), flush_interval_ms(1000) {}
    } monitor;
};

/// 期货做市策略 - 作为 UFT 策略运行
class UftFutuMmStrategy : public UftStrategy
{
public:
    UftFutuMmStrategy(const char* id);
    virtual ~UftFutuMmStrategy();

    virtual const char* getName() override { return "FutuMM"; }
    virtual const char* getFactName() override { return "FutuStraFact"; }

    virtual bool init(WTSVariant* cfg) override;

    //==========================================================================
    // UFT 策略回调
    //==========================================================================

    virtual void on_init(IUftStraCtx* ctx) override;
    virtual void on_session_begin(IUftStraCtx* ctx, uint32_t uTDate) override;
    virtual void on_session_end(IUftStraCtx* ctx, uint32_t uTDate) override;

    virtual void on_tick(IUftStraCtx* ctx, const char* stdCode, WTSTickData* newTick) override;
    virtual void on_order_queue(IUftStraCtx* ctx, const char* stdCode, WTSOrdQueData* newOrdQue) override;
    virtual void on_order_detail(IUftStraCtx* ctx, const char* stdCode, WTSOrdDtlData* newOrdDtl) override;
    virtual void on_transaction(IUftStraCtx* ctx, const char* stdCode, WTSTransData* newTrans) override;

    virtual void on_trade(IUftStraCtx* ctx,
                          uint32_t localid,
                          const char* stdCode,
                          bool isLong,
                          uint32_t offset,
                          double vol,
                          double price) override;
    virtual void on_order(IUftStraCtx* ctx,
                          uint32_t localid,
                          const char* stdCode,
                          bool isLong,
                          uint32_t offset,
                          double totalQty,
                          double leftQty,
                          double price,
                          bool isCanceled) override;
    virtual void on_position(IUftStraCtx* ctx,
                             const char* stdCode,
                             bool isLong,
                             double prevol,
                             double preavail,
                             double newvol,
                             double newavail) override;

    virtual void on_channel_ready(IUftStraCtx* ctx) override;
    virtual void on_channel_lost(IUftStraCtx* ctx) override;

    /// 报单回报回调 - 处理报单错误（如保证金不足）
    virtual void on_entrust(uint32_t localid, bool bSuccess, const char* message) override;

    /// 参数热更新回调
    virtual void on_params_updated() override;

    //==========================================================================
    // R1: 告警通道注入 (WtUftRunner 注入 EventNotifier;
    //   RiskMonitor 直达, ArbManager 经本策略层 handleRiskAlert 转发)
    //==========================================================================
    void setEventNotifier(wtp::EventNotifier* notifier);

private:
    //==========================================================================
    // 内部方法
    //==========================================================================

    /// 初始化业务模块
    void initBusinessModules(wtp::IUftStraCtx* ctx);

    /// R1: 处理 SpreadArbitrageManager 的 RiskAlert 回调, 转发到 EventNotifier
    void handleRiskAlert(const RiskAlert& alert);

    /// 检查毒性并决定是否熔断
    bool checkToxicityAndCircuitBreak(IUftStraCtx* ctx);

    /// 处理跨期价差套利信号

    /// 处理套利成交回报
    //==========================================================================
    // on_tick 子函数 (P2-1a: 从 on_tick 拆出)
    //==========================================================================

    /// 报价暂停条件恢复（ERROR 状态指数退避恢复）
    void handleQuotingAutoResume();

    /// 更新行情数据（markToMarket + correlation + hedge_ratio）
    void handleMarketDataUpdate(const char* stdCode, WTSTickData* tick, double mid);

    /// LeadLag 跨合约数据推送
    void handleLeadLagPush(const char* stdCode, WTSTickData* tick, double mid);

    /// Coordinator 主处理 + closeout 执行驱动
    void handleCoordinatorTick(IUftStraCtx* ctx, const char* stdCode, WTSTickData* tick, uint64_t now_ms = 0);

    /// M1/M2: 订单终结统一幂等清理 (拒单路径无 on_order 回调, router 活跃表/
    /// pair 映射与 tracker 订单会永久泄漏 -> closeout inflight 守卫卡死/util 虚高)。
    /// onOrderDone/untrackOrder 对不存在 id 均 no-op。
    void finalizeOrder(uint32_t localid);

private:
    FutuMmConfig _config;

    /// 5A-3: 模块装配器 (initBusinessModules 外移, 引用别名绑定下列成员)
    friend class FutuModuleAssembler;
    /// 5A-3: 运行时事件处理 (on_trade/on_channel_ready 外移)
    friend class FutuRuntimeOps;

    //==========================================================================
    // 业务模块实例
    //==========================================================================

    /// 组合风险管理（持仓、Delta、倾斜、对冲）
    std::unique_ptr<FutuPortfolio> _portfolio;

    /// 相关性管理器 (全局多合约)
    std::unique_ptr<CorrelationManager> _correlation_manager;

    /// 多档位报价引擎（每合约一个）
    wtp::wt_hashmap<std::string, std::unique_ptr<FutuQuoter>> _quoters;

    /// GLFT价差优化器（每合约一个）
    wtp::wt_hashmap<std::string, std::unique_ptr<SpreadOptimizer>> _spread_optimizers;

    /// 统一订单跟踪器 (FutuQuoter, AutoCancelPolicy, SelfTradePrevention 共享)
    std::unique_ptr<UnifiedOrderTracker> _order_tracker;

    /// 策略协调器 (核心处理流水线)
    std::unique_ptr<StrategyCoordinator> _coordinator;

    std::unordered_map<std::string, std::unique_ptr<MarketDataContext>> _market_data;

    /// 信号聚合器 (新信号架构)
    std::unordered_map<std::string, std::unique_ptr<SignalAggregator>> _signal_aggregators;

    /// 风险监控
    std::unique_ptr<FutuRiskMonitor> _risk_monitor;

    /// R1: EventNotifier 指针 (WtUftRunner 注入; RiskMonitor 直达 + ArbManager 回调转发)
    wtp::EventNotifier* _event_notifier = nullptr;

    /// 收盘平仓编排器 (closeout 驱动职责, 架构重构 C3)
    CloseoutOrchestrator _closeout_orch;

    /// 渐进式收盘对冲执行器 (urgency-driven)
    std::unique_ptr<CloseoutExecutor> _closeout_executor;

    /// 毒性检测器 (VPIN)
    std::unique_ptr<ToxicFlowDetector> _toxicity_detector;

    //==========================================================================
    // 综合信号组件 (for markets without L2 transaction data)
    //==========================================================================

    /// 自身成交校准器
    std::unique_ptr<SelfTradeCalibrator> _self_trade_calibrator;
    //==========================================================================
    // 性能监控
    //==========================================================================

    /// 性能监控
    std::unique_ptr<PerformanceMonitor> _performance_monitor;

    /// 绩效分析器
    std::unique_ptr<PerformanceAnalyzer> _perf_analyzer;

    /// 监控数据桥 (WtMonSvr GUI: stradata/funds.csv 落盘, 默认关)
    MonitorBridge _mon_bridge;

    /// 跨期价差套利管理器
    std::unique_ptr<SpreadArbitrageManager> _spread_arb_manager;

    /// 自成交防护模块
    std::unique_ptr<SelfTradePrevention> _stp;

    /// 统一下单路由器 (套利/对冲/平仓)
    std::unique_ptr<OrderRouter> _order_router;

    /// 异步套利执行器
    std::unique_ptr<AsyncArbitrageExecutor> _async_arb;

    /// 套利执行桥 (套利下单编排/残腿对冲/快照同步, 架构重构 C4)
    ArbExecutionBridge _arb_bridge;

    std::vector<RiskViolation> _violations_buf; // 风控违规复用缓冲 (仅 TdSpi 路径 RuntimeOps 使用;
                                                //   coordinator 有独立成员供 MdSpi checkRisk, 双缓冲零共享)

    //==========================================================================
    // 辅助数据
    //==========================================================================

    // 合约信息缓存 (ContractInfo 已移至命名空间级)
    std::vector<ContractInfo> _contract_infos;

    // 当前 tick 中间价缓存 (v7.6: init 定码预填, 值原子; MdSpi 写/TdSpi 读,
    //   init 后 map 结构不可变 — unordered_dense rehash 需移动元素, 原子值不可移动
    //   故用 unique_ptr 包装)
    struct MidSlot
    {
        std::atomic<double> v{0.0};
    };
    wtp::wt_hashmap<std::string, std::unique_ptr<MidSlot>> _last_mid;

    // v7.4 P0-2: 回调串行化锁 — 框架源码核实实盘回调非单线程:
    //   on_tick 系列=CTP MdSpi 线程, on_trade/on_order/on_entrust=CTP TdSpi
    //   线程, on_session_end(盘中)=RtTicker 定时线程, 均同步直达策略无队列。
    //   回测单线程, 此锁恒无竞争 ~20ns; 实盘竞争仅发生在成交/ tick 同时刻。
    //   recursive: 防回调路径嵌套(如实盘 on_trade 内 stra_buy 的同步回执)。
    //
    // v7.6 编译开关 FUTU_CALLBACK_LOCK (默认 1=大锁, 生产基线):
    //   1 = 全部回调 _cb_mtx 串行化 (保守, 结构锁冗余但无害);
    //   0 = 细粒度模式 — 依赖阶段1-3的原子/结构锁/order_api_mtx,
    //       _cb_mtx 不再加 (实盘灰度验证前勿用)。
    std::recursive_mutex _cb_mtx;

    // P0-1 (v7.4): 统一强平/减仓原语 (无状态, setDeps 即用)
    RiskLiquidator _liquidator;

    // C11: SPSC queue for deferring TdSpi fill-path logs to tick path
    TdSpiLogQueue _tdspi_log_queue;
    std::atomic<uint64_t> _tdspi_logs_dropped{0};  // C11: queue-full drop counter
    void drainTdSpiLogs();

    // 交易时段信息缓存（初始化时一次性缓存，避免每次 tick 重复查询）
    struct SessionCache
    {
        wtp::WTSCommodityInfo* commInfo;
        wtp::WTSSessionInfo* sessInfo;

        SessionCache() : commInfo(nullptr), sessInfo(nullptr) {}
        SessionCache(wtp::WTSCommodityInfo* c, wtp::WTSSessionInfo* s) : commInfo(c), sessInfo(s) {}
    };
    wtp::wt_hashmap<std::string, SessionCache> _session_cache;

    // PortfolioContext 缓存（避免每tick分配）
    mutable PortfolioContext _cached_portfolio_ctx;
    mutable std::atomic<bool> _portfolio_ctx_dirty{true}; // v7.6: MdSpi/TdSpi 双写

    // 运行状态 (v7.6: 全部原子化 — MdSpi/TdSpi 跨线程读写)
    std::atomic<bool> _channel_ready{false};
    std::atomic<bool> _price_stale{false}; ///< P1-4: 价格过期标志（channel恢复后到首tick之间）
    TradingState _trading_state;           // 统一交易状态（替代5个bool）

    // 保存ctx指针，供on_entrust等无ctx回调使用
    IUftStraCtx* _main_ctx = nullptr;

    // 风险控制状态（由 FutuRiskMonitor 管理）
    std::unordered_map<std::string, bool> _blocked_contracts; // 单合约封锁

    // 当前 tick 数据缓存（避免重复计算）
    uint64_t _current_tick_timestamp; // 当前 tick 时间戳
    double _current_tick_mid;         // 当前 tick 中间价

    // 下单错误处理（统一处理所有下单错误）
    std::atomic<uint32_t> _order_error_count{0}; // 连续下单错误计数 (v7.6 原子)
    // order_error_threshold: use _config.order_control.order_error_threshold directly
    std::atomic<uint64_t> _quoting_paused_since{0}; // ERROR qphase 开始时间戳(ms)，0=未暂停

    // 收盘前平仓状态 (now managed by FutuRiskMonitor state machine)

    // 参数调优计数器
    uint32_t _tick_count; // Tick计数器
    std::atomic<uint64_t> _exchange_time_ms{
        0}; // v7.1: 最近 tick 的 replay 时间 (actiondate/actiontime 推出, 跨日单调; 节流统一时间基准)
            // v7.6 原子: MdSpi 每 tick 写, TdSpi 读 (quote→fill 延迟/recordFill/untrack)
    uint64_t _tsc_tick0 = 0;         // P0: on_tick 入口 rdtsc (perf monitor 启用时), tick-to-quote 全链路测量
    bool _is_backtest = false;       // 回测标志: on_trade 中只撤不挂(避免 _orders 迭代器失效)
    uint32_t _param_update_interval; // 参数更新间隔(ticks)

    //==========================================================================
    // 热更新参数（运行时可修改，无需重启）
    // 仅包含直接影响报价价格计算的参数
    // 仓位管理/风控/对冲等参数需重启生效
    //==========================================================================
    // 热更新参数 (已拆分至 FutuHotParamManager, 架构重构 C2)
    //==========================================================================
    FutuHotParamManager _hot_mgr;
    FutuHotParamWatcher _hot_watcher;  // hotparams.yaml → 共享内存同步
};

} // namespace futu
