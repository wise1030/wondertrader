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

namespace futu {

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
class SignalAggregator;  // 新增：信号聚合器
class AsyncArbitrageExecutor;
class SpreadArbitrageManager;
class UnifiedOrderTracker;
class CloseoutExecutor;

// 综合信号组件
class TickTransactionInferer;
class SelfTradeCalibrator;
class SyntheticSignalFusion;

// R3 v2: BilateralQuoteStats 已下放到 FutuQuoter 内部，本头文件不再前向声明

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
    
    struct Portfolio {
        double max_delta;
        double hedge_ratio;
        double hedge_delta_threshold;  ///< 对冲触发Delta利用率阈值 (0.0-1.0)
        uint32_t hedge_cooldown_ms;    ///< 对冲冷却时间(ms)
        Portfolio() : max_delta(50.0), hedge_ratio(1.0), hedge_delta_threshold(0.8), hedge_cooldown_ms(5000) {}
    } portfolio;
    
    struct Quoting {
        uint32_t num_levels;
        double base_spread;
        double base_qty;
        double qty_decay;
        double level_step;
        double sticky_threshold;
        double improve_retreat_ratio;
        double max_price_deviation;
        bool price_protection;
        double protect_ticks;
        bool use_bilateral_quote;
        double max_obligation_spread;
        // v3 软风控参数
        double qty_decay_factor;
        double obligation_min_qty;
        double obligation_max_spread_ticks;
        bool obligation_only_l0;
        bool always_obligation;
        Quoting()
            : num_levels(1), base_spread(2.0), base_qty(5.0), qty_decay(0.7)
            , level_step(1.0), sticky_threshold(1.0)
            , improve_retreat_ratio(2.0), max_price_deviation(20.0)
            , price_protection(true), protect_ticks(1.0)
            , use_bilateral_quote(false), max_obligation_spread(10.0)
            , qty_decay_factor(2.0), obligation_min_qty(10.0)
            , obligation_max_spread_ticks(10.0), obligation_only_l0(true)
            , always_obligation(true) {}
    } quoting;
    
    struct Risk {
        double max_exposure;
        double max_daily_loss;
        uint32_t max_orders_per_sec;
        uint32_t max_cancels_per_sec;
        uint32_t max_trades_per_sec;
        uint32_t cooldown_ms;
        uint32_t check_interval_ms;
        double recovery_threshold;
        double max_delta_change_per_sec;
        uint32_t max_recovery_count;
        double pnl_recovery_ratio;
        double max_loss_for_recovery;
        double position_breach_pause_threshold;
        double delta_critical_mult;
        double delta_warning_mult;
        uint32_t widen_threshold;
        uint32_t pause_threshold;
        uint32_t flatten_threshold;
        uint32_t delta_rate_window_sec;
        uint32_t delta_rate_cooldown_ms;
        Risk()
            : max_exposure(35000000.0), max_daily_loss(-200000.0)
            , max_orders_per_sec(50), max_cancels_per_sec(30), max_trades_per_sec(20)
            , cooldown_ms(30000), check_interval_ms(5000), recovery_threshold(0.8)
            , max_delta_change_per_sec(3.0), max_recovery_count(3)
            , pnl_recovery_ratio(0.5), max_loss_for_recovery(0)
            , position_breach_pause_threshold(1.2), delta_critical_mult(1.5)
            , delta_warning_mult(0.8), widen_threshold(1), pause_threshold(2)
            , flatten_threshold(3), delta_rate_window_sec(2), delta_rate_cooldown_ms(15000) {}
    } risk;
    
    struct Closeout {
        uint32_t minutes_before;       // 全天收盘前N分钟触发平仓
        bool flatten_position;
        uint32_t max_retries;
        uint32_t retry_interval_ms;
        uint32_t close_time;           // 全天收盘时间 (HHMMSS格式，白盘收盘)
        uint32_t night_close_time;     // 夜盘收盘时间 (HHMM格式，0=无夜盘)
        uint32_t night_minutes_before; // 夜盘收盘前N分钟触发平仓 (默认同minutes_before)
        // CloseoutExecutor 参数
        uint32_t drain_timeout_ms;     // Phase1 drain 超时
        double   depth_ratio_passive;  // 被动档深度比例
        double   depth_ratio_mid;      // 中间档深度比例
        double   depth_ratio_aggr;     // 主动档深度比例
        uint32_t sweep_threshold_ms;   // 距收盘多少ms进入SWEEP
        uint32_t sweep_ticks;          // SWEEP档越过对手价tick数
        bool     use_fak;              // 是否使用FAK下单
        Closeout()
            : minutes_before(5), flatten_position(true)
            , max_retries(3), retry_interval_ms(5000), close_time(150000)
            , night_close_time(0), night_minutes_before(5)
            , drain_timeout_ms(3000)
            , depth_ratio_passive(0.3), depth_ratio_mid(0.5), depth_ratio_aggr(0.8)
            , sweep_threshold_ms(5000), sweep_ticks(3), use_fak(true) {}
    } closeout;
    
    struct Perf {
        uint64_t monitor_latency_threshold;
        bool enabled;
        uint32_t log_interval;
        uint32_t warn_threshold_ns;
        uint32_t critical_threshold_ns;
        Perf()
            : monitor_latency_threshold(100000), enabled(true)
            , log_interval(1000), warn_threshold_ns(10000), critical_threshold_ns(50000) {}
    } perf;
    
    struct Modules {
        bool use_spread_optimizer;
        bool use_toxicity_detector;
        bool use_adaptive_param;
        bool use_performance_monitor;
        bool use_performance_analyzer;
        bool use_market_making;
        bool use_spread_arbitrage;
        Modules()
            : use_spread_optimizer(true), use_toxicity_detector(true)
            , use_adaptive_param(false), use_performance_monitor(false)
            , use_performance_analyzer(false), use_market_making(true)
            , use_spread_arbitrage(false) {}
    } modules;
    
    struct OrderControl {
        uint32_t order_error_threshold;
        uint32_t max_orders;
        double stp_min_price_gap;
        OrderControl() : order_error_threshold(3), max_orders(32), stp_min_price_gap(1.0) {}
    } order_control;
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
    
    virtual void on_trade(IUftStraCtx* ctx, uint32_t localid, const char* stdCode, 
                         bool isLong, uint32_t offset, double vol, double price) override;
    virtual void on_order(IUftStraCtx* ctx, uint32_t localid, const char* stdCode,
                         bool isLong, uint32_t offset, double totalQty, double leftQty, 
                         double price, bool isCanceled) override;
    virtual void on_position(IUftStraCtx* ctx, const char* stdCode, bool isLong,
                            double prevol, double preavail, double newvol, double newavail) override;
    
    virtual void on_channel_ready(IUftStraCtx* ctx) override;
    virtual void on_channel_lost(IUftStraCtx* ctx) override;
    
    /// 报单回报回调 - 处理报单错误（如保证金不足）
    virtual void on_entrust(uint32_t localid, bool bSuccess, const char* message) override;
    
    /// 参数热更新回调
    virtual void on_params_updated() override;

private:
    //==========================================================================
    // 内部方法
    //==========================================================================
    
    /// 初始化业务模块
    void initBusinessModules(wtp::IUftStraCtx* ctx);
    
    /// 执行收盘前平仓对冲
    void executeCloseoutHedge(IUftStraCtx* ctx);
    
    /// 检查毒性并决定是否熔断
    bool checkToxicityAndCircuitBreak(IUftStraCtx* ctx);
    
    /// 处理跨期价差套利信号
    void processSpreadArbitrage(IUftStraCtx* ctx, const char* stdCode, WTSTickData* tick);
    
    /// 处理套利成交回报
    void onSpreadTrade(IUftStraCtx* ctx, const std::string& pair_id, 
                       const std::string& code, bool is_buy, double qty, double price);
    
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
    void handleCoordinatorTick(IUftStraCtx* ctx, const char* stdCode, WTSTickData* tick);
    
private:
    FutuMmConfig _config;
    
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
    
    /// closeout 对冲单只在 FLATTENING 状态首次执行一次
    bool _closeout_hedge_executed = false;
    // track closeout hedge order ids so on_order can distinguish them from MM cancels
    std::unordered_set<uint32_t> _closeout_pending_ids;
    // defer hedge to a later tick so inflight cancel/fill回执先回流
    bool _closeout_hedge_pending = false;
    uint32_t _closeout_hedge_wait_ticks = 0;
    static constexpr uint32_t CLOSEOUT_HEDGE_WAIT_TICKS = 2;

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
    

    
    /// 跨期价差套利管理器
    std::unique_ptr<SpreadArbitrageManager> _spread_arb_manager;
    
    /// 自成交防护模块
    std::unique_ptr<SelfTradePrevention> _stp;
    
    /// 统一下单路由器 (套利/对冲/平仓)
    std::unique_ptr<OrderRouter> _order_router;
    
    /// 异步套利执行器
    std::unique_ptr<AsyncArbitrageExecutor> _async_arb;

    // R3 v2: BilateralQuoteStats 已下放到 Per-Quoter 值成员，本处不再持有单实例。

    /// 用于防止套利引擎无限追单（追踪最近下达的套利单价格）
    wtp::wt_hashmap<std::string, double> _arb_last_order_price;
    
    //==========================================================================
    // 辅助数据
    //==========================================================================
    
    // 合约信息缓存
    struct ContractInfo {
        std::string code;
        double multiplier;
        double tick_size;
        double max_position;      // 单合约最大持仓（硬限制）
        double max_delta;         // 单合约 Delta 软限制（用于单合约 skew 计算）
        double target_position;   // 单合约目标持仓 (默认0，超过时主动平仓)
        uint32_t close_time;      // 全天收盘时间 (HHMMSS格式，白盘收盘)
        uint32_t night_close_time;// 夜盘收盘时间 (HHMM格式，0=无夜盘，如230=02:30, 100=01:00, 2300=23:00)
    };
    std::vector<ContractInfo> _contract_infos;
    
    // 当前 tick 中间价缓存
    wtp::wt_hashmap<std::string, double> _last_mid;
    
    // 交易时段信息缓存（初始化时一次性缓存，避免每次 tick 重复查询）
    struct SessionCache {
        wtp::WTSCommodityInfo* commInfo;
        wtp::WTSSessionInfo* sessInfo;
        
        SessionCache() : commInfo(nullptr), sessInfo(nullptr) {}
        SessionCache(wtp::WTSCommodityInfo* c, wtp::WTSSessionInfo* s) : commInfo(c), sessInfo(s) {}
    };
    wtp::wt_hashmap<std::string, SessionCache> _session_cache;
    
    // PortfolioContext 缓存（避免每tick分配）
    mutable PortfolioContext _cached_portfolio_ctx;
    mutable bool _portfolio_ctx_dirty;
    
    // 运行状态
    bool _channel_ready;
    bool _price_stale = false;  ///< P1-4: 价格过期标志（channel恢复后到首tick之间）
    TradingState _trading_state;           // 统一交易状态（替代5个bool）
    uint64_t _toxicity_resume_time; // 熔断恢复时间
    
    // 保存ctx指针，供on_entrust等无ctx回调使用
    IUftStraCtx* _main_ctx = nullptr;
    
    // 风险控制状态（由 FutuRiskMonitor 管理）
    std::unordered_map<std::string, bool> _blocked_contracts; // 单合约封锁
    
    // 当前 tick 数据缓存（避免重复计算）
    uint64_t _current_tick_timestamp;  // 当前 tick 时间戳
    double _current_tick_mid;          // 当前 tick 中间价
    
    // 下单错误处理（统一处理所有下单错误）
    uint32_t _order_error_count;     // 连续下单错误计数
    // order_error_threshold: use _config.order_control.order_error_threshold directly
    uint64_t _quoting_paused_since;  // ERROR qphase 开始时间戳(ms)，0=未暂停
    
    // 收盘前平仓状态 (now managed by FutuRiskMonitor state machine)
    
    // 参数调优计数器
    uint32_t _tick_count;        // Tick计数器
    uint32_t _param_update_interval; // 参数更新间隔(ticks)
    
    //==========================================================================
    // 热更新参数（运行时可修改，无需重启）
    // 仅包含直接影响报价价格计算的参数
    // 仓位管理/风控/对冲等参数需重启生效
//==========================================================================
    // 热更新参数 (数组化)
    //==========================================================================
    enum HotParamIndex : uint32_t {
        HP_BASE_SPREAD = 0,
        HP_BASE_QTY,
        HP_QTY_DECAY,
        HP_LEVEL_STEP,
        HP_MAX_DELTA,
        HP_ALPHA_SENSITIVITY,
        HP_OFI_WEIGHT,
        HP_TRADE_WEIGHT,
        HP_BOOK_IMBALANCE_WEIGHT,
        HP_MOMENTUM_WEIGHT,
        HP_LEAD_LAG_WEIGHT,
        HP_STRONG_THRESHOLD,
        HP_CONFIDENCE_WEIGHT_MIN,
        HP_CONFIDENCE_WEIGHT_MAX,
        HP_PHI,
        HP_DELTA_SKEW_THRESHOLD,
        HP_DELTA_SKEW_FACTOR,
        HP_MAX_SPREAD_MULT,
        HP_MIN_SPREAD_MULT,
        HP_DEPTH_SENSITIVITY,
        HP_TOXICITY_SPREAD_FACTOR,
        HP_LOW_CONFIDENCE_SPREAD_FACTOR,
        HP_STICKY_THRESHOLD,
        HP_IMPROVE_RETREAT_RATIO,
        HP_PROTECT_TICKS,
        HP_MAX_PRICE_DEVIATION,
        HP_COUNT
    };
    
    struct HotParamEntry {
        const char* name;
        double default_val;
        double* ptr;
    };
    
    HotParamEntry _hot_params[HP_COUNT];
    
    bool isHotChanged(HotParamIndex idx) const { return _hot_params[idx].ptr != nullptr; }
    double hotVal(HotParamIndex idx) const { return _hot_params[idx].ptr ? *_hot_params[idx].ptr : _hot_params[idx].default_val; }
};

} // namespace futu
