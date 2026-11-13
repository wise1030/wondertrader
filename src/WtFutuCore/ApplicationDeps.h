// ApplicationDeps.h
// 19.33: 全应用单例依赖的唯一权威聚合 (Deps 卫生化重构)。
//
// 设计裁决 (对齐 19.19 "独立组件=值拷贝" 与 P1-6 "不拆类"):
//   - 组件不持引用/指针指向本聚合, 而在 setDeps(const ApplicationDeps&)
//     时按值切片拷贝自身所需子集 (成员名与字段名全集合一致, 消费点
//     `_deps.xxx` 零改动);
//   - 消灭的是"填充多源"(原 5 Deps 在 assembler/coordinator/runtimeops
//     三处各填一份, RiskLiquidator 有两个填充源), 定义级子集视图保留
//     于本结构内, 单一 fill 权威 = FutuModuleAssembler 一次构建 +
//     StrategyCoordinator::wireDeps 链式下发;
//   - validate(): 启动期 fail-fast 校验核心非空字段, 漏接线即刻报错。
//
// 本头仅前向声明 + 标准库, 无循环包含风险。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../Includes/FasterDefs.h" // wtp::wt_hashmap

namespace wtp
{
class IUftStraCtx;
}

namespace futu
{

// 前向声明 (全部以指针/容器指针形式借用, 生命周期归 UftFutuMmStrategy 壳)
class AsyncArbitrageExecutor;
class CloseoutExecutor;
class CloseoutOrchestrator;
class CorrelationManager;
class FutuPortfolio;
class HaltReasons;
class IOrderSink;
class FutuQuoter;
class FutuRiskMonitor;
class MarketDataContext;
class OrderRouter;
class PerformanceMonitor;
class QuotePolicyChain;
class SelfTradeCalibrator;
class SelfTradePrevention;
class SignalAggregator;
class SpreadArbitrageManager;
class SpreadOptimizer;
class ToxicFlowDetector;
class TradingState;
class UnifiedOrderTracker;
struct ContractInfo;
struct CoordinatorConfig;

/// 19.33: 应用级依赖单聚合 (超集; 各组件按值切片)
struct ApplicationDeps
{
    // ---- 核心 (required, 启动期 validate 强制非空) ----
    FutuPortfolio* portfolio = nullptr;
    UnifiedOrderTracker* order_tracker = nullptr;
    FutuRiskMonitor* risk_monitor = nullptr;
    OrderRouter* order_router = nullptr;
    IOrderSink* router = nullptr;  ///< RiskLiquidator 成员别名 (_deps.router, B6 窄接口), 与 order_router 同源填充
    TradingState* trading_state = nullptr;

    // ---- 借用容器指针 (生命周期: 策略壳, 须 outlive 全部组件) ----
    wtp::wt_hashmap<std::string, std::unique_ptr<FutuQuoter>>* quoters = nullptr;
    wtp::wt_hashmap<std::string, std::unique_ptr<SpreadOptimizer>>* spread_opts = nullptr;
    std::unordered_map<std::string, std::unique_ptr<MarketDataContext>>* market_data = nullptr;
    std::unordered_map<std::string, std::unique_ptr<SignalAggregator>>* signal_aggregators = nullptr;
    const std::vector<ContractInfo>* contract_infos = nullptr;

    // ---- 可选模块 (nullable, 按配置启停) ----
    ToxicFlowDetector* toxicity = nullptr;
    PerformanceMonitor* perf_monitor = nullptr;
    SelfTradeCalibrator* self_trade_calibrator = nullptr;
    CorrelationManager* correlation_manager = nullptr;
    AsyncArbitrageExecutor* arb_executor = nullptr;  ///< nullable (arb disabled)
    AsyncArbitrageExecutor* async_arb = nullptr;     ///< ArbExecutionBridge 成员别名, 与 arb_executor 同源
    SpreadArbitrageManager* arb_manager = nullptr;   ///< nullable (arb disabled)
    SelfTradePrevention* stp = nullptr;              ///< nullable (STP disabled)
    QuotePolicyChain* quote_chain = nullptr;         ///< RiskCoordinator 引用 (与 coordinator 共享链)
    HaltReasons* halt_reasons = nullptr;             ///< §20.9 批 1: 本策略 halt 原因位图 (owner=TradingState; 镜像写入用, 可空)
    CloseoutExecutor* executor = nullptr;            ///< CloseoutOrchestrator 执行器
    CloseoutOrchestrator* closeout_orch = nullptr;   ///< 19.34: Stage0 触发决策调用通道 (coordinator 持有, 非拥有)

    // ---- 配置与元信息 ----
    const CoordinatorConfig* cfg = nullptr;      ///< coordinator 配置快照 (Trigger/RiskCoord)
    const std::string* anchor_code = nullptr;    ///< CloseoutOrchestrator anchor
    uint32_t close_time = 150000;                ///< HHMMSS 收盘平仓时点
    bool flatten_position = true;                ///< 收盘是否 flatten
    const char* strategy_id = "";                ///< 日志标签
    bool use_spread_arbitrage = false;           ///< arb 模块开关 (Bridge)

    // ---- 行为挂钩 (单点填充: cancel_all_quotes 由 assembler 填,
    //       flush_bilateral_stats 由 coordinator wireDeps 填) ----
    std::function<void(wtp::IUftStraCtx*)> cancel_all_quotes;
    std::function<void(wtp::IUftStraCtx*, uint32_t hhmm, uint32_t secs)> flush_bilateral_stats;

    // ---- 策略层恢复状态直通挂钩 (原子指针, Md 单写者线程) ----
    std::atomic<uint64_t>* quoting_paused_since = nullptr;
    std::atomic<uint32_t>* order_error_count = nullptr;

    /// 19.33: 启动期 fail-fast — 核心字段缺失即 error 日志并返回 false。
    /// owner 标签定位漏接线组件; optional 字段不参与校验。
    bool validate(const char* owner) const;
};

} // namespace futu
