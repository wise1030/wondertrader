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
#include <string>
#include <vector>
#include <memory>
#include <mutex>

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

class MarketDataContext;
class FutuRiskMonitor;
class ToxicFlowDetector;
class CorrelationManager;
class PerformanceMonitor;
class AdaptiveParamManager;
class PerformanceAnalyzer;

class SpreadArbitrageManager;
class SelfTradePrevention;
class StrategyCoordinator;
class SignalAggregator;  // 新增：信号聚合器
class AsyncArbitrageExecutor;
class SpreadArbitrageManager;
class UnifiedOrderTracker;

// 综合信号组件
class TickTransactionInferer;
class SelfTradeCalibrator;
class SyntheticSignalFusion;

// 双边报价统计
class BilateralQuoteStats;

/// 期货做市策略配置
/// 
/// 注意：模块参数已移至独立配置文件管理：
///   - SpreadOptimizer, AlphaEngine, ToxicityDetector, MarketState, AutoCancel 
///     参数在 coordinator.yaml 的 modules 节点
///   - SpreadArbitrage 参数在 spread_arbitrage.yaml
///   - SelfTradePrevention 参数在 coordinator.yaml 的 modules 节点
struct FutuMmConfig
{
    //==========================================================================
    // 合约配置
    //==========================================================================
    std::string anchor_code;        // 主力合约（对冲用）
    std::vector<std::string> codes; // 做市合约列表
    
    //==========================================================================
    // 配置文件路径
    //==========================================================================
    std::string coordinator_config;     // Coordinator 配置文件路径
    std::string spread_arbitrage_config; // 套利模块配置文件路径
    
    //==========================================================================
    // 组合级 Delta 软指标（用于组合级 skew 和对冲决策，不触发风控动作）
    // 注意：这是所有合约的 delta 之和的限制
    //==========================================================================
    double max_delta;               // 组合级 Delta 软限制（用于组合级 skew 计算基准）
    double hedge_threshold;         // 对冲触发阈值
    double max_spread_mult;         // 最大价差倍数
    
    //==========================================================================
    // 报价参数
    //==========================================================================
    uint32_t num_levels;            // 档位数量
    double base_spread;             // 基础价差(tick)
    double base_qty;                // 基础量
    double qty_decay;               // 外层衰减
    double level_step;              // 档位间距
    
    //==========================================================================
    // 库存管理参数
    //==========================================================================
    double max_inventory;           // 最大库存
    double skew_factor;             // 库存倾斜因子 (φ in GLFT model)
    double max_skew;                // 最大倾斜值
    double hedge_ratio;             // 对冲比例
    double target_inventory;        // 目标库存
    
    //==========================================================================
    // 硬风控指标（不得突破）
    //==========================================================================
    double max_exposure;            // 最大暴露（硬限制）
    
    //==========================================================================
    // Sticky 策略参数 (减少频繁撤单重报)
    //==========================================================================
    double sticky_threshold;        // 价格粘性阈值(tick)
    double improve_retreat_ratio;   // 改善/撤退容忍比 (不对称粘性, default: 2.0)
    
    //==========================================================================
    // 价格验证参数 (防止异常报价)
    //==========================================================================
    double max_price_deviation;     // 最大价格偏离(tick), 0=不限制
    
    //==========================================================================
    // 做市报价价格保护参数（传递给 FutuQuoter）
    //==========================================================================
    bool price_protection;          // 是否启用价格保护 (default: true)
    double protect_ticks;           // 价格保护tick数 (default: 1.0)
    
    //==========================================================================
    // 增强 Skew 参数 (更激进的库存回归)
    //==========================================================================
    double skew_sensitivity;        // Skew 灵敏度系数 (非线性增强)
    double aggressive_skew_threshold;  // 激进 Skew 阈值 (delta利用率)
    double one_sided_threshold;     // 单向报价阈值 (delta利用率)
    
    //==========================================================================
    // 下单错误处理参数
    //==========================================================================
    uint32_t order_error_threshold; // 触发暂停的连续下单错误次数
    
    //==========================================================================
    // 收盘前平仓参数
    //==========================================================================
    uint32_t closeout_minutes_before;   // 收盘前多少分钟停止报价 (0=不启用)
    bool closeout_flatten_position;     // 收盘前是否平掉所有敞口
    uint32_t close_time;                // 收盘时间 (HHMMSS格式)
    
    //==========================================================================
    // 风控参数
    //==========================================================================
    double max_daily_loss;          // 最大日内亏损
    
    //==========================================================================
    // 模块开关 (已废弃 - 从 CoordinatorConfig 同步)
    // 注: 以下字段仅为向后兼容保留，实际值从 coordinator 获取
    // 注：use_alpha_engine 和 use_market_state 已移除，由 SignalAggregator 管理
    //==========================================================================
    bool use_spread_optimizer;      // [deprecated] 从 coordinator 同步
    bool use_auto_cancel;           // [deprecated] 从 coordinator 同步
    // use_market_state 已移除 - 由 SignalAggregator 管理
    // use_alpha_engine 已移除 - 由 SignalAggregator 管理
    bool use_toxicity_detector;     // [deprecated] 从 coordinator 同步
    bool use_synthetic_transaction; // [deprecated] 从 coordinator 同步
    bool use_adaptive_param;        // [deprecated] 从 coordinator 同步
    bool use_performance_monitor;   // 使用性能监控
    bool use_performance_analyzer;  // 使用绩效分析
    
    //==========================================================================
    // GLFT 核心参数已移至 ModuleParams (spread_phi)
    // 注: spread_phi 现在从 coordinator.yaml 的 spreadOptimizer.phi 读取
    
    //==========================================================================
    // 毒性检测参数 (策略级运行时使用)
    //==========================================================================
    uint32_t toxicity_cooloff_ms;   // 毒性冷却时间(ms)
    
    //==========================================================================
    // 风控参数 (放在 config.yaml)
    //==========================================================================
    // FutuRiskMonitor 参数
    uint32_t risk_max_orders_per_sec;
    uint32_t risk_max_cancels_per_sec;
    uint32_t risk_max_trades_per_sec;
    uint32_t risk_cooldown_ms;
    uint32_t risk_check_interval_ms;
    double   risk_recovery_threshold;
    
    //==========================================================================
    // 性能监控参数 (放在 config.yaml)
    //==========================================================================
    // PerformanceAnalyzer 参数
    uint32_t perf_analyzer_window_size;
    double perf_analyzer_risk_free_rate;
    
    // PerformanceMonitor 参数
    uint64_t perf_monitor_latency_threshold;
    
    //==========================================================================
    // 策略模式开关 (独立控制)
    //==========================================================================
    bool use_market_making;          // 启用做市策略
    bool use_spread_arbitrage;       // 启用跨期价差套利

    //==========================================================================
    // 双边报价参数
    //==========================================================================
    bool   use_bilateral_quote;      // 是否使用双边报价接口
    double max_obligation_spread;    // 最大做市义务价差(ticks)
    
    //==========================================================================
    // 构造函数 - 默认值
    //==========================================================================
    FutuMmConfig()
        : coordinator_config("coordinator.yaml")
        , spread_arbitrage_config("spread_arbitrage.yaml")
        , max_delta(5000000.0)         // Delta 软限制（用于 skew 计算基准）
        , hedge_threshold(3000000.0)
        , max_spread_mult(3.0)
        , num_levels(3)
        , base_spread(2.0)
        , base_qty(1.0)
        , qty_decay(0.7)
        , level_step(1.0)
        , max_inventory(100.0)
        , skew_factor(0.01)
        , max_skew(5.0)
        , hedge_ratio(0.5)
        , target_inventory(0)
        , max_exposure(300000000.0)    // 硬风控指标
        , sticky_threshold(1.0)
        , improve_retreat_ratio(2.0)
        , max_price_deviation(20.0)
        , price_protection(true)
        , protect_ticks(1.0)
        , skew_sensitivity(2.0)
        , aggressive_skew_threshold(0.5)
        , one_sided_threshold(0.8)
        , order_error_threshold(3)
        , closeout_minutes_before(5)
        , closeout_flatten_position(true)
        , max_daily_loss(-50000.0)
        , use_spread_optimizer(true)
        , use_auto_cancel(true)
        // use_market_state 已移除
        // use_alpha_engine 已移除
        , use_toxicity_detector(true)
        , use_synthetic_transaction(true)
        , use_adaptive_param(false)
        , use_performance_monitor(false)
        , use_performance_analyzer(false)
        , toxicity_cooloff_ms(5000)
        , risk_max_orders_per_sec(50)
        , risk_max_cancels_per_sec(30)
        , risk_max_trades_per_sec(20)
        , risk_cooldown_ms(30000)
        , risk_check_interval_ms(5000)
        , risk_recovery_threshold(0.8)
        , perf_analyzer_window_size(1000)
        , perf_analyzer_risk_free_rate(0.02)
        , perf_monitor_latency_threshold(100000)  // 100us
        , use_market_making(true)
        , use_spread_arbitrage(false)
        , use_bilateral_quote(false)
        , max_obligation_spread(100.0)
    {}
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
    void initBusinessModules();
    
    /// 执行收盘前平仓对冲
    void executeCloseoutHedge(IUftStraCtx* ctx);
    
    /// 检查毒性并决定是否熔断
    bool checkToxicityAndCircuitBreak(IUftStraCtx* ctx);
    
    /// 处理跨期价差套利信号
    void processSpreadArbitrage(IUftStraCtx* ctx, const char* stdCode, WTSTickData* tick);
    
    /// 处理套利成交回报
    void onSpreadTrade(IUftStraCtx* ctx, const std::string& pair_id, 
                       const std::string& code, bool is_buy, double qty, double price);
    
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
    
    /// 自适应参数管理器
    std::unique_ptr<AdaptiveParamManager> _param_manager;
    
    /// 绩效分析器
    std::unique_ptr<PerformanceAnalyzer> _perf_analyzer;
    

    
    /// 跨期价差套利管理器
    std::unique_ptr<SpreadArbitrageManager> _spread_arb_manager;
    
    /// 自成交防护模块
    std::unique_ptr<SelfTradePrevention> _stp;
    
    /// 异步套利执行器
    std::unique_ptr<AsyncArbitrageExecutor> _async_arb;
    
    /// 双边报价统计模块（独立模块）
    std::unique_ptr<BilateralQuoteStats> _bilateral_stats;
    
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
        uint32_t close_time;      // 收盘时间 (HHMMSS格式，用于收盘前平仓)
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
    bool _trading_halted;
    bool _use_coordinator_mode;  // true = 使用 StrategyCoordinator (模式B), false = 原有逻辑 (模式A)
    bool _toxicity_paused;       // 毒性熔断状态
    uint64_t _toxicity_resume_time; // 熔断恢复时间
    
    // 风险控制状态（由 FutuRiskMonitor 管理）
    bool _long_blocked;          // 禁止开多
    bool _short_blocked;         // 禁止开空
    bool _quoting_paused;        // 报价暂停
    bool _market_state_paused;   // 市场状态检测暂停报价
    std::unordered_map<std::string, bool> _blocked_contracts; // 单合约封锁
    
    // 当前 tick 数据缓存（避免重复计算）
    uint64_t _current_tick_timestamp;  // 当前 tick 时间戳
    double _current_tick_mid;          // 当前 tick 中间价
    
    // 下单错误处理（统一处理所有下单错误）
    uint32_t _order_error_count;     // 连续下单错误计数
    uint32_t _order_error_threshold; // 触发暂停的连续错误阈值
    
    // 收盘前平仓状态 (now managed by FutuRiskMonitor state machine)
    
    // 参数调优计数器
    uint32_t _tick_count;        // Tick计数器
    uint32_t _param_update_interval; // 参数更新间隔(ticks)
    
    //==========================================================================
    // 热更新参数（运行时可修改，无需重启）
    //==========================================================================
    
    // 报价参数
    double* _hot_base_spread;        // 基础价差
    double* _hot_spread_mult;        // 价差倍数
    double* _hot_base_qty;           // 基础数量
    double* _hot_skew_factor;        // 库存倾斜因子
    double* _hot_max_skew;           // 最大倾斜
    double* _hot_max_inventory;      // 最大库存
    double* _hot_target_inventory;   // 目标库存
    
    // Delta 软指标
    double* _hot_max_delta;          // 最大Delta（软指标）
    double* _hot_hedge_threshold;    // 对冲阈值
    
    // 硬风控参数
    double* _hot_max_daily_loss;     // 最大日内亏损
    uint32_t* _hot_order_error_threshold; // 下单错误阈值
    
    // 毒性检测参数
    double* _hot_toxicity_threshold; // 毒性阈值
    uint32_t* _hot_toxicity_cooloff_ms; // 毒性冷却期
};

} // namespace futu
