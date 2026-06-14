/*!
* \file StrategyCoordinator.cpp
* \brief Strategy Coordinator Implementation
* 
* Complete tick processing pipeline - replaces inline on_tick logic.
*/

#include "StrategyCoordinator.h"
#include "FutuPortfolio.h"
#include "FutuQuoter.h"
#include "FutuRiskMonitor.h"
#include "ToxicFlowDetector.h"
#include "UnifiedOrderTracker.h"
#include "CorrelationManager.h"

#include "SpreadOptimizer.h"
#include "MarketDataContext.h"
#include "SelfTradeCalibrator.h"
#include "PerformanceMonitor.h"
#include "SignalAggregator.h"  // 新增：信号聚合器
#include "OrderRouter.h"       // 新增：统一下单路由器
#include "../WTSUtils/WTSCfgLoader.h"  // YAML 加载器
#include "../Includes/IUftStraCtx.h"
#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../WTSTools/WTSLogger.h"

#include <algorithm>
#include <cmath>

namespace futu {

StrategyCoordinator::StrategyCoordinator()
: _channel_ready(true)
, _tick_count(0)
, _portfolio_ctx_dirty(true)
{
}

StrategyCoordinator::~StrategyCoordinator()
{
// 智能指针自动清理
}

//==========================================================================
// Configuration Loading
//==========================================================================

bool StrategyCoordinator::loadConfig(const std::string& config_file)
{
// 使用 WTSCfgLoader 加载 YAML 配置文件
wtp::WTSVariant* cfg = WTSCfgLoader::load_from_file(config_file);
if (!cfg)
{
WTSLogger::error("StrategyCoordinator: failed to load config file '{}'", config_file);
return false;
}

// 获取 coordinator 节点
wtp::WTSVariant* coordinator = cfg->get("coordinator");
if (!coordinator)
{
// 尝试直接使用根节点（兼容无 coordinator 包裹的配置）
WTSLogger::warn("StrategyCoordinator: no 'coordinator' section in '{}', using root", config_file);
coordinator = cfg;
}

// 调用 loadConfigFromVariant 解析配置
loadConfigFromVariant(coordinator);

WTSLogger::info("StrategyCoordinator: loaded from '{}' (signal_aggregator={})", 
config_file, _cfg.use_signal_aggregator ? "ON" : "OFF");

return true;
}

void StrategyCoordinator::loadConfigFromVariant(wtp::WTSVariant* cfg)
{
if (!cfg) return;

_cfg._raw_variant = cfg;

// Helper functions
auto readBool = [](wtp::WTSVariant* v, const char* key, bool defVal) -> bool {
if (!v) return defVal;
wtp::WTSVariant* node = v->get(key);
return node ? node->asBoolean() : defVal;
};
auto readUInt32 = [](wtp::WTSVariant* v, const char* key, uint32_t defVal) -> uint32_t {
if (!v) return defVal;
wtp::WTSVariant* node = v->get(key);
return node ? (uint32_t)node->asInt64() : defVal;
};
auto readDouble = [](wtp::WTSVariant* v, const char* key, double defVal) -> double {
if (!v) return defVal;
wtp::WTSVariant* node = v->get(key);
return node ? node->asDouble() : defVal;
};

// Read modules section (supports both nested and flat structures)
wtp::WTSVariant* modules = cfg->get("modules");
if (modules) {
// Check for nested module configs (new structure)
// Each module has its own section with 'enabled' key
auto readModuleEnabled = [&](const char* name, bool defVal) -> bool {
wtp::WTSVariant* mod = modules->get(name);
if (mod) {
return readBool(mod, "enabled", defVal);
}
// Fallback to flat structure (old style: use_xxx)
std::string flatKey = std::string("use_") + name;
return readBool(modules, flatKey.c_str(), defVal);
};

// Strategy mode switches (independent control)
_cfg.use_market_making = readBool(modules, "useMarketMaking", _cfg.use_market_making);
_cfg.use_spread_arbitrage = readBool(modules, "useSpreadArbitrage", _cfg.use_spread_arbitrage);

// 新架构开关 (在 coordinator 根级别读取)
_cfg.use_signal_aggregator = readBool(cfg, "use_signal_aggregator", _cfg.use_signal_aggregator);

// Map module names to config flags (market making modules)
// 注：use_alpha_engine 和 use_market_state 已移除，由 SignalAggregator 内部管理
_cfg.use_toxicity_detector = readModuleEnabled("toxicityDetector", _cfg.use_toxicity_detector);
_cfg.use_spread_optimizer = readModuleEnabled("spreadOptimizer", _cfg.use_spread_optimizer);
_cfg.use_adaptive_params = readModuleEnabled("adaptiveParam", _cfg.use_adaptive_params);
_cfg.use_self_trade_prevention = readModuleEnabled("selfTradePrevention", _cfg.use_self_trade_prevention);

// Also check flat keys for backward compatibility
if (!_cfg.use_toxicity_detector) _cfg.use_toxicity_detector = readBool(modules, "use_toxicity_detector", false);
if (!_cfg.use_spread_optimizer) _cfg.use_spread_optimizer = readBool(modules, "use_spread_optimizer", false);

// If market making is disabled, disable all MM-specific modules
if (!_cfg.use_market_making) {
_cfg.use_toxicity_detector = false;
_cfg.use_spread_optimizer = false;
WTSLogger::info("Market making disabled, MM modules deactivated");
}

// 新架构依赖 MarketDataContext 作为数据源，由 SignalAggregator 内部管理
if (_cfg.use_signal_aggregator) {
WTSLogger::info("SignalAggregator enabled: MarketDataContext auto-enabled as data source");
}
}

// Read pipeline section
wtp::WTSVariant* pipeline = cfg->get("pipeline");
if (pipeline) {
_cfg.param_update_interval = readUInt32(pipeline, "paramUpdateInterval", _cfg.param_update_interval);
_cfg.modules.alpha_sensitivity = readDouble(pipeline, "alphaSensitivity", _cfg.modules.alpha_sensitivity);
}

// closeout/perf params are propagated from config.yaml via FutuMmConfig, not read here

// signal_pipeline section removed - weights not used in current implementation

// Read module parameters (for strategy to create modules)
if (modules) {
// ToxicityDetector parameters (cooloff_ms 仍需保留，其余已迁移到 fromVariant)
wtp::WTSVariant* toxicity = modules->get("toxicityDetector");
if (toxicity) {
_cfg.modules.toxicity_cooloff_ms = readUInt32(toxicity, "cooloffMs", _cfg.modules.toxicity_cooloff_ms);
}

// SpreadOptimizer parameters: 已迁移到 GLFTParams::fromVariant
// SpreadOptimizer: 已迁移到 GLFTParams::fromVariant
// alphaSensitivity 仍需保留在 ModuleParams 中
wtp::WTSVariant* spread = modules->get("spreadOptimizer");
if (spread) {
}
// MarketState: 无消费者，已删除

// AutoCancel parameters
wtp::WTSVariant* autoCancel = modules->get("autoCancel");
if (autoCancel) {
_cfg.modules.auto_cancel_max_age_ms = readUInt32(autoCancel, "maxAgeMs", _cfg.modules.auto_cancel_max_age_ms);
_cfg.modules.auto_cancel_price_deviation = readDouble(autoCancel, "priceDeviation", _cfg.modules.auto_cancel_price_deviation);
_cfg.modules.auto_cancel_inventory_cooldown_ms = readUInt32(autoCancel, "inventoryLimitCooldownMs", _cfg.modules.auto_cancel_inventory_cooldown_ms);
}

// SelfTradePrevention: 已迁移到 StpConfig::fromVariant

// Adaptive parameters
wtp::WTSVariant* adaptive = modules->get("adaptiveParam");
if (adaptive) {
    _cfg.modules.adaptive_update_interval = readUInt32(adaptive, "updateInterval", _cfg.modules.adaptive_update_interval);
    _cfg.modules.adaptive_min_phi = readDouble(adaptive, "minPhi", _cfg.modules.adaptive_min_phi);
    _cfg.modules.adaptive_max_phi = readDouble(adaptive, "maxPhi", _cfg.modules.adaptive_max_phi);
}

// CorrelationManager: 已迁移到 CorrelationConfig::fromVariant

// SignalAggregator: 已迁移到 SignalAggregatorConfig::fromVariant
}

// Hedging parameters (对冲控制)
_cfg.use_hedging = readBool(cfg, "useHedging", _cfg.use_hedging);
_cfg.hedge_delta_threshold = readDouble(cfg, "hedgeDeltaThreshold", _cfg.hedge_delta_threshold);
_cfg.hedge_cooldown_ms = readUInt32(cfg, "hedgeCooldownMs", _cfg.hedge_cooldown_ms);

WTSLogger::info("StrategyCoordinator: loaded config from variant (toxicity={}, perf={}, hedging={})",
_cfg.use_toxicity_detector, _cfg.perf_enabled, _cfg.use_hedging);
}

void StrategyCoordinator::initialize()
{
WTSLogger::info("StrategyCoordinator: initialized (perf={})",
_cfg.perf_enabled);
}

//==========================================================================
// Main Entry Point
//==========================================================================

ProcessingResult StrategyCoordinator::processTick(
wtp::IUftStraCtx* ctx, const char* stdCode, wtp::WTSTickData* tick)
{
ProcessingResult result;

auto start_time = std::chrono::high_resolution_clock::now();

// Build tick context
TickContext tc;
tc.code = stdCode;
tc.time_hms = ctx->stra_get_time();
tc.date = ctx->stra_get_date();
// FIX P0-1: 使用毫秒时间戳替代压缩时间戳，修复毒性冷却期比较
// 原代码: tc.timestamp = tc.date * 1000000ULL + tc.time_hms * 100ULL + ctx->stra_get_secs();
// 问题: 压缩时间戳+毫秒偏移比较无意义
tc.timestamp = TimeUtils::getLocalTimeNow();  // 毫秒时间戳，用于冷却期计算

// Stage 0: Closeout state machine (always needed)
if (processCloseout(ctx, tc)) {
result.closeout_executed = true;
result.processed = true;
return result;
}

// Stage 1: Pre-check (always needed)
if (!preCheck(ctx, tc, tick)) {
return result;
}

// Stage 2: Update market data (always needed)
updateMarketData(ctx, tc, tick);

// ===== Market Making Pipeline =====
if (_cfg.use_market_making) {
// Stage 3: Update signals (MM only)
updateSignals(ctx, tc, tick);

// Stage 4: Check risk
if (!checkRisk(ctx, tc)) {
result.processed = true;
return result;
}

// Stage 5: Process quoting (MM core)
result.quote_placed = processQuoting(ctx, tc, tick);

// Stage 6: Process auto-cancel (MM only)
result.order_canceled = processAutoCancel(ctx, tc);
}

// ===== Arbitrage Pipeline =====
// Note: Arbitrage processing is handled by UftFutuMmStrategy::processSpreadArbitrage
// when use_spread_arbitrage is enabled

// Stage 7: Check and hedge (always needed for risk management)
result.hedge_triggered = checkAndHedge(ctx);

// Stage 7.5: Position reduction removed — skew+clamp handles inventory reduction via quote offset
// (attemptPositionReduction used 3-tick cross-spread which was too costly; 
//  enhanced skew with clamp+scale now drives ask to mid for natural reduction)

// Stage 8: Update adaptive parameters
updateAdaptiveParams(ctx, tc);

result.processed = true;
_tick_count++;

auto end_time = std::chrono::high_resolution_clock::now();
result.processing_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
end_time - start_time).count();

// Record to performance monitor
if (_perf_monitor && _cfg.perf_enabled) {
_perf_monitor->recordTickToQuote(result.processing_time_ns);
_perf_monitor->recordTickProcessed();
}

return result;
}

//==========================================================================
// Stage 0: Closeout State Machine
//==========================================================================

bool StrategyCoordinator::processCloseout(wtp::IUftStraCtx* ctx, TickContext& tc)
{
if (!_risk_monitor) {
return false;
}

// 至少有一个触发点启用才继续
if (_cfg.closeout_minutes_before <= 0 && 
    (_cfg.night_close_time == 0 || _cfg.night_minutes_before <= 0)) {
return false;
}

CloseoutState state = _risk_monitor->getCloseoutState();
uint32_t closeTime = _cfg.close_time;

switch (state)
{
case CloseoutState::IDLE:
{
bool triggered = _risk_monitor->checkCloseout(tc.time_hms, closeTime);
if (triggered)
{
if (_quoters) {
for (auto& [code, quoter] : *_quoters) {
if (quoter) quoter->cancelAll(ctx);
}
}

if (_cfg.closeout_flatten_position && _portfolio) {
_risk_monitor->markCloseoutFlattening(tc.time_hms * 100);
} else {
_risk_monitor->markCloseoutCompleted(tc.time_hms * 100);
}
return true;
}
return false;
}

case CloseoutState::FAILED:
{
uint64_t now_ms = tc.timestamp;
if (_risk_monitor->checkCloseoutRetry(now_ms))
{
if (_quoters) {
for (auto& [code, quoter] : *_quoters) {
if (quoter) quoter->cancelAll(ctx);
}
}
if (_portfolio) {
_risk_monitor->markCloseoutFlattening(now_ms);
}
}
return true;
}

case CloseoutState::TRIGGERED:
case CloseoutState::FLATTENING:
case CloseoutState::RETRYING:
return true;

case CloseoutState::COMPLETED:
{
//======================================================================
// FIX Bug-A: 区分夜盘/白盘 closeout 完成
//
// 夜盘平仓完成后，立即重置状态+恢复做市。
// 白盘 closeout 完成后，不再重置（终态，直到日内交易结束）。
//
// 旧逻辑: 等 currentHour>=6 才 reset → 凌晨完成时 hour=0 不满足
//         → 整个白盘都不做市！
// 新逻辑: 夜盘 closeout 完成立即 reset + resume，白盘 closeout
//         在 minutes_before=15 时才重新触发 (14:45)，期间正常做市。
//======================================================================
const auto& closeoutInfo = _risk_monitor->getCloseoutStateInfo();

if (_cfg.night_close_time > 0 && closeoutInfo.is_night_closeout)
{
// 夜盘 closeout 完成 → 立即 reset，让白盘可以正常做市
_risk_monitor->resetCloseout();
if (_trading_state) {
    _trading_state->resume();
}
WTSLogger::info("[CLOSEOUT] Night session closeout completed, resetting for day session");
// 重新检查白盘 closeout (09:00 不会触发，14:45 才触发)
bool triggered = _risk_monitor->checkCloseout(tc.time_hms, closeTime);
if (triggered)
{
if (_quoters) {
for (auto& [code, quoter] : *_quoters) {
if (quoter) quoter->cancelAll(ctx);
}
}
if (_cfg.closeout_flatten_position && _portfolio) {
_risk_monitor->markCloseoutFlattening(tc.time_hms * 100);
} else {
_risk_monitor->markCloseoutCompleted(tc.time_hms * 100);
}
return true;
}
return false;
}

// 白盘 closeout 完成 → 终态，不做市直到日内交易结束
// FIX Bug-A-2: 只在首次进入 COMPLETED 时打日志+halt，避免每 tick 循环
if (_trading_state) {
    _trading_state->halt(TradingState::PauseReason::CLOSEOUT);
}
if (_quoters) {
for (auto& [code, quoter] : *_quoters) {
if (quoter) quoter->cancelAll(ctx);
}
}
return true;
}

default:
return false;
}
}

//==========================================================================
// Stage 1: Pre-check
//==========================================================================

bool StrategyCoordinator::preCheck(
wtp::IUftStraCtx* ctx, TickContext& tc, wtp::WTSTickData* tick)
{
if (!_channel_ready || !tick) {
return false;
}

if (_risk_monitor && _risk_monitor->isCloseoutTriggered()) {
return false;
}

// Extract prices
tc.bid_px = tick->bidprice(0);
tc.ask_px = tick->askprice(0);

// FIX P0: nan/inf tick 防御 — IEEE754 下 nan<=0 == false,会绕过 <=0 校验
// 历史教训: EC 数据存在 ts=0859... 预开盘 nan tick 污染 SpreadCalculator 的 leg_history,
// 进而 calculateBeta()=nan → hedge_ratio=nan → delta()=position*nan=nan,蔓延全局
if (!std::isfinite(tc.bid_px) || !std::isfinite(tc.ask_px)) {
    static thread_local uint64_t nan_tick_cnt = 0;
    if ((++nan_tick_cnt & 0xFFF) == 1) {  // 每 4096 次打一条,避免日志洪水
        WTSLogger::warn("StrategyCoordinator: {} non-finite tick bid={} ask={} (cnt={}), skipping",
            tc.code, tc.bid_px, tc.ask_px, nan_tick_cnt);
    }
    return false;
}

if (tc.bid_px <= 0 || tc.ask_px <= 0) {
return false;
}

tc.mid = (tc.bid_px + tc.ask_px) / 2.0;

// Get tick size from portfolio
if (_portfolio) {
const ContractState* cs = _portfolio->getContract(tc.code);
if (cs) {
tc.tick_size = cs->tick_size;
}
}

// FIX P2: tick_size=0保护 — 合约信息缺失时除零会导致报价计算崩溃
if (tc.tick_size <= 0) {
WTSLogger::warn("StrategyCoordinator: {} tick_size=0 (contract info missing), skipping tick", tc.code);
return false;
}

// 真正的交易时段检查（修复休市期间报价问题）
auto sess_it = _session_info.find(tc.code);
if (sess_it != _session_info.end() && sess_it->second)
{
wtp::WTSSessionInfo* sessInfo = sess_it->second;
// FIX: stra_get_time() 全栈约定返回 HHMM(4位), 不是 HHMMSS(6位)
// 之前 /100 → HH, 把 23:14 误判成 00:23, 导致回测全 skip
uint32_t currentTime = ctx->stra_get_time();  // HHMM 格式
tc.is_trading_session = sessInfo->isInTradingTime(currentTime);

if (!tc.is_trading_session)
{            WTSLogger::debug("StrategyCoordinator: {} not in trading session at {:04d}, skipping", 
tc.code, currentTime);
}
}
else
{
// 无 session 信息，默认允许交易（兼容旧行为）
tc.is_trading_session = true;
}

return tc.is_trading_session;
}

//==========================================================================
// Stage 2: Update Market Data
//==========================================================================

void StrategyCoordinator::updateMarketData(
wtp::IUftStraCtx* ctx, const TickContext& tc, wtp::WTSTickData* tick)
{
// Update portfolio (position and prices)
if (_portfolio)
{
_portfolio->onTick(tc.code.c_str(), tick);
_last_mid[tc.code] = tc.mid;

_global_portfolio_ctx.total_delta = _portfolio->getTotalDelta();
_global_portfolio_ctx.total_exposure = _portfolio->getTotalExposure();
_global_portfolio_ctx.related.clear();
_portfolio_ctx_dirty = false;
}
}

//==========================================================================
// Stage 3: Update Signals
//==========================================================================

void StrategyCoordinator::updateSignals(
wtp::IUftStraCtx* ctx, const TickContext& tc, wtp::WTSTickData* tick)
{
//==========================================================================
// 错误处理：SignalAggregator 未初始化
// 新架构默认启用，此分支仅在初始化失败时执行
//==========================================================================
if (!_signal_aggregators || !_market_data) {
WTSLogger::error("StrategyCoordinator: SignalAggregator not initialized, skipping signal update");
return;
}

// 1. 更新 MarketDataContext（唯一数据源）
auto book_it = _market_data->find(tc.code);
if (book_it == _market_data->end() || !book_it->second) {
return;
}
book_it->second->onTick(tick);

// 2. 使用 SignalAggregator 聚合所有信号
auto agg_it = _signal_aggregators->find(tc.code);
if (agg_it == _signal_aggregators->end() || !agg_it->second) {
return;
}

const SignalContext& sig_ctx = agg_it->second->update(*book_it->second);
SignalContext& mutable_sig_ctx = agg_it->second->getContext();

// 3. 更新市场状态暂停标志
// FIX P0-12: 使用TradingState方法
if (sig_ctx.shouldPause()) {
    if (_trading_state) _trading_state->pauseForMarket();
    // DIAG: 限频诊断日志，确认shouldPause()触发原因
    {
        uint64_t now_ms = TimeUtils::getLocalTimeNow();
        if (now_ms - _last_pause_diag_ms > 5000) {
            WTSLogger::warn("[DIAG] {} shouldPause=true: should_pause={} toxic_detected={} "
                "vol_tier={} realized_vol={:.6f} vol_percentile={:.1f}",
                tc.code,
                sig_ctx.market_state.should_pause,
                sig_ctx.toxicity.toxic_detected,
                (int)sig_ctx.volatility.vol_tier,
                sig_ctx.volatility.realized_vol,
                sig_ctx.volatility.vol_percentile);
            _last_pause_diag_ms = now_ms;
        }
    }
} else {
    if (_trading_state) _trading_state->resumeFromMarket();
}

if (_trading_state && _trading_state->market_paused && _quoters) {
auto quoter_it = _quoters->find(tc.code);
if (quoter_it != _quoters->end() && quoter_it->second) {
quoter_it->second->cancelAll(ctx);
}
}

// 4. 更新毒性检测器 (使用 SignalContext 的信号)
if (_cfg.use_toxicity_detector && _toxicity && sig_ctx.alpha.valid) {
AlphaResult alpha_res;
alpha_res.alpha = sig_ctx.alpha.alpha;
alpha_res.is_strong_signal = sig_ctx.alpha.is_strong_signal;
alpha_res.timestamp = sig_ctx.timestamp;

TradeImbalanceResult trade_res;
trade_res.net_flow = sig_ctx.trade_flow.net_flow;
trade_res.imbalance_ratio = sig_ctx.trade_flow.net_flow_normalized;

_toxicity->updateMarketAlpha(alpha_res, trade_res);
}

// 5. 更新 spread optimizer (用于交易统计，报价计算已纯函数化)
if (_cfg.use_spread_optimizer && _spread_opts) {
// NO-OP: onTick removed, SpreadOptimizer is now a functional engine
}

// 6. 更新 self trade calibrator
if (_self_trade_calibrator) {
_self_trade_calibrator->onTick(tc.code, tc.mid, tc.timestamp);
}

// 7. 更新 VPIN
if (_cfg.use_toxicity_detector && _toxicity) {
_toxicity->onTickVolume(tc.code.c_str(), tick);

ToxicityMetrics tox = _toxicity->analyze();

// 每tick输出毒性分数（debug级别），便于确认检测器在工作
WTSLogger::debug("[TOXIC] {} score={:.4f} vpin={:.4f} is_toxic={}",
    tc.code, tox.toxic_score, tox.predictive_toxicity, tox.is_toxic);

if (tox.is_toxic) {
mutable_sig_ctx.toxicity.toxicity_score = tox.toxic_score;
mutable_sig_ctx.toxicity.toxic_detected = true;
mutable_sig_ctx.toxicity.toxic_side = tox.toxic_side;
mutable_sig_ctx.toxicity.valid = true;
} else {
// FIX BUG-15b: toxic_detected每tick重算，不复位锁存
// 与should_pause相同的锁存BUG: 只设true不复位false
// 导致toxic_detected一旦被设就永久锁死
mutable_sig_ctx.toxicity.toxic_detected = false;
mutable_sig_ctx.toxicity.valid = true;
}
}
}

//==========================================================================
// Stage 4: Check Risk Limits
//==========================================================================

bool StrategyCoordinator::checkRisk(wtp::IUftStraCtx* ctx, const TickContext& tc)
{
if (!_risk_monitor || !_portfolio) return true;

// Check if previously halted (hard limit)
if (_risk_monitor->isTradingHalted()) {
// FIX P0-11/P0-12: 空指针保护 + 使用TradingState方法
if (_trading_state) {
    _trading_state->halt(TradingState::PauseReason::RISK_LIMIT);
}
// 限频日志：每5秒输出一次，避免刷屏
{
    uint64_t now_ms = TimeUtils::getLocalTimeNow();
    if (now_ms - _last_halt_log_ms > 5000) {
        WTSLogger::error("[RISK] Trading still halted (isTradingHalted=true), skipping risk check");
        _last_halt_log_ms = now_ms;
    }
}
return false;
}

if (_risk_monitor->checkDeltaRate()) {
// FIX P0-12: 使用TradingState方法
if (_trading_state) {
    _trading_state->pauseQuoting(TradingState::PauseReason::DELTA_LIMIT);
}
}

// P0-1.1: Active risk check every tick
auto violations = _risk_monitor->checkRiskLimits(_portfolio);
if (!violations.empty())
{
RiskCategory category;
RiskAction action = _risk_monitor->determineActionWithCategory(violations, category);

switch (action)
{
case RiskAction::HALT_TRADING:
// FIX P0-12: 使用TradingState方法
if (_trading_state) {
    _trading_state->halt(TradingState::PauseReason::RISK_LIMIT);
}
_risk_monitor->haltTrading(category, _portfolio->getTotalPnL());

// P0-2: halt 后动作补全 — 撤所有做市单
if (_quoters) {
    for (auto& [code, quoter] : *_quoters) {
        quoter->cancelAll(ctx);
    }
}
// 撤所有非做市活跃单
if (_order_router) {
    _order_router->cancelAllBySource(ctx, Source::CLOSEOUT);
    _order_router->cancelAllBySource(ctx, static_cast<Source>(1));  // HEDGE
}

// IRREVERSIBLE → 强平(对手价FAK)
if (category == RiskCategory::IRREVERSIBLE && _portfolio && _order_router) {
    double delta = _portfolio->getTotalDelta();
    if (std::abs(delta) > 0.01) {
        const std::string& anchor = _portfolio->getAnchorContract();
        const ContractState* cs = _portfolio->getContract(anchor);
        if (cs && cs->last_price > 0) {
            double qty = std::abs(delta);
            if (delta > 0) {
                _order_router->submitExitLong(ctx, anchor.c_str(), cs->bid1, qty, true, Source::CLOSEOUT, 1);
            } else {
                _order_router->submitExitShort(ctx, anchor.c_str(), cs->ask1, qty, true, Source::CLOSEOUT, 1);
            }
            WTSLogger::error("[RISK] FORCE FLAT: delta={:.1f}, anchor={}, qty={:.0f} @ {}",
                delta, anchor, qty, delta > 0 ? cs->bid1 : cs->ask1);
        }
    }
}

if (_arb_executor) {
AsyncArbConfig arbCfg = _arb_executor->getConfig();
arbCfg.enabled.store(false);
_arb_executor->setConfig(arbCfg);
WTSLogger::error("StrategyCoordinator[{}]: Arbitrage executor disabled due to HALT_TRADING", tc.code);
}
break;

case RiskAction::PAUSE_QUOTING:
// FIX P0-12: 使用TradingState方法
if (_trading_state) {
    _trading_state->pauseQuoting(TradingState::PauseReason::RISK_LIMIT);
}
WTSLogger::warn("[RISK] QUOTING_PAUSED: Quoting paused due to risk violation (position/exposure)");
if (_arb_executor) {
AsyncArbConfig arbCfg = _arb_executor->getConfig();
arbCfg.enabled.store(false);
_arb_executor->setConfig(arbCfg);
WTSLogger::warn("StrategyCoordinator[{}]: Arbitrage executor disabled due to PAUSE_QUOTING", tc.code);
}
break;

case RiskAction::BLOCK_SIDE_LONG:
// FIX P0-12: 使用TradingState方法
if (_trading_state) _trading_state->blockLong();
break;

case RiskAction::BLOCK_SIDE_SHORT:
// FIX P0-12: 使用TradingState方法
if (_trading_state) _trading_state->blockShort();
break;

default:
break;
}
}
else
{
// Auto-recovery check (if previously paused/blocked)
// FIX P0-11/P0-12: 空指针保护 + 使用TradingState查询
if (_trading_state && !_trading_state->isActive())
{
if (_risk_monitor->canRecover(_portfolio))
{
// FIX BUG-16: 恢复逻辑不完整 — 原代码只清quoting_paused，
// 不清market_paused和toxicity_paused，导致振荡环路:
// resumeQuoting清quoting_paused → isActive()仍为false(market_paused=true)
// → canRecover()返回true → "Risk normalized" → 但canQuote()=false → 不报价
_trading_state->resumeQuoting();
_trading_state->resumeFromMarket();     // BUG-16 fix: 清market_paused
_trading_state->resumeFromToxicity();   // BUG-16 fix: 清toxicity_paused
_trading_state->unblockLong();
_trading_state->unblockShort();

// P-11 fix: 同步RiskMonitor的atomic状态，保持单一source of truth
_risk_monitor->resumeQuoting();
_risk_monitor->unblockLong();
_risk_monitor->unblockShort();

if (_arb_executor) {
AsyncArbConfig arbCfg = _arb_executor->getConfig();
arbCfg.enabled.store(true);
_arb_executor->setConfig(arbCfg);
}
WTSLogger::info("StrategyCoordinator[{}]: Risk normalized, resuming operations", tc.code);
}
}    }

// Check toxicity cooldown
// FIX P0-11: 空指针保护 + P0-12: 使用TradingState方法
if (_toxicity && tc.timestamp < _toxicity_resume_time) {
if (_trading_state) _trading_state->pauseForToxicity();
if (_self_trade_calibrator) {
_self_trade_calibrator->decayCalibration(tc.code, tc.timestamp, _cfg.modules.toxicity_cooloff_ms);
}
} else {
// FIX P0-11/P0-12: 空指针保护 + 使用TradingState方法
if (_trading_state) _trading_state->resumeFromToxicity();
}

// FIX P0-11: 空指针保护
return !_trading_state || !_trading_state->trading_halted;
}

//==========================================================================
// Stage 5: Process Quoting
//==========================================================================

bool StrategyCoordinator::processQuoting(
wtp::IUftStraCtx* ctx, const TickContext& tc, wtp::WTSTickData* tick)
{
// FIX P0-11/P0-12: 空指针保护 + 使用TradingState查询方法
if (!_trading_state || !_trading_state->canQuote()) {
return false;
}

if (!_quoters || !_portfolio) return false;

auto it = _quoters->find(tc.code);
if (it == _quoters->end() || !it->second) return false;

//==========================================================================
// 0.5 冷启动保护：信号源未热身时使用 maxSpreadMult 保守报价
//==========================================================================
bool cold_start = false;
if (_signal_aggregators)
{
auto agg_it = _signal_aggregators->find(tc.code);
if (agg_it != _signal_aggregators->end() && agg_it->second)
{
const SignalContext& sc = agg_it->second->getContext();
if (!sc.alpha.valid || sc.alpha.confidence < _cfg.modules.alpha_sensitivity * _cfg.modules.cold_start_confidence_factor)
{
cold_start = true;
}
}
}

//==========================================================================
// 1. 获取市场信号上下文 (Alpha, Volatility Tier)
//==========================================================================
double alpha = 0.0;
const SignalContext* sig_ctx = nullptr;
if (_signal_aggregators)
{
auto agg_it = _signal_aggregators->find(tc.code);
if (agg_it != _signal_aggregators->end() && agg_it->second)
{
sig_ctx = &(agg_it->second->getContext());
alpha = sig_ctx->alpha.valid ? sig_ctx->alpha.alpha : 0.0;
}
}

//==========================================================================
// 2. 准备组合上下文 (从全局缓存构建单合约特定部分)
//==========================================================================
PortfolioContext p_ctx = _global_portfolio_ctx;

const ContractState* cs = _portfolio->getContract(tc.code);
if (cs)
{
p_ctx.current_multiplier = cs->multiplier;
p_ctx.current_hedge_ratio = cs->hedge_ratio;
p_ctx.current_price = tc.mid;
p_ctx.contract_max_delta = cs->contract_max_delta;
}

//==========================================================================
// 3. 使用 SpreadOptimizer 计算动态 Skew 和价差倍数
//==========================================================================
double skew = 0.0;
double spread_mult = 1.0;
double fallback_spread = 2.0;
if (_spread_opts) {
auto opt_it = _spread_opts->find(tc.code);
if (opt_it != _spread_opts->end() && opt_it->second) {
fallback_spread = opt_it->second->getParams().base_spread;
}
}
double l0_bid = tc.mid - fallback_spread * tc.tick_size;
double l0_ask = tc.mid + fallback_spread * tc.tick_size;

if (_spread_opts && _cfg.use_spread_optimizer && sig_ctx)
{
auto opt_it = _spread_opts->find(tc.code);
if (opt_it != _spread_opts->end() && opt_it->second)
{
double contractDelta = cs ? cs->delta() : 0.0;

GLFTResult res = opt_it->second->computeOptimalQuote(
tc.mid, contractDelta, *sig_ctx, _cfg.modules.alpha_sensitivity, &p_ctx);

skew = res.inventory_skew;
spread_mult = res.spread_mult;
l0_bid = res.bid_price;
l0_ask = res.ask_price;
}
}

//==========================================================================
// 3.1 毒性风控检查 (Toxicity Risk Control)
//==========================================================================
// FIX P0-12: 使用TradingState查询方法
bool allow_bid = _trading_state ? _trading_state->canBuy() : true;
bool allow_ask = _trading_state ? _trading_state->canSell() : true;
// v3 软风控字段：从 RiskMonitor.checkPreTradePosition 透传到 FutuQuoter.refreshQuotes
double _v3_long_util = 0.0;
double _v3_short_util = 0.0;
bool   _v3_force_ask_obligation = false;
bool   _v3_force_bid_obligation = false;
if (_risk_monitor) {
auto pre_trade = _risk_monitor->checkPreTradePosition(
tc.code, _portfolio, _order_tracker);
if (!pre_trade.allow_bid) allow_bid = false;
if (!pre_trade.allow_ask) allow_ask = false;
// v3 软风控字段透传到 quoter（util + obligation 标志）
_v3_long_util = pre_trade.long_utilization;
_v3_short_util = pre_trade.short_utilization;
_v3_force_ask_obligation = pre_trade.force_ask_obligation;
_v3_force_bid_obligation = pre_trade.force_bid_obligation;
} 
if (_cfg.use_toxicity_detector && _toxicity) {
ToxicityMetrics tox = _toxicity->analyze();

if (tox.is_toxic) {
// 设置冷却期：即使score短暂回落，也保持保护期
_toxicity_resume_time = tc.timestamp + _cfg.modules.toxicity_cooloff_ms;

if (tox.toxic_side == 1) {
allow_bid = false;
WTSLogger::warn("[TOXIC] {} Buy-side toxic (score={:.2f}), pausing bid quotes",
tc.code, tox.toxic_score);
} else if (tox.toxic_side == -1) {
allow_ask = false;
WTSLogger::warn("[TOXIC] {} Sell-side toxic (score={:.2f}), pausing ask quotes",
tc.code, tox.toxic_score);
} else {
allow_bid = false;
allow_ask = false;
WTSLogger::warn("[TOXIC] {} Both-side toxic (score={:.2f}), pausing all quotes",
tc.code, tox.toxic_score);
}
} else if (tc.timestamp < _toxicity_resume_time) {
// 冷却期内：is_toxic已恢复，但仍在保护期
allow_bid = false;
allow_ask = false;
WTSLogger::debug("[TOXIC] {} in cooloff (resume in {}ms)",
tc.code, _toxicity_resume_time - tc.timestamp);
}
}

//==========================================================================
// 3.2 冷启动保护：使用 maxSpreadMult 保守报价，同时满足做市义务
//==========================================================================
if (cold_start && _spread_opts)
{
auto opt_it = _spread_opts->find(tc.code);
if (opt_it != _spread_opts->end() && opt_it->second)
{
double max_mult = opt_it->second->getParams().max_spread_mult;
if (spread_mult < max_mult)
{
spread_mult = max_mult;
double half_spread = tc.tick_size * opt_it->second->getParams().base_spread * max_mult / 2.0;
l0_bid = tc.mid - half_spread - skew * tc.tick_size;
l0_ask = tc.mid + half_spread - skew * tc.tick_size;
l0_bid = std::floor(l0_bid / tc.tick_size) * tc.tick_size;
l0_ask = std::ceil(l0_ask / tc.tick_size) * tc.tick_size;
}
}
}

//==========================================================================
// 3.3 成交后退机制 (Fill Retreat)
//    买单成交 → bid 不得高于 (成交价 - retreat_ticks)
//    卖单成交 → ask 不得低于 (成交价 + retreat_ticks)
//==========================================================================
if (_self_trade_calibrator) {
FillRetreat retreat = _self_trade_calibrator->getFillRetreat(tc.code, tc.timestamp);
if (retreat.bid_retreat_active && l0_bid > retreat.bid_retreat_price) {
l0_bid = std::floor(retreat.bid_retreat_price / tc.tick_size) * tc.tick_size;
}
if (retreat.ask_retreat_active && l0_ask < retreat.ask_retreat_price) {
l0_ask = std::ceil(retreat.ask_retreat_price / tc.tick_size) * tc.tick_size;
}
}

//==========================================================================
// 4. 执行报价发布
//==========================================================================
it->second->refreshQuotes(ctx, tc.mid, l0_bid, l0_ask, spread_mult,
allow_bid, allow_ask, tc.timestamp,
tick->upperlimit(), tick->lowerlimit(), tick->bidprice(0), tick->askprice(0),
_v3_long_util, _v3_short_util,
_v3_force_ask_obligation, _v3_force_bid_obligation);

return true;
}
//==========================================================================
// Stage 6: Process Auto-cancel
//==========================================================================

bool StrategyCoordinator::processAutoCancel(wtp::IUftStraCtx* ctx, const TickContext& tc)
{
if (!_order_tracker) return false;

double tick_size = tc.tick_size > 0 ? tc.tick_size : 1.0;

// Check auto-cancel on the tracker directly
bool inventory_hit = _portfolio ? _portfolio->isAnyLimitBreached() : false;
double current_risk_delta = _portfolio ? _portfolio->getTotalDelta() : 0.0;

auto actions = _order_tracker->checkAutoCancel(tc.code, tc.timestamp, tc.mid, tick_size, false, inventory_hit, current_risk_delta);

if (!actions.empty()) {
for (const auto& action : actions) {
ctx->stra_cancel(action.order_id);
}
return true;
}

return false;
}

//==========================================================================
// Stage 7: Check and Hedge
//==========================================================================

bool StrategyCoordinator::checkAndHedge(wtp::IUftStraCtx* ctx)
{
if (!_portfolio || !_cfg.use_hedging)
return false;

// P1-2: 决策前从策略引擎同步持仓，确保 delta 基于最新策略持仓而非滞后快照
// 注意: 同步的是 stra_get_local_position (策略持仓)，不是账户持仓
if (ctx && _portfolio)
{
    for (const auto& c : _portfolio->getAllContracts())
    {
        double actual = ctx->stra_get_local_position(c.code.c_str());
        if (std::abs(c.position - actual) > 0.01)
        {
            WTSLogger::debug("Portfolio sync before hedge: {} {:.0f}->{:.0f}", c.code, c.position, actual);
            _portfolio->onPositionUpdate(c.code.c_str(), actual);
        }
    }
}

if (!_portfolio->needsHedging())
return false;

// ===== 对冲防震荡机制 =====
uint64_t now_ms = TimeUtils::getLocalTimeNow();
double currentDelta = _portfolio->getTotalDelta();
double max_delta = _portfolio->getParams().portfolio_max_delta;

// 0. 紧急突破：delta > 2*max_delta 时无视所有防震荡限制，立即对冲
bool is_emergency = max_delta > 0 && std::abs(currentDelta) > max_delta * 2.0;

if (!is_emergency)
{
    // 1. 冷却期检查：对冲后必须等待 hedge_cooldown_ms 才能再次对冲
    if (_last_hedge_time > 0 && (now_ms - _last_hedge_time) < _cfg.hedge_cooldown_ms)
    {
        return false;
    }

    // 2. 反向对冲保护：如果上次对冲方向与当前需要方向相反，需要delta翻转幅度超过1.2倍触发阈值
    //    防止：delta=80→SELL对冲→delta=-80→立即BUY对冲→无限循环
    if (_last_hedge_direction != 0)
    {
        bool need_buy = currentDelta < 0;   // delta<0需要BUY对冲
        bool need_sell = currentDelta > 0;  // delta>0需要SELL对冲
        bool is_reverse = (_last_hedge_direction > 0 && need_sell) || 
                          (_last_hedge_direction < 0 && need_buy);
        
        if (is_reverse)
        {
            // FIX P2-7: 反向对冲阈值基于max_delta而非上次hedge_delta
            // 原代码用abs(_last_hedge_delta)*1.2，如果上次对冲时delta很小，
            // 阈值也很小，导致频繁反向对冲(振荡)。
            // 改为基于max_delta: reverse_threshold = max_delta * factor(默认1.2)
            // 这样阈值稳定，不受上次对冲幅度影响
            double max_delta = _cfg.modules.portfolio_max_delta;
            if (max_delta <= 0) max_delta = _cfg.hedge_delta_threshold;  // fallback
            double reverse_threshold = max_delta * 1.2;
            if (std::abs(currentDelta) < reverse_threshold)
            {
                WTSLogger::debug("Hedge anti-oscillation: reverse hedge blocked, "
                    "current_delta={:.1f}, reverse_threshold={:.1f}, last_hedge_dir={}",
                    currentDelta, reverse_threshold, _last_hedge_direction);
                return false;
            }
            WTSLogger::info("Hedge reverse allowed: delta={:.1f} exceeds reverse_threshold={:.1f}",
                currentDelta, reverse_threshold);
        }
    }
}
else
{
    WTSLogger::warn("EMERGENCY HEDGE: delta={:.1f} > 2*max_delta={:.1f}, bypassing anti-oscillation",
        currentDelta, max_delta * 2.0);
}

HedgeAction action = _portfolio->computeHedge();
if (action.qty == 0)
return false;

// 3. 对冲数量限制：每次对冲不超过delta的50%，避免完全翻转
//    如果computeHedge返回的数量会导致delta翻转，截断到delta的50%
{
    double max_hedge_qty = std::abs(currentDelta) * 0.5;
    if (std::abs(action.qty) > max_hedge_qty && max_hedge_qty >= 1.0)
    {
        WTSLogger::info("Hedge qty capped: {:.0f} -> {:.0f} (anti-overshoot, delta={:.1f})",
            std::abs(action.qty), max_hedge_qty, currentDelta);
        action.qty = (action.qty > 0 ? 1 : -1) * std::round(max_hedge_qty);
    }
}

bool is_buy = action.qty > 0;
double qty = std::abs(action.qty);

if (action.is_urgent)
{
WTSLogger::warn("URGENT HEDGE: {} {}@{} (delta={})", 
is_buy ? "BUY" : "SELL", qty, action.code,
_portfolio->getTotalDelta());
}

// 使用对手价确保快速成交
double price = 0;
ContractState* cs = _portfolio->getContract(action.code);
if (cs)
{
price = is_buy ? cs->ask1 : cs->bid1;
}

// 通过 OrderRouter 下单（限速+防自成交+审计）
// 使用 stra_buy/sell 净仓模式，由框架底层按 actpolicy 自动拆分开平方向
if (_order_router)
{
OrderSubmitResult router_result;
if (is_buy)
{
router_result = _order_router->submitBuy(ctx, action.code.c_str(), price, qty, Source::HEDGING);
}
else
{
router_result = _order_router->submitSell(ctx, action.code.c_str(), price, qty, Source::HEDGING);
}

if (router_result.rate_limited)
{
WTSLogger::warn("Hedge order rate limited: {} (delta={})", 
action.code, _portfolio->getTotalDelta());
return false;
}
if (router_result.self_trade_blocked)
{
WTSLogger::warn("Hedge order self-trade blocked: {} (delta={})", 
action.code, _portfolio->getTotalDelta());
return false;
}

if (!router_result.localids.empty())
{
WTSLogger::info("Hedge order placed via OrderRouter: {} {} {}@{} (localid={})", 
action.code, is_buy ? "BUY" : "SELL", qty, price, router_result.localids.front());

if (_risk_monitor)
{
_risk_monitor->recordOrder();
}

// 记录对冲状态（防震荡）
_last_hedge_time = TimeUtils::getLocalTimeNow();
_last_hedge_direction = is_buy ? 1 : -1;
_last_hedge_delta = currentDelta;

return true;
}

WTSLogger::error("Hedge order FAILED via OrderRouter: {} (delta={})", 
action.code, _portfolio->getTotalDelta());
return false;
}

// Fallback: 直接调 ctx API（OrderRouter 未设置时）
// 使用 stra_buy/sell 净仓模式，由框架底层按 actpolicy 自动拆分开平方向
wtp::OrderIDs localids;
if (is_buy)
{
localids = ctx->stra_buy(action.code.c_str(), price, qty, 0);
}
else
{
localids = ctx->stra_sell(action.code.c_str(), price, qty, 0);
}

if (!localids.empty())
{
WTSLogger::info("Hedge order placed (direct): {} {} {}@{} (localid={})", 
action.code, is_buy ? "BUY" : "SELL", qty, price, localids.front());

if (_risk_monitor)
{
_risk_monitor->recordOrder();
}

// 记录对冲状态（防震荡）
_last_hedge_time = TimeUtils::getLocalTimeNow();
_last_hedge_direction = is_buy ? 1 : -1;
_last_hedge_delta = currentDelta;

return true;
}

WTSLogger::error("Hedge order FAILED (direct): {} (delta={})", 
action.code, _portfolio->getTotalDelta());
return false;
}

//==========================================================================
// Stage 7.5: Position Reduction — REMOVED
// Replaced by enhanced skew (clamp + inventory_skew_scale) which drives
// ask to mid for natural passive reduction, avoiding 3-tick cross-spread cost.
//==========================================================================

//==========================================================================
// Stage 8: Update Adaptive Parameters
//==========================================================================

void StrategyCoordinator::updateAdaptiveParams(wtp::IUftStraCtx* ctx, const TickContext& tc)
{
if (_tick_count % _cfg.param_update_interval != 0) {
return;
}

// Adaptive parameter update placeholder
// This is a placeholder - actual implementation would record performance
// and update parameters based on the manager's logic
}

//==========================================================================
// Reset
//==========================================================================


void StrategyCoordinator::resetSession()
{
// FIX P0-12: 使用TradingState::resume()统一重置所有状态
if (_trading_state) {
    _trading_state->resume();
}
_toxicity_resume_time = 0;
_tick_count = 0;
_last_mid.clear();
}

void StrategyCoordinator::resetDaily()
{
resetSession();
if (_risk_monitor) {
_risk_monitor->resetDaily();
}
}

} // namespace futu