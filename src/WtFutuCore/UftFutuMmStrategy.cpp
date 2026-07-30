/*!
* \file UftFutuMmStrategy.cpp
* \brief GLFT+Alpha Market-Making Strategy Implementation (as UFT Strategy)
* 
* 集成业务模块：
*   - FutuPortfolio: 持仓管理、Delta计算、对冲
*   - FutuQuoter: 多档位报价执行
*   - SpreadOptimizer: GLFT价差优化
*   - MicroAlphaEngine: Alpha预测引擎
*   - ToxicFlowDetector: VPIN毒性检测
*   - AutoCancelPolicy: 自动撤单策略
*   - FutuRiskMonitor: 风险监控
*   - Level2DataAdapter: Level2数据适配
*   - PerformanceAnalyzer: 绩效分析
*/
#include "UftFutuMmStrategy.h"
#include "../WtUftCore/UftStraContext.h"  // R1: dynamic_cast 获取 EventNotifier
#include "../Includes/IUftStraCtx.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSMarcos.h"
#include "../WTSTools/WTSLogger.h"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/TimeUtils.hpp"

// 业务模块头文件
#include "FutuPortfolio.h"
#include "FutuQuoter.h"
#include "SpreadOptimizer.h"

#include "UnifiedOrderTracker.h"
#include "MarketDataContext.h"
#include "CloseoutExecutor.h"
#include "FutuRiskMonitor.h"
#include "ToxicFlowDetector.h"
#include "PerformanceAnalyzer.h"
#include "PerformanceMonitor.h"
#include "SpreadArbitrageManager.h"
#include "FutuComponentFactory.h"
#include "FutuConfigLoader.h"
#include "SelfTradePrevention.h"
#include "OrderRouter.h"
#include "StrategyCoordinator.h"
#include "AsyncArbitrageExecutor.h"
#include "BilateralQuoteStats.h"
#include "CorrelationManager.h"

// 综合信号组件头文件
#include "TickTransactionInferer.h"
#include "SelfTradeCalibrator.h"
#include "SignalAggregator.h"  // 新增：信号聚合器
#include "SyntheticSignalFusion.h"
#include "FutuConfigValidator.h"  // 配置校验

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <chrono>

namespace futu {

//==========================================================================
// 辅助函数：从 WTSVariant 读取参数（带默认值）
//==========================================================================

namespace {

// 读取 double 参数
double readDouble(WTSVariant* cfg, const char* key, double defVal)
{
if (!cfg) return defVal;
WTSVariant* node = cfg->get(key);
return node ? node->asDouble() : defVal;
}

// 读取 uint32_t 参数
uint32_t readUInt32(WTSVariant* cfg, const char* key, uint32_t defVal)
{
if (!cfg) return defVal;
WTSVariant* node = cfg->get(key);
return node ? node->asUInt32() : defVal;
}

// 读取 bool 参数
bool readBool(WTSVariant* cfg, const char* key, bool defVal)
{
if (!cfg) return defVal;
WTSVariant* node = cfg->get(key);
return node ? node->asBoolean() : defVal;
}

// 读取 string 参数
std::string readString(WTSVariant* cfg, const char* key, const char* defVal)
{
if (!cfg) return defVal;
WTSVariant* node = cfg->get(key);
return node ? node->asString() : defVal;
}

/**
* @brief 将 fullCode 转换为 stdCode 格式
* 
* fullCode 格式: SHFE.ag2606 (交易所.合约代码)
* stdCode 格式:  SHFE.ag.2606 (交易所.品种.月份)
* 
* 使用 CodeHelper::rawMonthCodeToStdCode 实现转换
*/
std::string fullCodeToStdCode(const std::string& fullCode)
{
// 查找第一个点，分离交易所和合约代码
size_t firstDot = fullCode.find('.');
if (firstDot == std::string::npos)
return fullCode;  // 没有点，直接返回

std::string exchg = fullCode.substr(0, firstDot);
std::string code = fullCode.substr(firstDot + 1);

// 使用 CodeHelper 进行转换
return CodeHelper::rawMonthCodeToStdCode(code.c_str(), exchg.c_str());
}

} // anonymous namespace

//==========================================================================
// 构造/析构
//==========================================================================

UftFutuMmStrategy::UftFutuMmStrategy(const char* id)
    : UftStrategy(id)
    , _channel_ready(false)
    , _trading_state()  // TradingState default-constructs all false
    , _toxicity_resume_time(0)
    , _order_error_count(0)
    , _quoting_paused_since(0)
, _tick_count(0)
, _param_update_interval(100)
// P0-2.3: PortfolioContext cache - dirty on start to force first build
, _portfolio_ctx_dirty(true)
// 热更新参数指针初始化
, _hot_mgr()
{
}

UftFutuMmStrategy::~UftFutuMmStrategy()
{
}

//==========================================================================
// 初始化
//==========================================================================

bool UftFutuMmStrategy::init(WTSVariant* cfg)
{
if (!cfg)
return false;

// 配置解析与校验已拆分至 FutuConfigLoader (架构重构 C1)
    return FutuConfigLoader::load(cfg, _config, _contract_infos, id());
}

//==========================================================================
// R1: 告警通道注入 + 套利 alert 转发 (策略层作为组合根)
//==========================================================================

void UftFutuMmStrategy::setEventNotifier(wtp::EventNotifier* notifier)
{
    _event_notifier = notifier;
    // RiskMonitor: 直达 (既有 22 处 broadcastAlert 立即生效)
    if (_risk_monitor)
        _risk_monitor->setEventNotifier(notifier);
    // ArbManager: 经回调 → 本策略 handleRiskAlert → EventNotifier
    // (解耦: ArbManager 不直接依赖 EventNotifier, 保持模块边界清晰)
    if (_spread_arb_manager)
    {
        _spread_arb_manager->setAlertCallback(
            [this](const RiskAlert& alert) { this->handleRiskAlert(alert); });
    }
}

void UftFutuMmStrategy::handleRiskAlert(const RiskAlert& alert)
{
    WTSLogger::warn("[ARB_RISK] pair={} level={} type={}: {}",
        alert.pair_id, static_cast<int>(alert.level),
        static_cast<int>(alert.type), alert.message);

    // 转发到 EventNotifier (运维侧订阅 nanomsg topic="ARB_RISK")
    if (_event_notifier)
    {
        std::string msg = fmt::format(
            "[L{}] pair={} type={} val={:.2f}/thr={:.2f}: {}",
            static_cast<int>(alert.level), alert.pair_id,
            static_cast<int>(alert.type), alert.value, alert.threshold,
            alert.message);
        _event_notifier->notify("ARB_RISK", msg.c_str());
    }
}

//==========================================================================
// 业务模块初始化
//==========================================================================

void UftFutuMmStrategy::initBusinessModules(wtp::IUftStraCtx* ctx)
{
//------------------------------------------------------------
// 0. 初始化 StrategyCoordinator 获取模块开关及配置
//------------------------------------------------------------
_coordinator = std::make_unique<StrategyCoordinator>();

// Load from coordinator_config
std::string coord_cfg_path = _config.coordinator_config.empty() ? "coordinator.yaml" : _config.coordinator_config;
_coordinator->loadConfig(coord_cfg_path);

// Apply strategy-level settings (final override)
{
const auto& mp = _coordinator->getConfig().modules;
CoordinatorConfig coord_cfg = _coordinator->getConfig();
coord_cfg.closeout_minutes_before = _config.closeout.minutes_before;
coord_cfg.close_time = _config.closeout.close_time;
coord_cfg.closeout_flatten_position = _config.closeout.flatten_position;
coord_cfg.night_close_time = _config.closeout.night_close_time;
coord_cfg.night_minutes_before = _config.closeout.night_minutes_before;
coord_cfg.perf_monitor_latency_threshold = _config.perf.monitor_latency_threshold;
coord_cfg.perf_enabled = _config.perf.enabled;
coord_cfg.perf_log_interval = _config.perf.log_interval;
coord_cfg.perf_warn_threshold_ns = _config.perf.warn_threshold_ns;
coord_cfg.perf_critical_threshold_ns = _config.perf.critical_threshold_ns;
_coordinator->setConfig(coord_cfg);
}

// 从 Coordinator 获取模块开关（来自 coordinator.yaml）
const auto& coord_cfg = _coordinator->getConfig();

// 模块开关（从 coordinator.yaml 读取，而非 config.yaml）
_config.modules.use_toxicity_detector = coord_cfg.use_toxicity_detector;
_config.modules.use_spread_optimizer = coord_cfg.use_spread_optimizer;
_config.modules.use_adaptive_param = coord_cfg.use_adaptive_params;

//==========================================================================
// 模块开关统一解析 (单一权威来源):
//   coordinator.yaml 根级是以下 7 个开关的唯一位置, 不再 fallback 到
//   modules 节点或 config.yaml — 避免多源错配.
//     useMarketMaking / useSpreadArbitrage / useAsyncArbThread
//     usePerformanceMonitor / usePerformanceAnalyzer
//     use_signal_aggregator / useHedging
//   3 个模块级开关(toxicityDetector/spreadOptimizer/adaptiveParam) 只读
//   modules.<name>.enabled, 由 StrategyCoordinator::loadConfigFromVariant 解析.
//   下方 coordBool 找不到键时使用编译期默认(见 Modules 构造函数).
//==========================================================================
{
wtp::WTSVariant* raw = coord_cfg._raw_variant;
auto coordBool = [&](const char* key, bool compile_time_default) -> bool {
    if (raw) {
        wtp::WTSVariant* node = raw->get(key);
        if (node) return node->asBoolean();
    }
    return compile_time_default;
};
_config.modules.use_market_making        = coordBool("useMarketMaking",         true);
_config.modules.use_spread_arbitrage     = coordBool("useSpreadArbitrage",      false);
_config.modules.use_async_arb_thread     = coordBool("useAsyncArbThread",       true);
_config.modules.use_performance_monitor  = coordBool("usePerformanceMonitor",   false);
_config.modules.use_performance_analyzer = coordBool("usePerformanceAnalyzer",  false);
}

WTSLogger::info("Strategy mode: MM={}, Arb={}", 
_config.modules.use_market_making ? "ON" : "OFF",
_config.modules.use_spread_arbitrage ? "ON" : "OFF");

//------------------------------------------------------------
// 1. FutuPortfolio（持仓管理）- 始终需要
//------------------------------------------------------------
_portfolio = std::make_unique<FutuPortfolio>();

PortfolioParams portfolio_params;
portfolio_params.portfolio_max_delta = _config.portfolio.max_delta;
portfolio_params.hedge_ratio = _config.portfolio.hedge_ratio;
portfolio_params.max_exposure = _config.risk.max_exposure;
// max_loss 语义为正容忍度 (RiskMonitor: pnl < -max_loss 触发)。
// 配置历史上正负约定混用(默认 -200000, README 示例 50000), 统一取绝对值防御.
portfolio_params.max_loss = std::abs(_config.risk.max_daily_loss);
_portfolio->setParams(portfolio_params);
_portfolio->setAnchorContract(_config.anchor_code);

WTSLogger::info("FutuPortfolio: maxDelta={} (soft), hedgeRatio={}, maxExposure={}, maxLoss={}",
portfolio_params.portfolio_max_delta, portfolio_params.hedge_ratio,
portfolio_params.max_exposure, portfolio_params.max_loss);

// 添加合约到 Portfolio（包含单合约限制）
for (const auto& ci : _contract_infos)
{
double max_pos = (ci.max_position > 0) ? ci.max_position : 0;
double contract_max_del = (ci.max_delta > 0) ? ci.max_delta : 0;
_portfolio->addContract(ci.code, ci.multiplier, ci.tick_size, 1.0, max_pos, contract_max_del, ci.target_position);
}

//------------------------------------------------------------
// 2. CorrelationManager (相关性与组合极度套利)
//------------------------------------------------------------
if (_config.modules.use_market_making || _config.modules.use_spread_arbitrage)
{
_correlation_manager = std::make_unique<CorrelationManager>();

const auto& mp = coord_cfg.modules;
wtp::WTSVariant* root = coord_cfg._raw_variant;
wtp::WTSVariant* modules_v = root ? root->get("modules") : nullptr;

CorrelationConfig corr_cfg;
if (modules_v) {
wtp::WTSVariant* corr_v = modules_v->get("correlationManager");
if (corr_v) corr_cfg = CorrelationConfig::fromVariant(corr_v);
}
_correlation_manager->setConfig(corr_cfg);

for (size_t i = 0; i < _contract_infos.size(); ++i) {
_correlation_manager->addContract(_contract_infos[i].code, _contract_infos[i].multiplier);
for (size_t j = i + 1; j < _contract_infos.size(); ++j) {
_correlation_manager->addRelation(_contract_infos[i].code, _contract_infos[j].code);
}
}
WTSLogger::info("CorrelationManager: initialized with windowSize={}", corr_cfg.window_size);
}

//------------------------------------------------------------
// 2. FutuQuoter（报价引擎）- 每合约一个 (仅做市)
//------------------------------------------------------------
if (_config.modules.use_market_making)
{
for (const auto& ci : _contract_infos)
{
auto quoter = std::make_unique<FutuQuoter>();

QuoterConfig qcfg;
qcfg.code = ci.code;
qcfg.num_levels = _config.quoting.num_levels;
qcfg.base_spread = _config.quoting.base_spread;
qcfg.level_step = _config.quoting.level_step;
qcfg.base_qty = _config.quoting.base_qty;
qcfg.level_qty_multiplier = _config.quoting.level_qty_multiplier;
qcfg.tick_size = ci.tick_size;
qcfg.sticky_threshold = _config.quoting.sticky_threshold;
qcfg.improve_retreat_ratio = _config.quoting.improve_retreat_ratio;
qcfg.max_price_deviation = _config.quoting.max_price_deviation;

// 价格保护参数
qcfg.price_protection = _config.quoting.price_protection;
qcfg.protect_ticks = _config.quoting.protect_ticks;

// 双边报价参数
qcfg.use_bilateral_quote = _config.quoting.use_bilateral_quote;
qcfg.min_valid_qty = _config.quoting.base_qty;  // 有效挂单最小数量 = 基础挂单量

// v3 软风控参数透传
qcfg.qty_decay_factor = _config.quoting.qty_decay_factor;
qcfg.obligation_min_qty = _config.quoting.obligation_min_qty;
qcfg.obligation_max_spread_ticks = _config.quoting.obligation_max_spread_ticks;
qcfg.obligation_only_l0 = _config.quoting.obligation_only_l0;
qcfg.always_obligation = _config.quoting.always_obligation;

quoter->init(qcfg);
// Note: UnifiedOrderTracker will be set after it's created (in section 5)
_quoters[ci.code] = std::move(quoter);
}

WTSLogger::info("FutuQuoter: {} quoters initialized, {} levels, baseSpread={}",
_quoters.size(), _config.quoting.num_levels, _config.quoting.base_spread);
}
else
{
WTSLogger::info("FutuQuoter: skipped (market making disabled)");
}

//------------------------------------------------------------
// 3. SpreadOptimizer（价差优化器）- 每合约一个 (仅做市)
//------------------------------------------------------------
if (_config.modules.use_market_making && _config.modules.use_spread_optimizer)
{
_coordinator->setPortfolioMaxDelta(_config.portfolio.max_delta);
const CoordinatorConfig& updated_cfg = _coordinator->getConfig();
const auto& mp = updated_cfg.modules;

for (const auto& ci : _contract_infos)
{
auto optimizer = FutuComponentFactory::createSpreadOptimizer(
updated_cfg, ci.code, _config.quoting.base_spread, ci.tick_size);

_spread_optimizers[ci.code] = std::move(optimizer);
}

WTSLogger::info("SpreadOptimizer: {} optimizers, baseSpread={}, maxDelta={} (soft)",
_spread_optimizers.size(), _config.quoting.base_spread, _config.portfolio.max_delta);
}

//------------------------------------------------------------
// 4. UnifiedOrderTracker + AutoCancelPolicy（统一订单跟踪）
//------------------------------------------------------------
_order_tracker = std::make_unique<UnifiedOrderTracker>();

{
const auto& mp = coord_cfg.modules;

UnifiedTrackerConfig tracker_cfg;
tracker_cfg.max_orders = _config.order_control.max_orders;
tracker_cfg.max_age_ms = mp.auto_cancel_max_age_ms;
tracker_cfg.price_deviation = mp.auto_cancel_price_deviation;
tracker_cfg.sticky_threshold = _config.quoting.sticky_threshold;
tracker_cfg.inv_limit_cooldown_ms = mp.auto_cancel_inventory_cooldown_ms;
// STP decoupled from arb (default false), but FORCED true when arb is enabled.
// Reason: arb sends marketable orders via OrderRouter that can cross own MM quotes.
// Without STP, arb would self-trade with its own MM book.
bool stp_effective = _config.order_control.use_stp || _config.modules.use_spread_arbitrage;
if (_config.modules.use_spread_arbitrage && !_config.order_control.use_stp)
{
    WTSLogger::info("STP forced ON because use_spread_arbitrage=true (prevents arb→MM self-trade)");
}
tracker_cfg.stp_enabled = stp_effective;
tracker_cfg.stp_min_price_gap = _config.order_control.stp_min_price_gap;
_order_tracker->setConfig(tracker_cfg);
}

// 为所有 FutuQuoter 设置共享 tracker
for (auto& [code, quoter] : _quoters)
{
quoter->setOrderTracker(_order_tracker.get());
}

// R3 v2: BilateralQuoteStats 已下放到 Per-Quoter 值成员
//   - 每个 quoter 持值成员，setConfig(min_valid_qty/obligation_max_spread_ticks) 来自 quoter 自身 cfg
//   - sessInfo 从 _session_cache 取（on_init 中已用三段式查询并缓存）
//   - 不再重复调 stra_get_comminfo（两段式 code 查不到品种信息）
uint32_t stats_ok = 0;
uint32_t stats_fail = 0;
for (auto& [code, quoter] : _quoters)
{
    WTSSessionInfo* sessInfo = nullptr;
    auto scIt = _session_cache.find(code);
    if (scIt != _session_cache.end())
        sessInfo = scIt->second.sessInfo;
    if (quoter->initBilateralStats(sessInfo))
        stats_ok++;
    else
        stats_fail++;
}
WTSLogger::info("BilateralQuoteStats: Per-Quoter init done, ok={} fail={} (total={})",
    stats_ok, stats_fail, _quoters.size());



WTSLogger::info("UnifiedOrderTracker: initialized (shared by {} FutuQuoters + AutoCancelPolicy + SelfTradePrevention)", 
_quoters.size());

//------------------------------------------------------------
// 5.5 注册模块至 StrategyCoordinator
//------------------------------------------------------------

// Register modules with coordinator
_coordinator->setOrderTracker(_order_tracker.get());
_coordinator->setQuoters(&_quoters);
_coordinator->setSpreadOptimizers(&_spread_optimizers);
_coordinator->setOrderBooks(&_market_data);
_coordinator->setSignalAggregators(&_signal_aggregators);

// Register modules created before coordinator
_coordinator->setPortfolio(_portfolio.get());
_coordinator->setCorrelationManager(_correlation_manager.get());

//------------------------------------------------------------
// 5.6 OrderRouter（统一下单路由器 — 套利/对冲/平仓）
//------------------------------------------------------------
_order_router = std::make_unique<OrderRouter>();
_order_router->setOrderTracker(_order_tracker.get());

// 设置限速：套利30单/秒，对冲10单/秒，平仓不限速
_order_router->setRateLimit(Source::ARBITRAGE, 30, 1000);
_order_router->setRateLimit(Source::HEDGING, 30, 1000);
_order_router->setRateLimit(Source::CLOSEOUT, 0, 1000);  // 0 = 不限速

_coordinator->setOrderRouter(_order_router.get());
WTSLogger::info("OrderRouter: initialized (arb=30/s, hedge=30/s, closeout=unlimited)");

//------------------------------------------------------------
// 5.7 CloseoutExecutor（渐进式收盘对冲执行器）
//------------------------------------------------------------
_closeout_executor = std::make_unique<CloseoutExecutor>();
_closeout_executor->setOrderRouter(_order_router.get());
_closeout_executor->setOrderTracker(_order_tracker.get());
_closeout_executor->setPortfolio(_portfolio.get());
{
    CloseoutExecConfig exec_cfg;
    exec_cfg.drain_timeout_ms    = _config.closeout.drain_timeout_ms;
    exec_cfg.depth_ratio_passive = _config.closeout.depth_ratio_passive;
    exec_cfg.depth_ratio_mid     = _config.closeout.depth_ratio_mid;
    exec_cfg.depth_ratio_aggr    = _config.closeout.depth_ratio_aggr;
    exec_cfg.sweep_threshold_ms  = _config.closeout.sweep_threshold_ms;
    exec_cfg.sweep_ticks         = _config.closeout.sweep_ticks;
    exec_cfg.use_fak             = _config.closeout.use_fak;
    _closeout_executor->setConfig(exec_cfg);
}
WTSLogger::info("CloseoutExecutor: initialized (drain={}ms, sweep_ticks={}, fak={})",
                _config.closeout.drain_timeout_ms,
                _config.closeout.sweep_ticks,
                _config.closeout.use_fak);


// 设置交易时段信息（用于休市检查）
for (const auto& [code, cache] : _session_cache)
{
_coordinator->setSessionInfo(code, cache.sessInfo);
}

// Initialize coordinator internal components
_coordinator->initialize();

WTSLogger::info("StrategyCoordinator: initialized (modules registered)");

//------------------------------------------------------------
// 6. FutuRiskMonitor（风险监控）
//------------------------------------------------------------
_risk_monitor = std::make_unique<FutuRiskMonitor>();

RateLimits rate_limits;
rate_limits.max_orders_per_sec = _config.risk.max_orders_per_sec;
rate_limits.max_cancels_per_sec = _config.risk.max_cancels_per_sec;
rate_limits.max_trades_per_sec = _config.risk.max_trades_per_sec;
rate_limits.max_delta_change_per_sec = _config.risk.max_delta_change_per_sec;
rate_limits.delta_rate_window_sec = _config.risk.delta_rate_window_sec;
rate_limits.delta_rate_cooldown_ms = _config.risk.delta_rate_cooldown_ms;
rate_limits.position_breach_pause_threshold = _config.risk.position_breach_pause_threshold;
rate_limits.delta_critical_mult = _config.risk.delta_critical_mult;
    rate_limits.delta_warning_mult = _config.risk.delta_warning_mult;
    rate_limits.position_warning_l1 = _config.risk.position_warning_l1;
    rate_limits.position_warning_l2 = _config.risk.position_warning_l2;
    rate_limits.widen_threshold = _config.risk.widen_threshold;
    rate_limits.flatten_threshold = _config.risk.flatten_threshold;
_risk_monitor->setRateLimits(rate_limits);

// 设置恢复配置
RecoveryConfig recovery_cfg;
recovery_cfg.cooldown_ms = _config.risk.cooldown_ms;
recovery_cfg.check_interval_ms = _config.risk.check_interval_ms;
recovery_cfg.recovery_threshold = _config.risk.recovery_threshold;
recovery_cfg.max_recovery_count = _config.risk.max_recovery_count;
recovery_cfg.pnl_recovery_ratio = _config.risk.pnl_recovery_ratio;
recovery_cfg.max_loss_for_recovery = _config.risk.max_loss_for_recovery;
recovery_cfg.auto_clear_irreversible_on_reset = _config.risk.auto_clear_irreversible_on_reset;
_risk_monitor->setRecoveryConfig(recovery_cfg);

CloseoutConfig closeout_cfg;
closeout_cfg.minutes_before = _config.closeout.minutes_before;
closeout_cfg.max_retries = _config.closeout.max_retries;
closeout_cfg.retry_interval_ms = _config.closeout.retry_interval_ms;
closeout_cfg.night_close_time = _config.closeout.night_close_time;
closeout_cfg.night_minutes_before = _config.closeout.night_minutes_before;
_risk_monitor->setCloseoutConfig(closeout_cfg);

WTSLogger::info("FutuRiskMonitor: maxOrdersPerSec={}, maxCancelsPerSec={}, cooldownMs={}, recoveryThreshold={}, maxDeltaChangePerSec={}",
_config.risk.max_orders_per_sec, _config.risk.max_cancels_per_sec,
_config.risk.cooldown_ms, _config.risk.recovery_threshold,
_config.risk.max_delta_change_per_sec);

// Register with coordinator
if (_coordinator) {
_coordinator->setRiskMonitor(_risk_monitor.get());
}

// CloseoutOrchestrator 依赖注入 (架构重构 C3) — 所有依赖模块至此均已创建
{
    CloseoutOrchestrator::Deps orch_deps;
    orch_deps.executor = _closeout_executor.get();
    orch_deps.risk_monitor = _risk_monitor.get();
    orch_deps.portfolio = _portfolio.get();
    orch_deps.trading_state = &_trading_state;
    orch_deps.order_router = _order_router.get();
    orch_deps.quoters = &_quoters;
    orch_deps.anchor_code = &_config.anchor_code;
    orch_deps.close_time = _config.closeout.close_time;
    orch_deps.flatten_position = _config.closeout.flatten_position;
    orch_deps.strategy_id = id();
    _closeout_orch.setDeps(orch_deps);
}

//------------------------------------------------------------
// 7. MarketDataContext（核心行情上下文）
//------------------------------------------------------------
for (const auto& ci : _contract_infos)
{
_market_data.emplace(ci.code, 
FutuComponentFactory::createMarketDataContext(coord_cfg));
}
WTSLogger::info("MarketDataContext: mandatory core enabled");

//------------------------------------------------------------
// 7.1 SignalAggregator（信号聚合器）- 新信号架构
//------------------------------------------------------------
if (_config.modules.use_market_making && coord_cfg.use_signal_aggregator)
{
SignalAggregatorConfig sig_cfg;
wtp::WTSVariant* root = coord_cfg._raw_variant;
wtp::WTSVariant* modules_v = root ? root->get("modules") : nullptr;
if (modules_v) {
wtp::WTSVariant* sig_v = modules_v->get("signalAggregator");
if (sig_v) sig_cfg = SignalAggregatorConfig::fromVariant(sig_v);
}

// model.type 校验失败 -> 跳过 SignalAggregator 初始化
if (!sig_cfg.valid) {
WTSLogger::error("SignalAggregator: config invalid (unsupported model type), skipping initialization");
return;
}

for (const auto& ci : _contract_infos)
{
auto aggregator = std::make_unique<SignalAggregator>(sig_cfg);

// Configure LeadLag: anchor contract is the lead for all non-anchor contracts
if (sig_cfg.use_lead_lag && !_config.anchor_code.empty())
{
if (ci.code != _config.anchor_code)
{
// Non-anchor contract: anchor is its lead
aggregator->addLeadContract(_config.anchor_code, 1.0);
}
// Anchor contract itself doesn't need a lead (it IS the lead)
}

_signal_aggregators[ci.code] = std::move(aggregator);
}

WTSLogger::info("SignalAggregator: {} aggregators initialized "
"(ofi={:.2f}, trade={:.2f}, book={:.2f}, mom={:.2f}, lead_lag={:.2f})",
_signal_aggregators.size(),
sig_cfg.ofi_weight, sig_cfg.trade_weight,
sig_cfg.book_imbalance_weight, sig_cfg.momentum_weight,
sig_cfg.lead_lag_weight);
}
else if (_config.modules.use_market_making)
{
WTSLogger::info("SignalAggregator: skipped (use_signal_aggregator=false, using legacy architecture)");
}

//------------------------------------------------------------
// 8. MicroAlphaEngine（已移除）
// 新架构 (SignalAggregator) 已包含所有 Alpha 信号计算
// MicroAlphaEngine 不再需要，已完全移除
//------------------------------------------------------------
WTSLogger::info("MicroAlphaEngine: DISABLED (SignalAggregator handles all alpha signals)");

//------------------------------------------------------------
// 9. ToxicFlowDetector（毒性流动检测器）(仅做市)
//------------------------------------------------------------
if (_config.modules.use_market_making && _config.modules.use_toxicity_detector)
{
_toxicity_detector = FutuComponentFactory::createToxicFlowDetector(coord_cfg);

// Register with coordinator
if (_coordinator) {
_coordinator->setToxicityDetector(_toxicity_detector.get());
}

WTSLogger::info("ToxicFlowDetector: created");
}
else
{
WTSLogger::info("ToxicFlowDetector: disabled");
}

//------------------------------------------------------------
// 9.5 SelfTradeCalibrator（统一管理自身成交，供毒性检测和综合信号使用）
//------------------------------------------------------------
{
_self_trade_calibrator = FutuComponentFactory::createSelfTradeCalibrator(coord_cfg);
}

// 将校准器设置到毒性检测器（统一使用 SelfTradeCalibrator 管理 Fill 记录）
if (_toxicity_detector)
{
_toxicity_detector->setSelfTradeCalibrator(_self_trade_calibrator.get());
}

// Register with coordinator
if (_coordinator)
{
_coordinator->setSelfTradeCalibrator(_self_trade_calibrator.get());
}

{
WTSLogger::info("SelfTradeCalibrator: created");
}

//------------------------------------------------------------
// 11. PerformanceAnalyzer（绩效分析器）
//------------------------------------------------------------
if (_config.modules.use_performance_analyzer)
{
_perf_analyzer = FutuComponentFactory::createPerformanceAnalyzer(coord_cfg);
WTSLogger::info("PerformanceMonitor: latencyThreshold={}ns", _config.perf.monitor_latency_threshold);
}
else
{
WTSLogger::info("PerformanceAnalyzer: disabled");
}

//------------------------------------------------------------
// 12. PerformanceMonitor（性能监控）
//------------------------------------------------------------
if (_config.modules.use_performance_monitor)
{
_performance_monitor = FutuComponentFactory::createPerformanceMonitor(coord_cfg);
// 接线到 Coordinator — 旧代码漏接, 协调器内 recordTickToQuote 因空指针永不执行
_coordinator->setPerformanceMonitor(_performance_monitor.get());
WTSLogger::info("PerformanceMonitor: latencyThreshold={}ns", _config.perf.monitor_latency_threshold);
}
else
{
WTSLogger::info("PerformanceMonitor: disabled");
}


//------------------------------------------------------------
// 14. SpreadArbitrageManager（跨期价差套利管理器）
// 独立配置文件: 从 config 中读取 spread_arbitrage_config
//------------------------------------------------------------
if (_config.modules.use_spread_arbitrage)
{
_spread_arb_manager = std::make_unique<SpreadArbitrageManager>();

// 加载独立配置文件
std::string arb_cfg_path = _config.spread_arbitrage_config.empty() ? "spread_arbitrage.yaml" : _config.spread_arbitrage_config;
if (_spread_arb_manager->loadConfig(arb_cfg_path))
{
WTSLogger::info("SpreadArbitrageManager: loaded config from {}", arb_cfg_path);
}
else
{
// 加载失败，使用默认配置
SpreadArbitrageConfig arb_cfg;
arb_cfg.enabled = true;
arb_cfg.enhance_market_making = true;
arb_cfg.max_total_position = 20.0;
_spread_arb_manager->setConfig(arb_cfg);

WTSLogger::warn("SpreadArbitrageManager: using default config (file load failed from {})", arb_cfg_path);
}

// Scheme B-3: inject Portfolio SSOT for portfolio-derived spread monitoring.
// Must be set before any generateSignal call. Portfolio outlives SpreadArbMgr.
_spread_arb_manager->setPortfolio(_portfolio.get());
_spread_arb_manager->setInFlightTimeoutMs(120000ULL);  // 120 seconds
WTSLogger::info("SpreadArbitrageManager: B-3 gate enabled (Portfolio SSOT, in_flight_timeout={}ms)",
    120000ULL);
}
else
{
WTSLogger::info("SpreadArbitrageManager: disabled");
}    
//------------------------------------------------------------
// 15. SelfTradePrevention（自成交防护模块）
//------------------------------------------------------------
if (_config.modules.use_spread_arbitrage)
{
_stp = FutuComponentFactory::createSelfTradePrevention(coord_cfg, _order_tracker.get());

// H7: 统一 min_price_gap — config.yaml 的 stpMinPriceGap 是权威来源
// (coordinator.yaml 的 minPriceGap 被 override, 避免双路径不一致)
{
    auto stp_cfg = _stp->getConfig();
    if (std::abs(stp_cfg.min_price_gap - _config.order_control.stp_min_price_gap) > 1e-9) {
        WTSLogger::warn("STP min_price_gap synced to config.yaml stpMinPriceGap={:.1f} (coordinator.yaml minPriceGap overridden)",
            _config.order_control.stp_min_price_gap);
    }
    stp_cfg.min_price_gap = _config.order_control.stp_min_price_gap;
    _stp->setConfig(stp_cfg);
}

WTSLogger::info("SelfTradePrevention: enabled, strategy=CANCEL_MM, using UnifiedOrderTracker");
}

//------------------------------------------------------------
// 16. AsyncArbitrageExecutor（异步套利执行器）
//------------------------------------------------------------
if (_config.modules.use_spread_arbitrage)
{
_async_arb = FutuComponentFactory::createAsyncArbitrageExecutor(coord_cfg);
_async_arb->setArbitrageManager(_spread_arb_manager.get());
_async_arb->setSelfTradePrevention(_stp.get());
// A10: 接线利润门槛 (此前 setMinProfitThreshold 无调用者, 价格调整成本检查恒放行)
_async_arb->setMinProfitThreshold(_spread_arb_manager->getConfig().min_profit_threshold_ticks);
if (_coordinator) {
_coordinator->setArbExecutor(_async_arb.get());
_coordinator->setArbManager(_spread_arb_manager.get());  // B2: 平仓 intent 查询通道
}
// B5: 过冲保险丝 — Portfolio sign-flip → arb_manager->onOvershootDetected
if (_portfolio) {
_portfolio->setArbManager(_spread_arb_manager.get());
}

// 设置每个合约的 tick size（用于套利订单价格调整）
for (const auto& ci : _contract_infos)
{
if (ci.tick_size > 0)
{
_async_arb->updateTickSize(ci.code, ci.tick_size);
}
}

WTSLogger::info("AsyncArbitrageExecutor: enabled, signalInterval=5000us, ticksPerSignal=5");

// ArbExecutionBridge 依赖注入 (架构重构 C4)
{
    ArbExecutionBridge::Deps bridge_deps;
    bridge_deps.async_arb = _async_arb.get();
    bridge_deps.arb_manager = _spread_arb_manager.get();
    bridge_deps.order_router = _order_router.get();
    bridge_deps.order_tracker = _order_tracker.get();
    bridge_deps.stp = _stp.get();
    bridge_deps.portfolio = _portfolio.get();
    bridge_deps.risk_monitor = _risk_monitor.get();
    bridge_deps.contract_infos = &_contract_infos;
    bridge_deps.use_spread_arbitrage = _config.modules.use_spread_arbitrage;
    bridge_deps.strategy_id = id();
    _arb_bridge.setDeps(bridge_deps);
}
}
else
{
WTSLogger::info("AsyncArbitrageExecutor: disabled");
}

// 共享TradingState（必须在arb if/else块之外，否则Arb=OFF时指针为nullptr导致segfault）
if (_coordinator) {
    _coordinator->setTradingState(&_trading_state);
}

// R1: 如果 WtUftRunner 在 init 之前已注入 _event_notifier,
// 此时所有下游(_risk_monitor / _spread_arb_manager)均已创建, 完成注入
if (_event_notifier) {
    setEventNotifier(_event_notifier);
}

//------------------------------------------------------------
// 初始化计数器
//------------------------------------------------------------
_tick_count = 0;
// _param_update_interval 从配置读取

// 从配置更新下单错误处理参数
// order_error_threshold used directly from _config.order_control
}

//==========================================================================
// UFT 策略回调
//==========================================================================

void UftFutuMmStrategy::on_init(IUftStraCtx* ctx)
{
// 保存ctx指针
_main_ctx = ctx;
_is_backtest = _config.is_backtest;
WTSLogger::info("UftFutuMmStrategy[{}] mode: {} (isBacktest={})", id(), _is_backtest ? "BACKTEST" : "LIVE", _is_backtest);

// R1: 从 ctx 获取 EventNotifier (WtUftRunner 创建 UftStraContext 时已注入 &_notifier)
auto* uft_ctx = dynamic_cast<UftStraContext*>(ctx);
if (uft_ctx && uft_ctx->getEventNotifier())
{
    setEventNotifier(uft_ctx->getEventNotifier());
    WTSLogger::info("UftFutuMmStrategy[{}] EventNotifier injected via ctx", id());
}

// 默认收盘时间
_config.closeout.close_time = 150000;

// 从基础数据管理模块获取合约参数（如果配置文件未指定）
for (auto& ci : _contract_infos)
{
std::string stdCode = fullCodeToStdCode(ci.code);
WTSCommodityInfo* commInfo = ctx->stra_get_comminfo(stdCode.c_str());

if (commInfo)
{
_session_cache[ci.code] = {commInfo, commInfo->getSessionInfo()};
WTSLogger::debug("UftFutuMmStrategy[{}] Session cache added: {} -> sessInfo={}", 
id(), ci.code, (void*)commInfo->getSessionInfo());

if (ci.multiplier <= 0)
ci.multiplier = commInfo->getVolScale();
if (ci.tick_size <= 0)
ci.tick_size = commInfo->getPriceTick();

if (commInfo->getSessionInfo())
{
const auto& sections = commInfo->getSessionInfo()->getTradingSections();
uint32_t dayCloseTime = 150000;
uint32_t nightCloseTime = 0;  // 0 = 无夜盘

for (const auto& section : sections)
{
uint32_t startTime = section.first_raw;
uint32_t endTime = section.second_raw;

// 白盘收盘: endTime > 600 且 <= 2359 (如 1130, 1500, 1515)
if (endTime > 600 && endTime <= 2359)
{
dayCloseTime = endTime * 100;  // 转为HHMMSS格式
}

// 夜盘收盘: endTime <= 600 (凌晨，如 100=01:00, 230=02:30)
//           或 startTime >= 2100 且 endTime <= 2359 (不跨日，如 2300, 2330)
if (endTime <= 600 && endTime > 0)
{
// 跨日品种: 夜盘收盘在凌晨 (01:00, 02:30)
nightCloseTime = endTime;  // 保持HHMM格式 (如 230)
}
else if (startTime >= 2100 && endTime <= 2359 && endTime > 600)
{
// 不跨日品种: 夜盘收盘在当晚 (23:00, 23:30)
nightCloseTime = endTime;  // 保持HHMM格式 (如 2300)
}
}

ci.close_time = dayCloseTime;
ci.night_close_time = nightCloseTime;
}
else
{
ci.close_time = 150000;
}

if (ci.code == _config.anchor_code)
{
_config.closeout.close_time = ci.close_time;
_config.closeout.night_close_time = ci.night_close_time;
}

WTSLogger::info("UftFutuMmStrategy[{}] contract {} from base: multiplier={}, tickSize={}, closeTime={}, nightCloseTime={}",
id(), ci.code, ci.multiplier, ci.tick_size, ci.close_time, ci.night_close_time);
}
else
{
if (ci.multiplier <= 0) ci.multiplier = 1.0;
if (ci.tick_size <= 0) ci.tick_size = 0.2;
ci.close_time = 150000;
WTSLogger::warn("UftFutuMmStrategy[{}] contract {} not found in base data, using defaults",
id(), ci.code);
}
}

// 初始化业务模块（需要合约参数 + ctx 用于 BilateralStats Per-Quoter sessInfo 注入）
initBusinessModules(ctx);

//============================================================
// 配置校验（在 initBusinessModules 之后，所有模块参数已加载）
//============================================================
{
FutuConfigValidator::ValidationResult vr;

// 信号权重校验
if (!_signal_aggregators.empty()) {
const auto& sig_cfg = _signal_aggregators.begin()->second->getConfig();
FutuConfigValidator::validateSignalWeights(
sig_cfg.ofi_weight, sig_cfg.trade_weight,
sig_cfg.book_imbalance_weight, sig_cfg.momentum_weight,
sig_cfg.lead_lag_weight, vr);
}

// GLFT 参数范围校验
if (!_spread_optimizers.empty()) {
const auto& glft = _spread_optimizers.begin()->second->getParams();
FutuConfigValidator::checkRange("base_spread", glft.base_spread, 0.5, 20.0, vr);
FutuConfigValidator::checkRange("phi", glft.phi, 0.01, 2.0, vr);
FutuConfigValidator::checkPositive("tick_size", glft.tick_size, vr);
FutuConfigValidator::checkRange("delta_skew_threshold", glft.delta_skew_threshold, 0.0, 0.9, vr);
}

// Portfolio 参数校验
FutuConfigValidator::checkPositive("portfolio_max_delta", _portfolio->getParams().portfolio_max_delta, vr);

// 输出结果
for (const auto& err : vr.errors) {
WTSLogger::error("UftFutuMmStrategy[{}] Config validation ERROR: {}", id(), err);
}
for (const auto& warn : vr.warnings) {
WTSLogger::warn("UftFutuMmStrategy[{}] Config validation WARNING: {}", id(), warn);
}
if (vr.valid) {
WTSLogger::info("UftFutuMmStrategy[{}] Config validation passed (0 errors, {} warnings)",
id(), vr.warningCount());
} else {
WTSLogger::error("UftFutuMmStrategy[{}] Config validation FAILED ({} errors, {} warnings) — strategy may misbehave!",
id(), vr.errorCount(), vr.warningCount());
}
}

//============================================================
// 注册热更新参数（运行时可修改，无需重启策略）
// 仅包含直接影响报价价格计算的参数
// 仓位管理/风控/对冲等参数需重启生效
// 注意：必须在 initBusinessModules() 之后注册，
//       以便从已初始化的模块读取实际参数值作为默认值
//============================================================

const auto& coord_mp = _coordinator->getConfig().modules;

// 从第一个 SpreadOptimizer 读取 GLFTParams 作为默认值
GLFTParams glft_defaults;
if (!_spread_optimizers.empty()) {
auto it = _spread_optimizers.begin();
if (it->second) glft_defaults = it->second->getParams();
}

// 从第一个 SignalAggregator 读取权重作为默认值
SignalAggregatorConfig sig_defaults;
if (!_signal_aggregators.empty()) {
auto it = _signal_aggregators.begin();
if (it->second) sig_defaults = it->second->getConfig();
}

// 热参数注册已拆分至 FutuHotParamManager (架构重构 C2)
_hot_mgr.registerParams(ctx, _config, glft_defaults, sig_defaults, coord_mp.alpha_sensitivity);

WTSLogger::info("UftFutuMmStrategy[{}] hot-update params registered (defaults from coordinator.yaml)", id());

// 输出初始化日志
WTSLogger::info("UftFutuMmStrategy[{}] initialized: {} contracts, {} levels",
id(), _contract_infos.size(), _config.quoting.num_levels);
WTSLogger::info("MaxDelta={} (soft)",
_config.portfolio.max_delta);
WTSLogger::info("Modules: spreadOpt={}, toxicity={}",
_config.modules.use_spread_optimizer, _config.modules.use_toxicity_detector);

// 设置跨期套利信号回调
if (_spread_arb_manager)
{
_spread_arb_manager->setSignalCallback(
[this](const SpreadSignal& signal) {
// 信号回调只记录，实际执行在 processSpreadArbitrage 中
WTSLogger::info("SpreadArb signal: pair={}, type={}, confidence={}",
signal.pair_id, (int)signal.type, signal.confidence);
}
);
}

// 订阅所有合约行情（使用 fullCode，与行情推送格式一致）
for (const auto& ci : _contract_infos)
{
ctx->stra_sub_ticks(ci.code.c_str());
WTSLogger::info("UftFutuMmStrategy[{}] subscribed: {}", id(), ci.code);
}
}

void UftFutuMmStrategy::on_session_begin(IUftStraCtx* ctx, uint32_t uTDate)
{
// 重置日内状态
_risk_monitor->resetDaily();
// 重置组合日内 PnL — 旧代码 realized_pnl 跨日累计, 导致"日亏损"风控
// 用昨日亏损误判今日 IRREVERSIBLE halt.
_portfolio->resetDailyPnl();
// force=true —— 新交易日强制清 closeout state,
// 否则上一日卡 FLATTENING 时 resetCloseout 会被状态机拒绝,导致 state 永久死锁
_risk_monitor->resetCloseout(true);  // 重置收盘前平仓状态(强制)

// P1-1: reset() clears phase+qphase+blocks for new session
_trading_state.reset();

// 跨日重置下单错误状态机, 避免昨日累计错误带到今日.
_order_error_count = 0;
_quoting_paused_since = 0;

// reset closeout hedge guard so new day can fire hedge if needed
// (closeout 守卫/执行器复位已收编到 CloseoutOrchestrator, 架构重构 C3)
// (套利桥状态复位已收编到 ArbExecutionBridge, 架构重构 C4)
_arb_bridge.resetSession();
_closeout_orch.resetSession();

// 重置本地状态
_blocked_contracts.clear();

// 启动异步套利执行器
// useAsyncArbThread=true(实盘默认): 启动独立 arb 线程, pushTick 走 SPSC 队列 (~50ns)
// useAsyncArbThread=false(回测): 不启动线程, pushTick 主线程同步执行
// 跨线程安全已修复: computeDerivedSpread 改读 _pair_states(spin保护),
// canOpenPosition/getQuotingAdjustment 加锁, _min_profit_threshold 原子化.
if (_async_arb)
{
    if (_config.modules.use_async_arb_thread)
    {
        _async_arb->start();
        WTSLogger::info("AsyncArbitrageExecutor: async mode (arb thread started)");
    }
    else
    {
        WTSLogger::info("AsyncArbitrageExecutor: sync mode (arb thread disabled by config)");
    }
}

// 清空自成交防护模块
if (_stp)
{
_stp->clear();
}

// R3 v2: BilateralStats 已 Per-Quoter 化,session start 在每个 quoter 上独立触发
{
    uint32_t uTime_HHMM = ctx->stra_get_time();
    for (auto& [code, quoter] : _quoters)
    {
        auto& stats = quoter->getBilateralStats();
        if (stats.hasSessionInfo())
            stats.onSessionStart(uTime_HHMM);
    }
}

WTSLogger::info("UftFutuMmStrategy[{}] session begin: {}", id(), uTDate);
}

void UftFutuMmStrategy::on_session_end(IUftStraCtx* ctx, uint32_t uTDate)
{
    // P1-1: enter CLOSEOUT phase on session end
    _trading_state.enterCloseout();

// 停止异步套利执行器
if (_async_arb)
{
_async_arb->stop();
}

// 撤销所有委托
for (auto& [code, quoter] : _quoters)
{
quoter->cancelAll(ctx);
}

// 清空自成交防护模块
if (_stp)
{
_stp->clear();
}

// R3 v2: BilateralStats Per-Quoter,逐合约 onSessionEnd + formatString 输出
{
    uint32_t uTime_HHMM = ctx->stra_get_time();
    uint32_t sec_in_min = ctx->stra_get_secs();
    for (auto& [code, quoter] : _quoters)
    {
        auto& stats = quoter->getBilateralStats();
        if (!stats.hasSessionInfo()) continue;
        stats.onSessionEnd(uTime_HHMM, sec_in_min);
        WTSLogger::info("[BILATERAL_STATS] {} | {}", code, stats.formatString());
    }
}
    
    // 绩效分析报告
    if (_perf_analyzer)
    {
        auto metrics = _perf_analyzer->getMetrics();
        WTSLogger::info("[PERF] session={} | pnl={:.2f}(unreal={:.2f}) | vol={} trades={} | "
            "spread_captured={:.4f} capture_rate={:.2f}% | fill_rate={:.2f}% | "
            "max_dd={:.2f} sharpe={:.2f} win={:.2f}% | adverse={:.4f} real_adv/vol={:.4f} tox_events={} | "
            "alpha_acc={:.2f}% alpha_pnl={:.2f} | avg_inv={:.1f} turnover={:.2f}",
            uTDate,
            metrics.total_pnl, metrics.unrealized_pnl,
            metrics.total_volume, metrics.total_trades,
            metrics.avg_spread_captured, metrics.spread_capture_rate * 100,
            metrics.fill_rate * 100,
            metrics.max_drawdown, metrics.sharpe_ratio, metrics.win_rate * 100,
            metrics.adverse_ratio, metrics.real_adverse_per_vol, metrics.toxicity_events,
            metrics.alpha_accuracy * 100, metrics.alpha_pnl_per_trade,
            metrics.avg_inventory, metrics.inventory_turnover);
    }
    
    WTSLogger::info("UftFutuMmStrategy[{}] session end: {}, Delta: {}", 
        id(), uTDate, _portfolio->getTotalDelta());

    // session_end closeout 状态强制收尾 (已拆分至 CloseoutOrchestrator, 架构重构 C3)
    _closeout_orch.finalizeAtSessionEnd(_exchange_time_ms > 0 ? _exchange_time_ms : TimeUtils::getLocalTimeNow());
}

//============================================================
// on_tick 子函数 (P2-1a: 从 on_tick 拆出)
//============================================================

void UftFutuMmStrategy::handleQuotingAutoResume()
{
    if (_trading_state.qphase != QuotingPhase::ERROR || _quoting_paused_since == 0)
        return;

    uint64_t paused_ms = (_exchange_time_ms > 0 ? _exchange_time_ms : TimeUtils::getLocalTimeNow()) - _quoting_paused_since;
    uint64_t wait_threshold = 10000;  // 初始等待 10 秒
    if (_order_error_count > 0)
    {
        uint32_t shift = (_order_error_count < 5) ? _order_error_count : 5;
        uint64_t exp_wait = 10000ULL << shift;
        wait_threshold = (exp_wait > 60000ULL) ? 60000ULL : exp_wait;
    }

    if (paused_ms > wait_threshold)
    {
        // 试探性恢复: count 不衰减保留真实历史.
        // 退避到期后无条件试探翻 NORMAL, 让做市试发新单. 若再次失败 on_entrust
        // 失败回调会立即把 qphase 翻回 ERROR (count 已大, 走硬触发硬撤路径).
        if (_trading_state.tryResumeFrom(QuotingPhase::ERROR))
        {
            _quoting_paused_since = 0;
            WTSLogger::info("UftFutuMmStrategy[{}] Quoting auto-resumed after {}ms (count={}, probing)",
                id(), paused_ms, _order_error_count);
        }
    }
}

void UftFutuMmStrategy::handleMarketDataUpdate(const char* stdCode, WTSTickData* tick, double mid)
{
    if (mid > 0)
    {
        _portfolio->markToMarket(stdCode, mid);
        _last_mid[stdCode] = mid;

        if (_price_stale)
        {
            _price_stale = false;
            WTSLogger::info("UftFutuMmStrategy[{}] Price recovered (first tick after channel ready)", id());
        }
    }

    if (_correlation_manager) {
        _correlation_manager->onTick(tick);

        if (_portfolio) {
            const std::string& anchor = _config.anchor_code;
            if (stdCode != anchor) {
                double beta = _correlation_manager->getHedgeRatio(stdCode, anchor);
                auto stats = _correlation_manager->getCorrelation(stdCode, anchor);

                if (auto* cs = _portfolio->getContract(stdCode)) {
                    if (!cs->hedge_ratio_initialized && cs->last_price > 0 && std::isfinite(beta) && beta > 0) {
                        cs->hedge_ratio = beta;
                        cs->hedge_ratio_initialized = true;
                    } else {
                        bool should_update = (stats.sample_count >= 100);

                        if (should_update && cs->hedge_ratio > 0) {
                            double change_ratio = std::abs(beta - cs->hedge_ratio) / cs->hedge_ratio;
                            if (change_ratio > 0.2) {
                                beta = cs->hedge_ratio * (1.0 + (beta > cs->hedge_ratio ? 0.2 : -0.2));
                            }
                        }

                        if (should_update && std::isfinite(beta) && beta > 0) {
                            cs->hedge_ratio = beta;
                        }
                    }
                }
            }
        }
    }
}

void UftFutuMmStrategy::handleLeadLagPush(const char* stdCode, WTSTickData* tick, double mid)
{
    if (_config.anchor_code.empty() || mid <= 0)
        return;

    // LeadLag 调试: 确认 push 是否到达
    static uint64_t ll_dbg_counter = 0;
    if (ll_dbg_counter++ % 10000 == 0) {
        WTSLogger::debug("[LEADLAG_DBG] stdCode={} anchor={} mid={:.2f} aggregators={}",
            stdCode, _config.anchor_code, mid, _signal_aggregators.size());
    }

    if (stdCode != _config.anchor_code)
        return;

    uint64_t ts = tick->volume() > 0
        ? static_cast<uint64_t>(tick->actiondate()) * 1000000000ULL
        + static_cast<uint64_t>(tick->actiontime())
        : 0;

    for (auto& [code, aggregator] : _signal_aggregators)
    {
        if (code != _config.anchor_code && aggregator)
        {
            aggregator->updateLeadContract(_config.anchor_code, mid, ts);
        }
    }
}

void UftFutuMmStrategy::handleCoordinatorTick(IUftStraCtx* ctx, const char* stdCode, WTSTickData* tick, uint64_t now_ms)
{
    if (!_coordinator || _price_stale)
    {
        // Fallback: no coordinator (or stale price), trigger fail-safe
        if (!_coordinator) {
            WTSLogger::error("UftFutuMmStrategy[{}] Coordinator is null, triggering FAIL-SAFE!", id());
            _trading_state.setQuotingPhase(QuotingPhase::RISK_HALTED);
            for (auto& [code, quoter] : _quoters)
                quoter->cancelAll(ctx);
        }
        return;
    }

    auto result = _coordinator->processTick(ctx, stdCode, tick, now_ms);

    // 记录报价到绩效分析器
    if (result.quote_placed && _perf_analyzer)
    {
        double qmid = 0;
        auto mid_it = _last_mid.find(stdCode);
        if (mid_it != _last_mid.end()) qmid = mid_it->second;
        _perf_analyzer->recordQuote(stdCode, qmid, qmid, 0, 0,
            ctx->stra_get_date() * 1000000ULL + ctx->stra_get_time() * 100ULL + ctx->stra_get_secs());
    }

    // Run synthetic signal fusion cycle
    if (_toxicity_detector && _toxicity_detector->isFusionEnabled())
    {
        _toxicity_detector->runFusionCycle();
    }

    // === Closeout 驱动 (已拆分至 CloseoutOrchestrator, 架构重构 C3) ===
    _closeout_orch.onTick(ctx, tick, result.closeout_executed);
}

//============================================================
// on_tick 主控 (P2-1a: 从 285 行 → ~35 行)
//============================================================

void UftFutuMmStrategy::on_tick(IUftStraCtx* ctx, const char* stdCode, WTSTickData* tick)
{
    if (!_channel_ready || !tick)
        return;

    // 1. 报价暂停恢复 (ERROR → NORMAL with exponential backoff)
    handleQuotingAutoResume();

    // 2. v7.1 主时钟: 全策略决策逻辑统一用 replay 时间 (tick actiondate/actiontime)。
    //   回测中墙钟节流(delta-rate滑窗/毒性cooloff/限速/恢复冷却)随机器速度漂移,
    //   导致订单序列不可复现 (同配置两次运行 PnL 差数倍)。
    //   实盘时 replay 时间=交易所时间, 语义一致。
    {
        uint32_t ad = tick->actiondate();
        uint32_t at = tick->actiontime();
        _exchange_time_ms = static_cast<uint64_t>(ad) * 86400000ULL
                   + static_cast<uint64_t>(at / 10000000) * 3600000ULL
                   + static_cast<uint64_t>((at / 100000) % 100) * 60000ULL
                   + static_cast<uint64_t>((at / 1000) % 100) * 1000ULL
                   + (at % 1000);
        if (_coordinator)
            _coordinator->setExchangeTime(_exchange_time_ms);
        if (_order_router)
            _order_router->setNowMs(_exchange_time_ms);
        if (_spread_arb_manager)
            _spread_arb_manager->setNowMs(_exchange_time_ms);
    }
    uint64_t now_ms = _exchange_time_ms;
    if (_risk_monitor)
        _risk_monitor->setCurrentTime(now_ms);

    // 3. 行情数据更新 (markToMarket + correlation + hedge_ratio)
    double mid = (tick->bidprice(0) + tick->askprice(0)) / 2.0;
    handleMarketDataUpdate(stdCode, tick, mid);
    
    // 3.5 PerformanceAnalyzer tick 更新 (真实 adverse selection 追踪)
    if (_perf_analyzer)
        _perf_analyzer->onTickUpdate(stdCode, mid, tick->actiontime());

    // 4. LeadLag 跨合约推送
    handleLeadLagPush(stdCode, tick, mid);

    // 5. Coordinator 主处理 + closeout 驱动 (含 coordinator null fail-safe)
    handleCoordinatorTick(ctx, stdCode, tick, now_ms);

    // 6. 跨期价差套利 (与做市业务平级，独立处理 — 已拆分至 ArbExecutionBridge, C4)
    // v7.1: CLOSEOUT 阶段不喂 arb tick — 收盘平仓窗口内开新价差仓会让
    //       closeout 前功尽弃 (回测实证: 14:45 平仓后 arb 重建 ~50 手 delta 过夜,
    //       次日跳空触发日亏 IRREVERSIBLE halt)。夜盘 closeout 由
    //       exitToQuoting 恢复 phase 后自动恢复喂入。
    // v7.1: session 休息段同样不喂 arb (与报价暂停一致).
    if (_spread_arb_manager && _config.modules.use_spread_arbitrage
        && _trading_state.phase != MmPhase::CLOSEOUT
        && !(_coordinator && _coordinator->isSectionBreakActive()))
    {
        _arb_bridge.onTick(ctx, stdCode, tick);
    }

    _tick_count++;
}


void UftFutuMmStrategy::on_order_queue(IUftStraCtx* ctx, const char* stdCode, WTSOrdQueData* newOrdQue)
{
// Level2: 更新 MarketDataContext
auto it = _market_data.find(stdCode);
if (it != _market_data.end())
it->second->onOrderQueue(newOrdQue);
}

void UftFutuMmStrategy::on_order_detail(IUftStraCtx* ctx, const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
// Level2: 更新 MarketDataContext
auto it = _market_data.find(stdCode);
if (it != _market_data.end())
it->second->onOrderDetail(newOrdDtl);
}

void UftFutuMmStrategy::on_transaction(IUftStraCtx* ctx, const char* stdCode, WTSTransData* newTrans)
{
if (!newTrans) return;

const auto& trans = newTrans->getTransStruct();
double qty = static_cast<double>(trans.volume);
double price = trans.price;
uint64_t timestamp = trans.action_time;

// 根据 price vs mid 判断方向
auto it = _last_mid.find(stdCode);
bool isBuy = (it != _last_mid.end()) ? (price >= it->second) : true;

// 更新 SpreadOptimizer 的成交数据 (P0-2.1: Legacy onFill removed)
if (_config.modules.use_spread_optimizer)
{
// NO-OP: Fill stats should ideally be aggregated in SignalContext
}

if (_correlation_manager)
{
_correlation_manager->onTick(stdCode, price, timestamp);
}

// 移除不安全的直连调用：_spread_arb_manager->onTick(stdCode, price, 1.0, timestamp);
// 因为套利主计算已由 _async_arb 在子线程专门负责，主线程的 on_transaction 强行调用会导致致命的 Map 并发读写冲突 (Core Dump)

// 更新 MarketDataContext (不可或缺核心组件)
auto md_it = _market_data.find(stdCode);
if (md_it != _market_data.end())
md_it->second->onTransaction(newTrans);

// 更新 PerformanceMonitor
if (_performance_monitor)
{
_performance_monitor->recordFillReceived();
}
}

void UftFutuMmStrategy::on_trade(IUftStraCtx* ctx, uint32_t localid, const char* stdCode,
bool isLong, uint32_t offset, double vol, double price)
{
// 更新频率统计
if (_risk_monitor)
_risk_monitor->recordTrade();

// 使用策略本地持仓（而不是账户持仓）
// 注意：一个账户可能有多个策略，每个策略只管理自己的持仓
// stra_get_local_position 返回的是本策略的净头寸
double local_net = ctx->stra_get_local_position(stdCode);

// P0-1/v7.1: 分向成本簿记账 (offset 标志驱动, 替代净额推断)
// 旧净额推断在 MM+arb 共享同合约净头寸时必然失真:
//   arb 腿的开平被误判为 MM 加减仓, avg_cost 被污染 → daily_pnl 假阳性日亏
//   (回测实证: 引擎真账 +34k, 内部账 -3.7M → 日亏 IRREVERSIBLE halt 误杀)
{
    _portfolio->onTradeFill(stdCode, isLong, static_cast<int>(offset), vol, price);
    _portfolio_ctx_dirty = true;

    // 净持仓以引擎真值校验 (防漏单/异常路径漂移)
    const ContractState* cs_chk = _portfolio->getContract(stdCode);
    if (cs_chk && std::abs(cs_chk->position - local_net) > 0.01)
    {
        WTSLogger::warn("UftFutuMmStrategy[{}] position book divergence: {} book={:.0f} engine={:.0f}, resyncing",
            id(), stdCode, cs_chk->position, local_net);
        _portfolio->resyncPosition(stdCode, local_net);
    }
}

// 套利单成交处理 (in_flight 递减 + 残腿对冲, 已拆分至 ArbExecutionBridge, C4)
_arb_bridge.onTradeFill(ctx, localid, stdCode, isLong, vol, price);

// 更新 Quoter 订单状态
// R3 v2: onTrade 补时间参数(UFT: stra_get_time=HHMM, stra_get_secs=SSmmm)
{
    uint32_t uTime_HHMM = ctx->stra_get_time();
    uint32_t sec_in_min = ctx->stra_get_secs() / 1000;
    for (auto& [code, quoter] : _quoters)
    {
        if (quoter->isMyOrder(localid))
        {
            quoter->onTrade(localid, vol, price, uTime_HHMM, sec_in_min);
            break;
        }
    }
}

// v7.1: 单边成交侵蚀挂单深度 → 不满足双边做市义务时, 立即恢复
// 回测: 只撤 MM 单(stra_cancel postTask 安全), 重挂由 processQuoting 同 tick 完成
// 生产: 同步 requoteAfterFill (stra_buy/sell 发往交易所, 无 _orders 迭代问题)
if (_coordinator)
{
    if (_is_backtest)
    {
        auto qit = _quoters.find(stdCode);
        if (qit != _quoters.end())
            qit->second->cancelAll(ctx);
    }
    else
    {
        _coordinator->requoteAfterFill(ctx, stdCode, _exchange_time_ms);
    }
}

// 更新 SpreadOptimizer 成交统计 (P0-2.1: Legacy onFill removed)
if (_config.modules.use_spread_optimizer)
{
// NO-OP
}

// 从共享订单跟踪器中移除 — 仅完全成交时才 untrack。
// 旧代码无条件 untrack: 部分成交后残留活单从真相源消失,
// 导致自成交检查绕过/在途量低估/sticky 失效.
if (_order_tracker && _order_tracker->recordOrderFill(localid, vol))
_order_tracker->untrackOrder(localid);
// 记录到绩效分析器
if (_perf_analyzer)
{
TradeRecord trade;
trade.code = stdCode;
trade.is_buy = isLong;
trade.qty = vol;
trade.price = price;
trade.timestamp = ctx->stra_get_date() * 1000000ULL + ctx->stra_get_time() * 100ULL + ctx->stra_get_secs();
auto mid_it = _last_mid.find(stdCode);
if (mid_it != _last_mid.end())
{
trade.mid_at_trade = mid_it->second;
// 计算价差和穿越状态
const ContractState* cs = _portfolio->getContract(stdCode);
trade.spread_at_trade = cs ? cs->tick_size * 2.0 : 0.2;  // 与SelfTradeCalibrator一致
// 判断是否穿越价差：买单成交价>=mid 或 卖单成交价<=mid 表示主动穿越
double mid = mid_it->second;
trade.is_crossing = isLong ? (price >= mid) : (price <= mid);
}
// 记录成交时的 alpha 信号和波动率（用于 alpha 绩效追踪）
auto sig_it = _signal_aggregators.find(stdCode);
if (sig_it != _signal_aggregators.end() && sig_it->second)
{
    const SignalContext& sc = sig_it->second->getContext();
    if (sc.alpha.valid)
        trade.alpha_at_trade = sc.alpha.alpha;
    if (sc.volatility.valid)
        trade.volatility = sc.volatility.realized_vol;
}
_perf_analyzer->recordTrade(trade);
_perf_analyzer->updatePosition(stdCode, _portfolio->getPosition(stdCode), 0);
}

// 记录到自身成交校准器 (统一管理成交记录，供毒性检测使用)
if (_self_trade_calibrator)
{
// 统一 epoch ms — 旧代码用 date*1e6+time*100+secs 压缩时间(~2e13),
// 而 onTick/getFillRetreat/decayCalibration 均传 epoch ms(~1.75e12),
// uint64 下溢导致 FillRetreat/prune/decay 全部失效.
uint64_t timestamp = static_cast<uint64_t>(TimeUtils::getLocalTimeNow());
double mid_at_fill = price;
double spread_at_fill = 0.2;  // default spread
auto mid_it = _last_mid.find(stdCode);
if (mid_it != _last_mid.end()) {
mid_at_fill = mid_it->second;
}

// 计算当前价差
const ContractState* cs = _portfolio->getContract(stdCode);
if (cs) spread_at_fill = cs->tick_size * 2;

_self_trade_calibrator->recordFill(
stdCode, price, vol, isLong,
mid_at_fill, spread_at_fill, timestamp
);

// 将校准结果传递给毒性检测器
if (_config.modules.use_toxicity_detector && _toxicity_detector)
{
auto calibration = _self_trade_calibrator->getCalibration(stdCode);
_toxicity_detector->onSelfTradeCalibration(calibration);
}
}

    // 改进日志格式：显示开平方向，更容易理解
    // isLong + OPEN -> 开多, isLong + CLOSE -> 平多
    // !isLong + OPEN -> 开空, !isLong + CLOSE -> 平空
    // 注意: TraderAdapter::on_trade 将 WOT 枚举转换为数值: 0=OPEN, 1=CLOSE, 2=CLOSETODAY
    // 不能用 offset == '0' (ASCII 48)，因为传入的是数值0而非字符'0'
    bool isOpen = (offset == 0);  // 数值0 = WOT_OPEN
    const char* actionStr = "";
    if (isLong) {
        actionStr = isOpen ? "OPEN_LONG" : "CLOSE_LONG";
    } else {
        actionStr = isOpen ? "OPEN_SHORT" : "CLOSE_SHORT";
    }
    
    // 基于position变化判断实际效果
    // CTP的isLong+offset组合可能不反映实际持仓变化
    // 例如：ag2612有多仓24手时，OPEN_SHORT实际是平多（position减少）
    // 需要同时显示CTP方向和实际效果，避免日志误导
    const char* effectStr = "";
    if (auto* cs = _portfolio->getContract(stdCode)) {
        // position变化：正=增加多头/减少空头，负=减少多头/增加空头
        double pos_change = cs->position - cs->prev_position;  // 需要记录prev_position
        if (pos_change > 0) {
            effectStr = "(+long)";
        } else if (pos_change < 0) {
            effectStr = "(+short)";
        } else {
            effectStr = "(flat)";
        }
    }
    
    WTSLogger::info("UftFutuMmStrategy[{}] TRADE: {} {} {}@{} | Delta: {} {}",
        id(), stdCode, actionStr, vol, price, _portfolio->getTotalDelta(), effectStr);

    // P1-3: 成交后重置报单错误计数(成功成交 = 连续错误中断)
    if (_order_error_count > 0) {
        _order_error_count = 0;
    }

// ============================================================
// 成交后检查风控状态，如果没有硬指标违规则恢复交易
// ============================================================
if (_trading_state.qphase == QuotingPhase::RISK_HALTED && _risk_monitor)
{
_risk_monitor->checkRiskLimits(_portfolio.get(), _violations_buf);
auto& violations = _violations_buf;
bool hasHardBreach = false;
for (const auto& v : violations)
{
if (v.type == RiskLimitType::POSITION_NET || 
v.type == RiskLimitType::EXPOSURE ||
v.type == RiskLimitType::DAILY_LOSS)
{
hasHardBreach = true;
WTSLogger::debug("UftFutuMmStrategy[{}] Still has hard breach: {} {}", 
id(), v.code, (int)v.type);
break;
}
}

if (!hasHardBreach && _risk_monitor->getHaltCategory() != RiskCategory::IRREVERSIBLE)
{
// do not auto-resume while closeout flattening is in progress.
// closeout halt must persist until on_session_begin restores it.
if (_risk_monitor->isCloseoutFlattening() ||
    _risk_monitor->isCloseoutTriggered() ||
    _trading_state.phase == MmPhase::CLOSEOUT)
{
    // skip resume — closeout in progress
}
else
{
// Call resumeTrading() first and check return value.
// Only update TradingState if resumeTrading succeeds (not IRREVERSIBLE).
bool resumed = _risk_monitor->resumeTrading();
if (resumed)
{
// Use resumeFromRisk() instead of direct assignments
_trading_state.resumeFromRisk();
_trading_state.unblockLong();
_trading_state.unblockShort();
_blocked_contracts.clear();
_risk_monitor->resumeQuoting();
_risk_monitor->unblockLong();
_risk_monitor->unblockShort();
// A1: 同步重置协调器软风控倍数 — 本路径绕过 coordinator 的 checkRisk 自动恢复,
// 否则 _risk_spread_mult 残留, 恢复后报价宽度被永久放大.
if (_coordinator) _coordinator->onExternalResumeFromRisk();
WTSLogger::info("UftFutuMmStrategy[{}] Trading resumed after trade (risk check passed)", id());
}
else
{
// resumeTrading refused (IRREVERSIBLE) — keep TradingState halted
WTSLogger::warn("UftFutuMmStrategy[{}] resumeTrading refused (IRREVERSIBLE), keeping halted state", id());
}
}
}
}
}

void UftFutuMmStrategy::on_order(IUftStraCtx* ctx, uint32_t localid, const char* stdCode,
bool isLong, uint32_t offset, double totalQty, 
double leftQty, double price, bool isCanceled)
{
// 更新频率统计
if (_risk_monitor && isCanceled)
_risk_monitor->recordCancel();

// 计算当前时间戳:
//   - now_ms (epoch 毫秒) 给 RiskMonitor 用(closeout 超时/重试判定)
//   - uTime_HHMM / sec_in_min 给 BilateralStats 用
// 统一使用 TimeUtils::getLocalTimeNow() (epoch ms), 与 RiskMonitor 的
// checkCloseoutRetry / _current_time 保持同一时间基准。
// 旧实现 date*86400000 把 YYYYMMDD 当天数, 产生 ~1.75e15 的垃圾时间戳.
uint32_t time_hhmm = ctx->stra_get_time();
uint32_t ssmmm = ctx->stra_get_secs();
uint32_t s = ssmmm / 1000;
// v7.1: 用 replay 时钟 (与 RiskMonitor._current_time 同基准; 回测可复现)
uint64_t now_ms = _exchange_time_ms > 0 ? _exchange_time_ms : TimeUtils::getLocalTimeNow();

// 更新 Quoter 订单状态(内部会从 UnifiedOrderTracker 移除)
// 同时触发双边报价统计更新
// R3 v2: 改用 (uTime_HHMM, sec_in_min) 签名
uint32_t uTime_HHMM = time_hhmm;
uint32_t sec_in_min = s;
for (auto& [code, quoter] : _quoters)
{
if (quoter->isMyOrder(localid))
{
quoter->onOrder(localid, isCanceled, leftQty, uTime_HHMM, sec_in_min);
break;
}
}

// 从自成交防护模块中移除（订单撤销或完全成交）
if ((isCanceled || leftQty == 0) && _stp)
_stp->untrackOrder(localid);

// closeout 订单跟踪 (已拆分至 CloseoutOrchestrator, 架构重构 C3)
// 注意: 必须在 onOrderDone 抹除来源记录之前调用 (内部用 OrderRouter 来源标记识别)
_closeout_orch.onOrderEvent(ctx, localid, stdCode, isCanceled, leftQty, now_ms);

// 通知 OrderRouter 订单完成（撤销或完全成交）
if ((isCanceled || leftQty == 0) && _order_router)
_order_router->onOrderDone(localid);

// Scheme B-3: clean up arb localid → pair_id tag on order finalize
// (full fill or cancel). Defensive: removes stale entries even if
// onArbOrderFilled (in_flight tracking) already saw the fills.
if ((isCanceled || leftQty == 0) && _async_arb)
{
// A4: 套利腿撤单 → 通知 bridge (撤对侧 + 残腿标记 + 释放 in_flight).
// 必须在 onOrderFinalized 清 tag 之前查询 pair_id.
// consumePairTag 语义为"查询不抹除"(支持 partial fill), 此处调用安全.
if (isCanceled)
{
    std::string pair_id;
    if (_async_arb->consumePairTag(localid, pair_id))
    {
        _arb_bridge.onLegCancelled(ctx, pair_id);
    }
}
_async_arb->onOrderFinalized(localid);
}
}

void UftFutuMmStrategy::on_position(IUftStraCtx* ctx, const char* stdCode, bool isLong,
double prevol, double preavail, double newvol, double newavail)
{
// 框架回调：本地持仓更新（不是账户持仓）
// 注意：框架的 UftStraContext::on_position 账户持仓回调被注释掉了
// 这里收到的是本地持仓文件的缓存数据，参数含义：
//   prevol: 本地持仓量（净头寸）
//   preavail: 本地持仓量
//   newvol: 0
//   newavail: 0
// 今仓/昨仓逻辑由框架 ActionPolicy 处理，策略层不区分

double local_pos = prevol;  // 本地持仓净头寸

WTSLogger::debug("UftFutuMmStrategy[{}] Local position update: {} {}={}",
id(), stdCode, isLong ? "long" : "short", std::abs(local_pos));

// 使用策略本地持仓更新 Portfolio（而不是账户持仓）
// 一个账户可能有多个策略，每个策略只管理自己的持仓
double local_net = ctx->stra_get_local_position(stdCode);

double current = _portfolio->getPosition(stdCode);
if (std::abs(current - local_net) > 0.01)
{
WTSLogger::info("UftFutuMmStrategy[{}] Position sync: {} portfolio={} -> local_net={}",
id(), stdCode, current, local_net);
_portfolio->onPositionUpdate(stdCode, local_net);

// P0-2.3: Mark portfolio context as dirty for lazy update
_portfolio_ctx_dirty = true;
}
}

void UftFutuMmStrategy::on_channel_ready(IUftStraCtx* ctx)
{
_channel_ready = true;
_price_stale = true;  // P1-4: 标记价格过期，直到收到首个 tick

// 通道恢复, 重启套利 (与 on_channel_lost 的 setEnabled(false) 对称).
if (_async_arb)
{
    _async_arb->setEnabled(true);
}

//============================================================
// 同步持仓和未成交订单
//============================================================
WTSLogger::info("UftFutuMmStrategy[{}] syncing positions and pending orders...", id());

bool has_valid_price = false;  // 是否有有效价格用于 delta 计算

for (const auto& ci : _contract_infos)
{
// 使用策略本地持仓（而不是账户持仓）
// 一个账户可能有多个策略，每个策略只管理自己的持仓
double local_net = ctx->stra_get_local_position(ci.code.c_str());

// 尝试获取当前价格用于 Delta 计算
// 只使用 _last_mid (最新中间价)，这是从已收到的 tick 中计算的
double price = 0;
auto midIt = _last_mid.find(ci.code);
if (midIt != _last_mid.end() && midIt->second > 0)
{
price = midIt->second;
has_valid_price = true;
}

// 更新 Portfolio
double currentPos = _portfolio->getPosition(ci.code);
if (std::abs(currentPos - local_net) > 0.01)
{
_portfolio->updatePosition(ci.code, local_net, 0);
if (price > 0)
{
_portfolio->markToMarket(ci.code, price);
}
WTSLogger::info("UftFutuMmStrategy[{}] Position sync: {} local_portfolio={} -> local_net={}",
id(), ci.code, currentPos, local_net);
}

// 记录日志
if (price > 0)
{
WTSLogger::info("UftFutuMmStrategy[{}] Contract {} synced: local_net={}, multiplier={}, tickSize={}, price={:.2f}",
id(), ci.code, local_net, ci.multiplier, ci.tick_size, price);
}
else
{
WTSLogger::warn("UftFutuMmStrategy[{}] Contract {} synced: local_net={}, NO PRICE (delta will be 0, waiting for first tick)",
id(), ci.code, local_net);
}
}

// 同步后检查风控状态
double totalDelta = _portfolio->getTotalDelta();
WTSLogger::info("UftFutuMmStrategy[{}] Total delta after sync: {}", id(), totalDelta);

// 只有在非不可逆风险状态下才恢复
if (_risk_monitor)
{
if (_risk_monitor->getHaltCategory() != RiskCategory::IRREVERSIBLE)
{
// Delta 不依赖价格，可立即恢复报价
// Exposure/Loss 风控在 on_tick 中正常触发，无需等第一个 tick
if (!has_valid_price)
{
WTSLogger::info("UftFutuMmStrategy[{}] No price yet, resuming quoting (risk will activate on first tick)", id());
}

_risk_monitor->checkRiskLimits(_portfolio.get(), _violations_buf);
auto& violations = _violations_buf;
if (violations.empty())
{
// Use resumeFromRisk() instead of direct assignments
_trading_state.resumeFromRisk();
_trading_state.unblockLong();
_trading_state.unblockShort();
_blocked_contracts.clear();
_risk_monitor->resumeTrading();
_risk_monitor->resumeQuoting();
_risk_monitor->unblockLong();
_risk_monitor->unblockShort();
// A1: 同步重置协调器软风控倍数 (同 on_trade 恢复路径).
if (_coordinator) _coordinator->onExternalResumeFromRisk();
WTSLogger::info("UftFutuMmStrategy[{}] Trading resumed after channel ready (risk normalized)", id());
}
else
{
// ============================================================
// 检查是否有持仓超限，尝试自动平仓到安全水平
// ============================================================
bool has_position_breach = false;
for (const auto& v : violations)
{
if (v.type == RiskLimitType::POSITION_NET)
{
has_position_breach = true;
// 执行自动平仓
const ContractState* breached = _portfolio->getPositionBreachedContract();
if (breached)
{
        double reduction = _portfolio->getPositionReductionToLimit(*breached);
if (reduction != 0)
{
// 获取当前价格
std::unique_ptr<WTSTickData, void(*)(WTSTickData*)> tick(
ctx->stra_get_last_tick(breached->code.c_str()), 
[](WTSTickData* p){ if(p) p->release(); }
);

if (tick)
{
double price = tick->price();
if (reduction > 0)
{
// 多头超限，平多仓 — 通过 OrderRouter (exit_long)
if (_order_router)
{
auto res = _order_router->submitExitLong(ctx, breached->code.c_str(), price, std::abs(reduction), false, Source::CLOSEOUT);
if (res.rate_limited)
WTSLogger::warn("AUTO REDUCE EXIT_LONG rate limited: {}", breached->code);
else if (res.self_trade_blocked)
WTSLogger::warn("AUTO REDUCE EXIT_LONG self-trade blocked: {}", breached->code);
else
WTSLogger::warn("UftFutuMmStrategy[{}] AUTO REDUCE: EXIT_LONG {} x {} @ {} (position breach, via OrderRouter)", 
id(), breached->code, reduction, price);
}
else
{
ctx->stra_exit_long(breached->code.c_str(), price, std::abs(reduction), false);
WTSLogger::warn("UftFutuMmStrategy[{}] AUTO REDUCE: EXIT_LONG {} x {} @ {} (position breach)", 
id(), breached->code, reduction, price);
}
}
else
{
// 空头超限，平空仓 — 通过 OrderRouter (exit_short)
if (_order_router)
{
auto res = _order_router->submitExitShort(ctx, breached->code.c_str(), price, std::abs(reduction), false, Source::CLOSEOUT);
if (res.rate_limited)
WTSLogger::warn("AUTO REDUCE EXIT_SHORT rate limited: {}", breached->code);
else if (res.self_trade_blocked)
WTSLogger::warn("AUTO REDUCE EXIT_SHORT self-trade blocked: {}", breached->code);
else
WTSLogger::warn("UftFutuMmStrategy[{}] AUTO REDUCE: EXIT_SHORT {} x {} @ {} (position breach, via OrderRouter)", 
id(), breached->code, std::abs(reduction), price);
}
else
{
ctx->stra_exit_short(breached->code.c_str(), price, std::abs(reduction), false);
WTSLogger::warn("UftFutuMmStrategy[{}] AUTO REDUCE: EXIT_SHORT {} x {} @ {} (position breach)", 
id(), breached->code, std::abs(reduction), price);
}
}
}
}
}
break;  // 一次只处理一个超限
}
}

if (!has_position_breach)
{
WTSLogger::warn("UftFutuMmStrategy[{}] channel ready but risk still exists, keeping halted state", id());
}
else
{
WTSLogger::info("UftFutuMmStrategy[{}] Auto position reduction triggered, will retry after trade", id());
}

// 保持风控状态不变
// P1-1: TradingState manages its own phase; syncFromRiskMonitor removed.
// If RiskMonitor is halted, ensure TradingState reflects it.
if (_risk_monitor->isTradingHalted())
    _trading_state.setQuotingPhase(QuotingPhase::RISK_HALTED);
}
}
else
{
WTSLogger::warn("UftFutuMmStrategy[{}] channel ready but trading remains halted (IRREVERSIBLE)", id());
}
}
else
{
// Use resumeFromRisk() instead of direct assignment
_trading_state.resumeFromRisk();
}

WTSLogger::info("UftFutuMmStrategy[{}] channel ready", id());
}

void UftFutuMmStrategy::on_channel_lost(IUftStraCtx* ctx)
{
    _channel_ready = false;

    // 1. 立即暂停所有交易和报价
    // P1-1: enter RISK_HALTED on channel lost
    _trading_state.setQuotingPhase(QuotingPhase::RISK_HALTED);
    _quoting_paused_since = _exchange_time_ms > 0 ? _exchange_time_ms : TimeUtils::getLocalTimeNow();

    // 2. 撤销所有做市挂单（通道断开时无法保证订单状态）
    for (auto& [code, quoter] : _quoters)
    {
        quoter->cancelAll(ctx);
    }

    // 3. 通知风控模块
    if (_risk_monitor)
    {
        _risk_monitor->haltTrading(RiskCategory::REVERSIBLE, _portfolio ? _portfolio->getTotalPnL() : 0);
    }

    // 3.5 停止套利 — 通道断开时 arb 线程继续生成信号只会塞满 _order_queue 后无声丢弃.
    if (_async_arb)
    {
        _async_arb->setEnabled(false);
    }

    // 4. 快照当前持仓（通道恢复后用于校验）
    if (_portfolio)
    {
        for (const auto& cs : _portfolio->getAllContracts())
        {
            WTSLogger::warn("UftFutuMmStrategy[{}] Position snapshot on channel lost: {} pos={:.0f}",
                id(), cs.code, cs.position);
        }
    }

    WTSLogger::error("UftFutuMmStrategy[{}] channel lost - all orders cancelled, trading halted", id());
}

//==========================================================================
// 跨期价差套利执行逻辑 - 异步版本
//==========================================================================


void UftFutuMmStrategy::onSpreadTrade(IUftStraCtx* ctx, const std::string& pair_id,
const std::string& code, bool is_buy, 
double qty, double price)
{
WTSLogger::info("SpreadArb trade: pair={}, code={}, {} {}@{}",
pair_id, code, is_buy ? "BUY" : "SELL", qty, price);

// 1. 更新 Portfolio 持仓（关键修复：套利成交必须同步到Portfolio）
if (_portfolio)
{
ContractState* cs = _portfolio->getContract(code);
if (cs)
{
double old_pos = cs->position;
double new_pos = old_pos + (is_buy ? qty : -qty);

// 计算新的均价
double new_avg_cost = cs->avg_cost;
if (is_buy && new_pos > 0)
{
if (old_pos >= 0)
{
// 多头加仓：加权平均
new_avg_cost = (cs->avg_cost * old_pos + price * qty) / new_pos;
}
else
{
// 从空头翻多
new_avg_cost = price;
}
}
else if (!is_buy && new_pos < 0)
{
if (old_pos <= 0)
{
// 空头加仓时用绝对值计算加权均价
// old_pos<0, new_pos<0, 需要取绝对值避免负数导致计算错误
new_avg_cost = (cs->avg_cost * std::abs(old_pos) + price * qty) / std::abs(new_pos);
}
else
{
// 从多头翻空
new_avg_cost = price;
}
}
// 平仓场景：保持原均价不变（已实现盈亏通过 daily_pnl 体现）

_portfolio->updatePosition(code, new_pos, new_avg_cost);

WTSLogger::info("SpreadArb portfolio updated: code={}, pos={}->{}",
code, old_pos, new_pos);
}
else
{
WTSLogger::warn("SpreadArb trade for unknown contract: {}", code);
}
}

// 2. 更新套利管理器的仓位跟踪
if (_spread_arb_manager)
{
// SpreadArbitrageManager 内部仓位跟踪
// onTrade 方法需要由 SpreadArbitrageManager 提供
}

// 3. 更新风控计数器（套利成交也计入频率统计）
if (_risk_monitor)
{
_risk_monitor->recordTrade();
}

// 4. 标记 portfolio 上下文需要刷新
_portfolio_ctx_dirty = true;
}

void UftFutuMmStrategy::on_entrust(uint32_t localid, bool bSuccess, const char* message)
{
    //============================================================
    // 策略层不做柜台特定错误分类
    // 柜台错误(CTP平仓不足/流控等)由 TraderCTP 适配层处理
    // 策略只做通用错误计数: 达到阈值 → halt
    //============================================================
    // RISK_HALTED 期间 set(ERROR) 会被 canTransitionQuoting 静默拒绝
    // (H 来源仅允许 N), 但 _order_error_count++ 已污染状态.
    // H 是更高优先级的暂停, 不应再被下单错误覆盖, 直接忽略.
    if (_trading_state.qphase == QuotingPhase::RISK_HALTED)
    {
        WTSLogger::warn("UftFutuMmStrategy[{}] on_entrust during RISK_HALTED (success={}), ignored",
            id(), bSuccess);
        return;
    }

    if (bSuccess)
    {
        _order_error_count = 0;
        // v7.1: 报单引擎确认 → 双边统计挂单确认入口
        // (在场时间从引擎确认时刻起算, 不含发出→确认的网络延迟;
        //  回测 mocker postTask 异步触发, 回调时 quoter 已注册 ID)
        if (_main_ctx)
        {
            uint32_t uTime = _main_ctx->stra_get_time();
            uint32_t secs  = _main_ctx->stra_get_secs() / 1000;
            for (auto& [code, quoter] : _quoters)
            {
                if (quoter->isMyOrder(localid))
                {
                    quoter->onEntrustAck(localid, uTime, secs);
                    break;
                }
            }
        }
        // P1-6/U1: 用 tryResumeFrom 替代 setQuotingPhase(NORMAL),
        // 仅当 qphase 真的是 ERROR 时才翻 NORMAL (避免在其它态下乱翻)
        if (_trading_state.tryResumeFrom(QuotingPhase::ERROR))
        {
            _quoting_paused_since = 0;
            WTSLogger::info("UftFutuMmStrategy[{}] Quoting resumed after successful order", id());
        }
        return;
    }

    // 报单失败 — 通用计数
    std::string errMsg = message ? message : "" ;
    _order_error_count++;

    WTSLogger::error("UftFutuMmStrategy[{}] Order FAILED (count={}/{}): localid={}, error={}",
        id(), _order_error_count, _config.order_control.order_error_threshold, localid, errMsg);

    // v7.1 生产适配: 拒单的 localid 已被 handle*Quote 注册进 quoter level, 但订单
    // 从未上交易所。若不清理, getValidQuoteSnapshot 仍把死单当有效深度 → 统计虚高
    // (sticky/pause 期间死单滞留)。复用 onOrder(canceled) 路径移除死单 + 更新统计。
    if (_main_ctx)
    {
        uint32_t uTime = _main_ctx->stra_get_time();
        uint32_t secs  = _main_ctx->stra_get_secs() / 1000;
        for (auto& [code, quoter] : _quoters)
        {
            if (quoter->isMyOrder(localid))
            {
                quoter->onOrder(localid, true, 0, uTime, secs);
                break;
            }
        }
    }

    // 套利单被拒: 撤同 pair 另一腿,防止裸腿
    if (_async_arb && _order_router)
    {
        std::string pair_id;
        if (_async_arb->consumePairTag(localid, pair_id))
        {
            WTSLogger::warn("Arb order REJECTED: localid={}, pair={}, canceling all arb orders",
                localid, pair_id);
            _order_router->cancelByPair(_main_ctx, pair_id);  // A7: 原 cancelAllBySource 误撤其它 pair
            // A2: 补残腿防护标记 — broker 拒单场景此前仅撤单, 对侧在途成交无 hedge 回补
            _arb_bridge.markLegRejected(pair_id, 0);
        }
    }

    if (_order_error_count >= _config.order_control.order_error_threshold)
    {
        _trading_state.setQuotingPhase(QuotingPhase::ERROR);
        // 硬触发分支补设 paused_since, 让 handleQuotingAutoResume 正常工作.
        _quoting_paused_since = _exchange_time_ms > 0 ? _exchange_time_ms : TimeUtils::getLocalTimeNow();

        WTSLogger::error("UftFutuMmStrategy[{}] Trading HALTED due to consecutive order errors (count={}/threshold={})",
            id(), _order_error_count, _config.order_control.order_error_threshold);

        if (_risk_monitor)
            _risk_monitor->haltTrading(RiskCategory::REVERSIBLE);

        if (_main_ctx) {
            for (auto& [code, quoter] : _quoters)
                quoter->cancelAll(_main_ctx);
        }
    }
    else
    {
        // 软触发不 cancelAll, 维持原语义 (临时小问题, 挂单留着等下笔成功)
        _trading_state.setQuotingPhase(QuotingPhase::ERROR);
        _quoting_paused_since = _exchange_time_ms > 0 ? _exchange_time_ms : TimeUtils::getLocalTimeNow();
        WTSLogger::warn("UftFutuMmStrategy[{}] Quoting temporarily paused due to order error ({}/{})",
            id(), _order_error_count, _config.order_control.order_error_threshold);
    }
}

//==========================================================================
// 参数热更新回调
//==========================================================================

void UftFutuMmStrategy::on_params_updated()
{
    WTSLogger::info("UftFutuMmStrategy[{}] === PARAMS HOT UPDATE ===", id());
    
    // 热参数应用已拆分至 FutuHotParamManager (架构重构 C2)
    FutuHotParamManager::Targets t;
    t.config = &_config;
    t.quoters = &_quoters;
    t.spread_opts = &_spread_optimizers;
    t.aggregators = &_signal_aggregators;
    t.coordinator = _coordinator.get();
    t.portfolio = _portfolio.get();
    _hot_mgr.applyAll(t, id());
    
    WTSLogger::info("UftFutuMmStrategy[{}] === HOT UPDATE COMPLETE ===", id());
}

} // namespace futu

//==========================================================================
// 策略工厂导出
//==========================================================================

#include "../Includes/UftStrategyDefs.h"

class FutuStrategyFact : public IUftStrategyFact
{
public:
    FutuStrategyFact() {}
    virtual ~FutuStrategyFact() {}
    
    virtual const char* getName() override { return "FutuStraFact"; }
    
    virtual void enumStrategy(FuncEnumUftStrategyCallback cb) override
    {
        cb(getName(), "FutuMM", true);
    }
    
    virtual UftStrategy* createStrategy(const char* name, const char* id) override
    {
        if (strcmp(name, "FutuMM") == 0)
            return new futu::UftFutuMmStrategy(id);
        return nullptr;
    }
    
    virtual bool deleteStrategy(UftStrategy* stra) override
    {
        delete stra;
        return true;
    }
};

extern "C" {
    EXPORT_FLAG IUftStrategyFact* createStrategyFact()
    {
        return new FutuStrategyFact();
    }
    
    EXPORT_FLAG void deleteStrategyFact(IUftStrategyFact* &fact)
    {
        if (fact)
        {
            delete fact;
            fact = nullptr;
        }
    }
}
