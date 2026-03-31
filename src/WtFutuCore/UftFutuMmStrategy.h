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
#include <string>
#include <vector>
#include <memory>
#include <mutex>

NS_WTP_BEGIN
class WTSVariant;
NS_WTP_END

namespace futu {

// 业务模块
class FutuPortfolio;
class FutuQuoter;
class SpreadOptimizer;
class MarketStateDetector;
class AutoCancelPolicy;
class OrderBookAnalyzer;
class FutuRiskMonitor;
class MicroAlphaEngine;
class ToxicFlowDetector;
class PerformanceMonitor;
class AdaptiveParamManager;
class PerformanceAnalyzer;
class CorrelationManager;
class SpreadArbitrageManager;
class SelfTradePrevention;
class AsyncArbitrageExecutor;
class SpreadArbitrageManager;

// 综合信号组件
class TickTransactionInferer;
class SelfTradeCalibrator;
class SyntheticSignalFusion;

/// 期货做市策略配置
struct FutuMmConfig
{
    // 合约配置
    std::string anchor_code;        // 主力合约（对冲用）
    std::vector<std::string> codes; // 做市合约列表
    
    // Delta限制
    double delta_limit;             // Delta阈值
    double hedge_threshold;         // 对冲触发阈值
    double max_spread_mult;         // 最大价差倍数
    
    // 报价参数
    uint32_t num_levels;            // 档位数量
    double base_spread;             // 基础价差(tick)
    double base_qty;                // 基础量
    double qty_decay;               // 外层衰减
    double level_step;              // 档位间距
    
    // 库存管理参数
    double max_inventory;           // 最大库存
    double skew_factor;             // 库存倾斜因子 (φ in GLFT model)
    double max_skew;                // 最大倾斜值
    double hedge_ratio;             // 对冲比例
    double target_inventory;        // 目标库存
    double max_delta;               // 最大组合Delta
    double max_exposure;            // 最大暴露
    
    // Sticky 策略参数 (减少频繁撤单重报)
    double sticky_threshold;        // 价格粘性阈值(tick)
    
    // 增强 Skew 参数 (更激进的库存回归)
    double skew_sensitivity;        // Skew 灵敏度系数 (非线性增强)
    double aggressive_skew_threshold;  // 激进 Skew 阈值 (delta利用率)
    double one_sided_threshold;     // 单向报价阈值 (delta利用率)
    
    // 下单错误处理参数（统一处理所有下单错误）
    uint32_t order_error_threshold; // 触发暂停的连续下单错误次数
    
    // 收盘前平仓参数（收盘时间从基础数据获取）
    uint32_t closeout_minutes_before;   // 收盘前多少分钟停止报价 (0=不启用)
    bool closeout_flatten_position;     // 收盘前是否平掉所有敞口
    
    // 风控参数
    double max_daily_loss;          // 最大日内亏损
    
    // 高级开关
    bool use_spread_optimizer;      // 使用价差优化器 (GLFT)
    bool use_auto_cancel;           // 使用自动撤单
    bool use_market_state;          // 使用市场状态检测
    bool use_order_book;            // 使用订单簿分析
    bool use_alpha_engine;          // 使用Alpha预测引擎
    bool use_toxicity_detector;     // 使用毒性检测
    bool use_adaptive_param;        // 使用自适应参数调整（默认关闭）
    bool use_performance_monitor;   // 使用性能监控（默认关闭，影响延迟）
    bool use_performance_analyzer;  // 使用绩效分析（默认关闭）
    
    // GLFT SpreadOptimizer 参数
    double spread_vol_sensitivity;
    double spread_depth_sensitivity;
    double spread_phi;                  // GLFT库存惩罚系数
    uint32_t spread_vol_window;
    double spread_min_mult;
    double spread_portfolio_skew_weight; // 组合级倾斜权重
    double spread_min_correlation;       // 最小相关系数阈值
    
    // MicroAlpha Engine 参数
    double alpha_sensitivity;       // Alpha对价格的影响权重 (η)
    double alpha_ofi_weight;        // OFI权重
    double alpha_trade_weight;      // 交易流失衡权重
    double alpha_leadlag_weight;    // 跨品种权重
    double alpha_ema_factor;        // EMA平滑因子
    double alpha_strong_threshold;  // 强信号阈值
    
    // Toxicity Detector 参数
    double toxicity_vpin_threshold;
    uint32_t toxicity_window;
    uint32_t toxicity_cooloff_ms;
    
    // Synthetic Transaction 参数 (for markets without L2 transaction data)
    bool use_synthetic_transaction;    // 使用综合推断信号
    double synthetic_tick_weight;      // Tick推断权重
    double synthetic_book_weight;      // 订单簿权重
    double synthetic_self_trade_weight; // 自身成交校准权重
    uint32_t synthetic_min_samples;    // 最小样本数
    
    // MarketStateDetector 参数
    double market_vol_threshold;
    double market_move_threshold;
    double market_spread_threshold;
    double market_volume_threshold;
    uint32_t market_lookback_ticks;
    uint32_t market_cooldown_ticks;
    
    // AutoCancelPolicy 参数
    uint32_t cancel_max_age_ms;
    double cancel_price_deviation;
    bool cancel_on_state_change;
    bool cancel_on_inventory_limit;
    uint32_t cancel_inventory_limit_cooldown_ms;  // INVENTORY_LIMIT 撤单冷却期
    
    // FutuRiskMonitor 参数
    uint32_t risk_max_orders_per_sec;
    uint32_t risk_max_cancels_per_sec;
    uint32_t risk_max_trades_per_sec;
    
    // Risk Recovery 参数 (恢复机制)
    uint32_t risk_cooldown_ms;          // 冷却期 (毫秒)
    uint32_t risk_check_interval_ms;    // 恢复检查间隔 (毫秒)
    double   risk_recovery_threshold;   // 恢复阈值 (利用率)
    
    // CorrelationManager 参数
    uint32_t correlation_window_size;
    double correlation_min_correlation;
    double correlation_spread_z_threshold;
    
    // AdaptiveParamManager 参数
    uint32_t adaptive_update_interval;
    double adaptive_learning_rate;
    double adaptive_min_phi;
    double adaptive_max_phi;
    
    // PerformanceAnalyzer 参数
    uint32_t perf_analyzer_window_size;
    double perf_analyzer_risk_free_rate;
    
    // PerformanceMonitor 参数
    uint64_t perf_monitor_latency_threshold;
    
    // Spread Arbitrage 参数
    bool use_spread_arbitrage;       // 启用跨期价差套利
    bool spread_arb_enhance_mm;      // 价差信号增强做市
    double spread_arb_max_position;  // 价差套利最大仓位
    double spread_arb_entry_z;       // 价差入场Z-Score阈值
    double spread_arb_exit_z;        // 价差离场Z-Score阈值
    uint32_t spread_arb_window;      // 价差统计窗口
    
    FutuMmConfig()
        : delta_limit(50.0)
        , hedge_threshold(30.0)
        , max_spread_mult(3.0)
        , num_levels(3)
        , base_spread(2.0)
        , base_qty(1.0)
        , qty_decay(0.7)
        , level_step(1.0)
        , max_inventory(100.0)
        , skew_factor(0.01)         // GLFT φ
        , max_skew(5.0)
        , hedge_ratio(0.5)
        , target_inventory(0)
        , max_delta(100.0)
        , max_exposure(300.0)
        , sticky_threshold(1.0)     // 1个tick的粘性阈值
        , skew_sensitivity(2.0)     // Skew 灵敏度系数
        , aggressive_skew_threshold(0.5)  // 50%利用率开始激进
        , one_sided_threshold(0.8)  // 80%利用率单向报价
        , order_error_threshold(3)  // 3次下单错误暂停
        , closeout_minutes_before(5)    // 默认收盘前5分钟停止报价
        , closeout_flatten_position(true) // 默认收盘前平仓
        , max_daily_loss(-50000.0)
        , use_spread_optimizer(true)
        , use_auto_cancel(true)
        , use_market_state(true)
        , use_order_book(false)
        , use_alpha_engine(true)
        , use_toxicity_detector(true)
        , use_adaptive_param(false)      // 默认关闭
        , use_performance_monitor(false) // 默认关闭，影响延迟
        , use_performance_analyzer(false)// 默认关闭
        , spread_vol_sensitivity(1.0)
        , spread_depth_sensitivity(0.5)
        , spread_phi(0.01)
        , spread_vol_window(100)
        , spread_min_mult(0.5)
        , spread_portfolio_skew_weight(0.5)
        , spread_min_correlation(0.5)
        , alpha_sensitivity(0.5)    // η
        , alpha_ofi_weight(0.4)
        , alpha_trade_weight(0.3)
        , alpha_leadlag_weight(0.3)
        , alpha_ema_factor(0.3)
        , alpha_strong_threshold(0.7)
        , toxicity_vpin_threshold(0.7)
        , toxicity_window(50)
        , toxicity_cooloff_ms(5000)
        , use_synthetic_transaction(true)     // 默认启用综合推断
        , synthetic_tick_weight(0.4)
        , synthetic_book_weight(0.4)
        , synthetic_self_trade_weight(0.2)
        , synthetic_min_samples(5)
        , market_vol_threshold(0.003)
        , market_move_threshold(0.005)
        , market_spread_threshold(5.0)
        , market_volume_threshold(10.0)
        , market_lookback_ticks(50)
        , market_cooldown_ticks(20)
        , cancel_max_age_ms(5000)
        , cancel_price_deviation(3.0)
        , cancel_on_state_change(true)
        , cancel_on_inventory_limit(true)
        , cancel_inventory_limit_cooldown_ms(2000)  // 2秒冷却期
        , risk_max_orders_per_sec(50)
        , risk_max_cancels_per_sec(30)
        , risk_max_trades_per_sec(20)
        , risk_cooldown_ms(30000)            // 30秒冷却期
        , risk_check_interval_ms(5000)       // 5秒检查间隔
        , risk_recovery_threshold(0.8)       // 80%利用率恢复阈值
        , correlation_window_size(100)
        , correlation_min_correlation(0.5)
        , correlation_spread_z_threshold(2.0)
        , adaptive_update_interval(100)
        , adaptive_learning_rate(0.01)
        , adaptive_min_phi(0.001)
        , adaptive_max_phi(0.1)
        , perf_analyzer_window_size(100)
        , perf_analyzer_risk_free_rate(0.0)
        , perf_monitor_latency_threshold(100000)
        , use_spread_arbitrage(false)     // 默认关闭
        , spread_arb_enhance_mm(true)     // 默认启用做市增强
        , spread_arb_max_position(20.0)
        , spread_arb_entry_z(2.0)
        , spread_arb_exit_z(0.5)
        , spread_arb_window(200)
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

private:
    //==========================================================================
    // 内部方法
    //==========================================================================
    
    /// 初始化业务模块
    void initBusinessModules();
    
    /// 处理报价逻辑 (GLFT+Alpha)
    void processQuoting(IUftStraCtx* ctx, const char* stdCode, WTSTickData* tick);
    
    /// 检查并执行对冲
    void checkAndHedge(IUftStraCtx* ctx);
    
    /// 检查风险限制
    void checkRiskLimits(IUftStraCtx* ctx);
    
    /// 处理自动撤单
    void processAutoCancel(IUftStraCtx* ctx, const char* stdCode, double mid);
    
    /// 执行收盘前平仓对冲
    void executeCloseoutHedge(IUftStraCtx* ctx);
    
    /// 检查毒性并决定是否熔断
    bool checkToxicityAndCircuitBreak(IUftStraCtx* ctx);
    
    /// 处理跨期价差套利信号
    void processSpreadArbitrage(IUftStraCtx* ctx, const char* stdCode, WTSTickData* tick);
    
    /// 执行跨期套利信号（含自成交检测）
    void executeSpreadSignal(IUftStraCtx* ctx, const SpreadSignal& signal);
    
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
    
    /// 多档位报价引擎（每合约一个）
    std::vector<std::pair<std::string, std::unique_ptr<FutuQuoter>>> _quoters;
    
    /// GLFT价差优化器（每合约一个）
    std::vector<std::pair<std::string, std::unique_ptr<SpreadOptimizer>>> _spread_optimizers;
    
    /// Micro-Alpha预测引擎（每合约一个）
    std::vector<std::pair<std::string, std::unique_ptr<MicroAlphaEngine>>> _alpha_engines;
    
    std::unordered_map<std::string, std::unique_ptr<MarketStateDetector>> _market_states;
    
    /// 自动撤单策略
    std::unique_ptr<AutoCancelPolicy> _auto_cancel;
    
    std::unordered_map<std::string, std::unique_ptr<OrderBookAnalyzer>> _order_books;
    
    /// 风险监控
    std::unique_ptr<FutuRiskMonitor> _risk_monitor;
    
    /// 毒性检测器 (VPIN)
    std::unique_ptr<ToxicFlowDetector> _toxicity_detector;
    
    //==========================================================================
    // 综合信号组件 (for markets without L2 transaction data)
    //==========================================================================
    
    /// Tick级交易推断器（每合约一个）
    std::unordered_map<std::string, std::unique_ptr<TickTransactionInferer>> _tick_inferers;
    
    /// 自身成交校准器
    std::unique_ptr<SelfTradeCalibrator> _self_trade_calibrator;
    
    /// 综合信号融合器（每合约一个）
    std::unordered_map<std::string, std::unique_ptr<SyntheticSignalFusion>> _signal_fusions;
    
    //==========================================================================
    // 性能监控
    //==========================================================================
    
    /// 性能监控
    std::unique_ptr<PerformanceMonitor> _performance_monitor;
    
    /// 自适应参数管理器
    std::unique_ptr<AdaptiveParamManager> _param_manager;
    
    /// 绩效分析器
    std::unique_ptr<PerformanceAnalyzer> _perf_analyzer;
    
    /// 跨合约相关性管理器（用于组合级库存偏移）
    std::unique_ptr<CorrelationManager> _correlation_manager;
    
    /// 跨期价差套利管理器
    std::unique_ptr<SpreadArbitrageManager> _spread_arb_manager;
    
    /// 自成交防护模块
    std::unique_ptr<SelfTradePrevention> _stp;
    
    /// 异步套利执行器
    std::unique_ptr<AsyncArbitrageExecutor> _async_arb;
    
    //==========================================================================
    // 辅助数据
    //==========================================================================
    
    // 合约信息缓存
    struct ContractInfo {
        std::string code;
        double multiplier;
        double tick_size;
        double max_position;      // 单合约最大持仓
        double max_delta;         // 单合约最大 Delta
    };
    std::vector<ContractInfo> _contract_infos;
    
    // 当前 tick 中间价缓存
    wtp::wt_hashmap<std::string, double> _last_mid;
    
    // 运行状态
    bool _channel_ready;
    bool _trading_halted;
    bool _toxicity_paused;       // 毒性熔断状态
    uint64_t _toxicity_resume_time; // 熔断恢复时间
    
    // 风险控制状态（由 FutuRiskMonitor 管理）
    bool _long_blocked;          // 禁止开多
    bool _short_blocked;         // 禁止开空
    bool _quoting_paused;        // 报价暂停
    std::unordered_map<std::string, bool> _blocked_contracts; // 单合约封锁
    
    // 下单错误处理（统一处理所有下单错误）
    uint32_t _order_error_count;     // 连续下单错误计数
    uint32_t _order_error_threshold; // 触发暂停的连续错误阈值
    
    // 收盘前平仓状态
    bool _closeout_triggered;         // 收盘前平仓已触发
    bool _closeout_completed;         // 收盘前平仓已完成
    uint32_t _closeout_start_time;    // 平仓开始时间 (HHMMSS)
    
    // 参数调优计数器
    uint32_t _tick_count;        // Tick计数器
    uint32_t _param_update_interval; // 参数更新间隔(ticks)
};

} // namespace futu
