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
#include "SpreadArbitrageManager.h"  // B2/B6: 平仓 intent / 聚合 z-score
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

// =====================================================================
// 策略级开关 (coordinator 根级, 唯一权威位置, 不依赖 modules 节点存在)
// =====================================================================
_cfg.use_market_making    = readBool(cfg, "useMarketMaking",    _cfg.use_market_making);
_cfg.use_spread_arbitrage = readBool(cfg, "useSpreadArbitrage", _cfg.use_spread_arbitrage);
_cfg.use_signal_aggregator = readBool(cfg, "use_signal_aggregator", _cfg.use_signal_aggregator);

// Read modules section (modules.<name>.enabled 是模块级开关的唯一权威位置)
wtp::WTSVariant* modules = cfg->get("modules");
if (modules) {
    // 模块级开关: 只读 modules.<name>.enabled, 不再 fallback 到 use_xxx 扁平键
    auto readModuleEnabled = [&](const char* name, bool defVal) -> bool {
        wtp::WTSVariant* mod = modules->get(name);
        if (mod) {
            return readBool(mod, "enabled", defVal);
        }
        return defVal;
    };

    // Map module names to config flags (market making modules)
    // 注：use_alpha_engine 和 use_market_state 已移除，由 SignalAggregator 内部管理
    _cfg.use_toxicity_detector       = readModuleEnabled("toxicityDetector",      _cfg.use_toxicity_detector);
    _cfg.use_spread_optimizer        = readModuleEnabled("spreadOptimizer",       _cfg.use_spread_optimizer);
    _cfg.use_adaptive_params         = readModuleEnabled("adaptiveParam",         _cfg.use_adaptive_params);
    _cfg.use_self_trade_prevention   = readModuleEnabled("selfTradePrevention",   _cfg.use_self_trade_prevention);

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
// 除零保护: param_update_interval 用于每 tick 取模, 配 0 会 SIGFPE
if (_cfg.param_update_interval == 0) _cfg.param_update_interval = 1;
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
wtp::IUftStraCtx* ctx, const char* stdCode, wtp::WTSTickData* tick, uint64_t now_ms)
{
ProcessingResult result;

auto start_time = std::chrono::high_resolution_clock::now();

// Build tick context
TickContext tc;
tc.code = stdCode;
tc.time_hms = ctx->stra_get_time();
tc.date = ctx->stra_get_date();
// 毫秒时间戳(epoch), 用于冷却期/closeout 等计算.
// now_ms 由调用方(on_tick)注入时复用, 避免每 tick 重复读墙钟.
tc.timestamp = (now_ms > 0) ? now_ms : TimeUtils::getLocalTimeNow();

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

// 一次性解析本合约组件指针, 后续各 Stage 复用 (消除重复字符串哈希查找)
if (_signal_aggregators) {
auto it = _signal_aggregators->find(tc.code);
if (it != _signal_aggregators->end()) tc.aggregator = it->second.get();
}
if (_market_data) {
auto it = _market_data->find(tc.code);
if (it != _market_data->end()) tc.book = it->second.get();
}
if (_quoters) {
auto it = _quoters->find(tc.code);
if (it != _quoters->end()) tc.quoter = it->second.get();
}
if (_spread_opts) {
auto it = _spread_opts->find(tc.code);
if (it != _spread_opts->end()) tc.spread_opt = it->second.get();
}

// Stage 2: Update market data (always needed)
updateMarketData(ctx, tc, tick);

// ===== Market Making Pipeline =====
if (_cfg.use_market_making) {
// Stage 3: Update signals (MM only)
updateSignals(ctx, tc, tick);

// Stage 4: Check risk
if (!checkRisk(ctx, tc)) {
    // PAUSE_QUOTING 时仍执行 hedge — 减仓才能恢复正常
    // HALT_TRADING 时跳过(已有 FORCE FLAT 在 checkRisk 内执行)
    if (_risk_monitor && !_risk_monitor->isTradingHalted()) {
        checkAndHedge(ctx);
    }
    result.processed = true;
    return result;
}

// Stage 5: Process auto-cancel (先撤旧单,再报新单)
result.order_canceled = processAutoCancel(ctx, tc);

// Stage 6: Process quoting (MM core)
result.quote_placed = processQuoting(ctx, tc, tick);
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

// P0-4: 每秒更新一次性能统计
static uint64_t _last_perf_ms = 0;
if (tc.timestamp - _last_perf_ms >= 1000) {
_perf_monitor->updatePerSecondCounters();
_last_perf_ms = tc.timestamp;
}
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

CloseoutSub state = _risk_monitor->getCloseoutSub();
uint32_t closeTime = _cfg.close_time;

switch (state)
{
case CloseoutSub::IDLE:
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
_risk_monitor->markCloseoutDraining(tc.timestamp);
} else {
_risk_monitor->markCloseoutCompleted(tc.timestamp);
}
return true;
}
return false;
}

case CloseoutSub::FAILED:
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
_risk_monitor->markCloseoutDraining(now_ms);
}
}
return true;
}

case CloseoutSub::TRIGGERED:
case CloseoutSub::DRAINING:
case CloseoutSub::ASSESSING:
case CloseoutSub::EXECUTING:
case CloseoutSub::RETRYING:
return true;

case CloseoutSub::COMPLETED:
{
//======================================================================
// 区分夜盘/白盘 closeout 完成
//
// 夜盘平仓完成后，立即重置状态+恢复做市。
// 白盘 closeout 完成后，不再重置（终态，直到日内交易结束）。
//
// 旧逻辑: 等 currentHour>=6 才 reset → 凌晨完成时 hour=0 不满足
//         → 整个白盘都不做市！
// 新逻辑: 夜盘 closeout 完成立即 reset + resume，白盘 closeout
//         在 minutes_before=15 时才重新触发 (14:45)，期间正常做市。
//======================================================================
const auto& closeoutInfo = _risk_monitor->getCloseoutSubInfo();

if (_cfg.night_close_time > 0 && closeoutInfo.is_night_closeout)
{
// 夜盘 closeout 完成 → 立即 reset，让白盘可以正常做市
_risk_monitor->resetCloseout();
if (_trading_state) {
    _trading_state->exitToQuoting();
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
_risk_monitor->markCloseoutDraining(tc.timestamp);
} else {
_risk_monitor->markCloseoutCompleted(tc.timestamp);
}
return true;
}
return false;
}

// 白盘 closeout 完成 → 终态，不做市直到日内交易结束
// 只在首次进入 COMPLETED 时打日志+halt，避免每 tick 循环
if (_trading_state) {
    _trading_state->enterCloseout();
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

// nan/inf tick 防御 — IEEE754 下 nan<=0 == false,会绕过 <=0 校验
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

// P0-1: 隔夜持仓用昨收(pre_close)作为成本基准
if (_portfolio) {
    ContractState* cs = _portfolio->getContract(tc.code);
    if (cs && cs->position != 0 && cs->avg_cost == 0) {
        // 首次收到 tick 且有隔夜持仓,用昨收设置成本基准
        double pre_close = tick->preclose();
        if (pre_close > 0) {
            _portfolio->setReferencePrice(tc.code, pre_close);
            WTSLogger::info("Overnight position reference: {} cost={} (pre_close), pos={}",
                tc.code, pre_close, cs->position);
        }
    }
}

// P0-2: 填充涨跌停价到 TickContext
tc.upper_limit = tick->upperlimit();
tc.lower_limit = tick->lowerlimit();

// Get tick size from portfolio
if (_portfolio) {
const ContractState* cs = _portfolio->getContract(tc.code);
if (cs) {
tc.tick_size = cs->tick_size;
}
}

// tick_size=0保护 — 合约信息缺失时除零会导致报价计算崩溃
if (tc.tick_size <= 0) {
WTSLogger::warn("StrategyCoordinator: {} tick_size=0 (contract info missing), skipping tick", tc.code);
return false;
}

// 真正的交易时段检查（修复休市期间报价问题）
auto sess_it = _session_info.find(tc.code);
if (sess_it != _session_info.end() && sess_it->second)
{
wtp::WTSSessionInfo* sessInfo = sess_it->second;
// stra_get_time() 全栈约定返回 HHMM(4位), 不是 HHMMSS(6位)
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

// A2 fix: publish atomic PnL snapshot for arb thread (lock-free on x86-64)
_portfolio->publishPnLSnapshot();
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

// 1. 更新 MarketDataContext（唯一数据源）— 复用 processTick 入口解析的指针
MarketDataContext* book = tc.book;
if (!book) {
auto book_it = _market_data->find(tc.code);
if (book_it == _market_data->end() || !book_it->second) {
return;
}
book = book_it->second.get();
}
book->onTick(tick);

// 2. 使用 SignalAggregator 聚合所有信号 — 复用入口解析的指针
SignalAggregator* aggregator = tc.aggregator;
if (!aggregator) {
auto agg_it = _signal_aggregators->find(tc.code);
if (agg_it == _signal_aggregators->end() || !agg_it->second) {
return;
}
aggregator = agg_it->second.get();
}

const SignalContext& sig_ctx = aggregator->update(*book);
SignalContext& mutable_sig_ctx = aggregator->getContext();

// 3. 更新市场状态暂停标志
// 使用TradingState方法
// P1-6/U1: enter 用 setQuotingPhase (会被 RISK_HALTED 校验拒绝, 安全),
//         exit 用 tryResumeFrom(MARKET) (仅 qphase==MARKET 时翻 NORMAL,
//         避免在 HALT/ERROR/TOXICITY 期间被误翻).
if (sig_ctx.shouldPause()) {
    if (_trading_state) _trading_state->setQuotingPhase(QuotingPhase::MARKET);
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
    if (_trading_state) _trading_state->tryResumeFrom(QuotingPhase::MARKET);
}

if (_trading_state && _trading_state->qphase == QuotingPhase::MARKET && tc.quoter) {
tc.quoter->cancelAll(ctx);
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

// 每tick输出毒性分数（debug级别）— 降采样: 每 50 tick 一次,
// 热路径上避免每 tick fmt 格式化开销
if ((_tick_count % 50) == 0) {
WTSLogger::debug("[TOXIC] {} score={:.4f} vpin={:.4f} is_toxic={}",
    tc.code, tox.toxic_score, tox.predictive_toxicity, tox.is_toxic);
}

if (tox.is_toxic) {
mutable_sig_ctx.toxicity.toxicity_score = tox.toxic_score;
mutable_sig_ctx.toxicity.toxic_detected = true;
mutable_sig_ctx.toxicity.toxic_side = tox.toxic_side;
mutable_sig_ctx.toxicity.valid = true;
} else {
// toxic_detected每tick重算，不复位锁存
// 与should_pause相同的锁存: 只设true不复位false
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
// 尝试自动恢复(REVERSIBLE halt): checkAndRecover 内部有节流(check_interval_ms)
// + cooldown + canRecover 全套校验, IRREVERSIBLE 会被拒绝.
if (_risk_monitor->checkAndRecover(_portfolio) && !_risk_monitor->isTradingHalted()) {
if (_trading_state) {
    _trading_state->resumeFromRisk();
    _trading_state->unblockLong();
    _trading_state->unblockShort();
}
if (_arb_executor) {
AsyncArbConfig arbCfg = _arb_executor->getConfig();
arbCfg.enabled.store(true);
_arb_executor->setConfig(arbCfg);
}
WTSLogger::info("StrategyCoordinator[{}]: Recovered from REVERSIBLE halt, resuming operations", tc.code);
// fall through: 恢复成功后继续走正常风控检查
} else {
if (_trading_state) {
    _trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
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
}

if (_risk_monitor->checkDeltaRate()) {
// 使用TradingState方法
// B3: delta-rate 停机只在此设置; 恢复走下方 violations.empty() 分支的统一
// 恢复路径 (以 !checkDeltaRate() 为门, 等 RiskMonitor 15s 冷却清除标志后
// 一次性完整恢复). 不要在此加 else 恢复分支 — 它会对违规类 RISK_HALTED
// (PAUSE/BLOCK) 误触发, 抢在完整恢复前翻转 qphase, 导致 arb 永久禁用/
// 方向 block 残留/spread_mult 不复位.
if (_trading_state) {
    _trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
}
}

// R2.4: 策略性软响应 (delta util 0.8/0.9 → WIDEN_SPREAD, 不产生硬 violation).
//   在 hard check 之前执行; soft action 不阻断 hard check (两者可叠加).
//   设计: WIDEN_SPREAD 是策略行为 (调整报价), 不是硬风控 (BLOCK/PAUSE/HALT).
if (!_trading_state || _trading_state->qphase != QuotingPhase::RISK_HALTED)
{
    RiskAction soft = _risk_monitor->checkSoftLimits(_portfolio);
    if (soft == RiskAction::WIDEN_SPREAD)
    {
        double cur_util = _portfolio ? _portfolio->getPortfolioDeltaUtilization() : 0;
        double l2 = _risk_monitor->getRateLimits().position_warning_l2;
        double target = (cur_util >= l2) ? 1.5 : 1.2;  // R2.5: L2→×1.5, L1→×1.2 (做市最低报价要求, 温和加宽)
        _risk_spread_mult = std::max(_risk_spread_mult, target);
        WTSLogger::debug("[RISK] soft WIDEN_SPREAD: spread_mult={:.1f} (util={:.2f}, L{})",
            _risk_spread_mult, cur_util, cur_util >= l2 ? 2 : 1);
    }
}

// P0-1.1: Active risk check every tick (复用缓冲, 零堆分配)
_risk_monitor->checkRiskLimits(_portfolio, _violations_buf);
auto& violations = _violations_buf;
if (!violations.empty())
{
RiskCategory category;
RiskAction action = _risk_monitor->determineActionWithCategory(violations, category);

switch (action)
{
case RiskAction::HALT_TRADING:
// 使用TradingState方法
if (_trading_state) {
    _trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
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
    _order_router->cancelAllBySource(ctx, Source::HEDGING);
    _order_router->cancelAllBySource(ctx, Source::ARBITRAGE);
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
// 使用TradingState方法
if (_trading_state) {
    _trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
}
if (_risk_monitor) _risk_monitor->pauseQuoting();  // R2.6: 同步 _quoting_paused atomic
WTSLogger::warn("[RISK] QUOTING_PAUSED: Quoting paused due to risk violation (position/exposure)");
// 撤销存量做市单 — 旧代码只停新报价不撤旧单, 持仓已超限时旧报价继续被成交
if (_quoters) {
    for (auto& [code, quoter] : *_quoters) {
        quoter->cancelAll(ctx);
    }
}
if (_arb_executor) {
    AsyncArbConfig arbCfg = _arb_executor->getConfig();
    arbCfg.enabled.store(false);
    _arb_executor->setConfig(arbCfg);
    WTSLogger::warn("StrategyCoordinator[{}]: Arbitrage executor disabled due to PAUSE_QUOTING", tc.code);
}
break;

case RiskAction::BLOCK_SIDE_LONG:
// R2.7: 进入 RISK_HALTED, 走统一恢复路径 (此前 blockLong 后 qphase 仍 NORMAL,
// 恢复分支要求 qphase==RISK_HALTED → block 永久残留, 仅 channel_ready 可清)
if (_trading_state) {
    _trading_state->blockLong();
    _trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
}
if (_risk_monitor) _risk_monitor->pauseQuoting();
WTSLogger::warn("[RISK] BLOCK_SIDE_LONG: halted until recovery");
break;

case RiskAction::BLOCK_SIDE_SHORT:
// R2.7: 同 BLOCK_SIDE_LONG
if (_trading_state) {
    _trading_state->blockShort();
    _trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
}
if (_risk_monitor) _risk_monitor->pauseQuoting();
WTSLogger::warn("[RISK] BLOCK_SIDE_SHORT: halted until recovery");
break;

case RiskAction::WIDEN_SPREAD:
// R2.5: 分级倍数 — L1(util≥0.8)→×1.5, L2(util≥0.9)→×2.0.
// (soft check 已在 hard check 之前处理了主要的 WIDEN; 此处处理 WARNING 级别升级路径,
//  即 determineActionWithCategory 末尾 breachCount>=widen_threshold 的返回)
{
    double cur_util = _portfolio ? _portfolio->getPortfolioDeltaUtilization() : 0;
    double l2 = _risk_monitor->getRateLimits().position_warning_l2;
    double target_mult = (cur_util >= l2) ? 1.5 : 1.2;  // R2.5: L2→×1.5, L1→×1.2
    _risk_spread_mult = std::max(_risk_spread_mult, target_mult);
    WTSLogger::warn("[RISK] WIDEN_SPREAD: spread_mult={:.1f} (util={:.2f}, L{})",
        _risk_spread_mult, cur_util, cur_util >= l2 ? 2 : 1);
}
break;

// R2.5/D5: REDUCE_SIZE 已删除 — 做市有最低报价数量要求, 不能 reduce qty;
//   统一用 WIDEN_SPREAD 分级倍数替代 (加宽 spread 降低成交率, 近似 qty 缩减)

case RiskAction::FLATTEN_POSITION:
{
// R2.3: 多类 BREACH 同时发生 (breachCount>=flatten_threshold) 时触发, 此前不可达.
// 比 HALT 轻一级: 不翻 trading_state/不 halt, 但撤单 + 停 arb + anchor 强平.
WTSLogger::error("[RISK] FLATTEN_POSITION: cancel all quotes + disable arb + force flat anchor");
if (_quoters) {
    for (auto& [code, quoter] : *_quoters) {
        quoter->cancelAll(ctx);
    }
}
if (_order_router) {
    _order_router->cancelAllBySource(ctx, Source::CLOSEOUT);
    _order_router->cancelAllBySource(ctx, Source::HEDGING);
    _order_router->cancelAllBySource(ctx, Source::ARBITRAGE);
}
if (_arb_executor) {
    AsyncArbConfig arbCfg = _arb_executor->getConfig();
    arbCfg.enabled.store(false);
    _arb_executor->setConfig(arbCfg);
}
// anchor 强平 (对手价 FAK, 与 HALT_TRADING 的 IRREVERSIBLE 段同逻辑)
if (_portfolio && _order_router) {
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
            WTSLogger::error("[RISK] FLATTEN FORCE FLAT: delta={:.1f}, anchor={}, qty={:.0f}",
                delta, anchor, qty);
        }
    }
}
break;
}

default:
break;
}
}
else
{
// Auto-recovery check (仅针对 RISK_HALTED — MARKET/TOXICITY/ERROR 暂停
// 有各自的恢复路径, 旧代码 !isActive() 会把 MARKET 暂停误翻 NORMAL 造成状态闪烁)
// B3: delta-rate 停机期间 (!checkDeltaRate() 为 false) 禁止恢复 —
// 等 RiskMonitor 冷却清除 _delta_rate_breached 后才允许走统一恢复.
if (_trading_state && _trading_state->qphase == QuotingPhase::RISK_HALTED)
{
if (!_risk_monitor->checkDeltaRate() && _risk_monitor->canRecover(_portfolio))
{
// P1-1: resumeFromRisk() unconditionally sets qphase=NORMAL
// (replaces old 3-call recovery that cleared individual bool flags)
_trading_state->resumeFromRisk();
_trading_state->unblockLong();
_trading_state->unblockShort();

// P-11 fix: 同步RiskMonitor的atomic状态，保持单一source of truth
_risk_monitor->resumeQuoting();
_risk_monitor->unblockLong();
_risk_monitor->unblockShort();

// R2: 重置软风控倍数 (WIDEN_SPREAD 分级设置的, 恢复时归 1.0)
_risk_spread_mult = 1.0;

if (_arb_executor) {
AsyncArbConfig arbCfg = _arb_executor->getConfig();
arbCfg.enabled.store(true);
_arb_executor->setConfig(arbCfg);
}
WTSLogger::info("StrategyCoordinator[{}]: Risk normalized, resuming operations", tc.code);
}
}    }

// Check toxicity cooldown
// 空指针保护 + P0-12: 使用TradingState方法
// P1-6/U1: enter 用 setQuotingPhase, exit 用 tryResumeFrom(TOXICITY)
// 避免冷却期结束时在 HALT/ERROR/MARKET 期间被误翻 NORMAL.
if (_toxicity && tc.timestamp < _toxicity_resume_time) {
if (_trading_state) _trading_state->setQuotingPhase(QuotingPhase::TOXICITY);
if (_self_trade_calibrator) {
_self_trade_calibrator->decayCalibration(tc.code, tc.timestamp, _cfg.modules.toxicity_cooloff_ms);
}
} else {
if (_trading_state) _trading_state->tryResumeFrom(QuotingPhase::TOXICITY);
}

// 空指针保护
return !_trading_state || _trading_state->qphase != QuotingPhase::RISK_HALTED;
}

//==========================================================================
// Stage 5: Process Quoting
//==========================================================================

bool StrategyCoordinator::processQuoting(
wtp::IUftStraCtx* ctx, const TickContext& tc, wtp::WTSTickData* tick)
{
if (!_trading_state || !_trading_state->canQuote()) {
return false;
}

if (!_quoters || !_portfolio) return false;

if (!tc.quoter) return false;

//==========================================================================
// 0.5 冷启动保护：信号源未热身时使用 maxSpreadMult 保守报价
//==========================================================================
bool cold_start = false;
if (tc.aggregator)
{
const SignalContext& sc = tc.aggregator->getContext();
if (!sc.alpha.valid || sc.alpha.confidence < _cfg.modules.alpha_sensitivity * _cfg.modules.cold_start_confidence_factor)
{
cold_start = true;
}
}

//==========================================================================
// 1. 获取市场信号上下文 (Alpha, Volatility Tier)
//==========================================================================
double alpha = 0.0;
const SignalContext* sig_ctx = nullptr;
if (tc.aggregator)
{
sig_ctx = &(tc.aggregator->getContext());
alpha = sig_ctx->alpha.valid ? sig_ctx->alpha.alpha : 0.0;
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
if (tc.spread_opt) {
fallback_spread = tc.spread_opt->getParams().base_spread;
}
double l0_bid = tc.mid - fallback_spread * tc.tick_size;
double l0_ask = tc.mid + fallback_spread * tc.tick_size;

if (tc.spread_opt && _cfg.use_spread_optimizer && sig_ctx)
{
double contractDelta = cs ? cs->delta() : 0.0;

GLFTResult res = tc.spread_opt->computeOptimalQuote(
tc.mid, contractDelta, *sig_ctx, _cfg.modules.alpha_sensitivity, &p_ctx);

skew = res.inventory_skew;
spread_mult = res.spread_mult;
l0_bid = res.bid_price;
l0_ask = res.ask_price;
}

// R2: 软风控倍数 (WIDEN_SPREAD 分级设置: L1→1.5, L2→2.0; canRecover 恢复时重置 1.0)
spread_mult *= _risk_spread_mult;

//==========================================================================
// 3.1 毒性风控检查 (Toxicity Risk Control)
//==========================================================================
// 使用TradingState查询方法
bool allow_bid = _trading_state ? _trading_state->canBuy() : true;
bool allow_ask = _trading_state ? _trading_state->canSell() : true;

//==========================================================================
// 3.05 B2: ARB 平仓协同 — 抑制与 arb 平仓反向的 MM 报价侧.
//   arb 卖 leg → 抑制 MM bid (防 MM 买回 arb 正在拆除的库存);
//   arb 买 leg → 抑制 MM ask (同理). 同时天然避免自相成交 (STP 为第二道防线).
//   仅 STOP_LOSS/TIMEOUT 主动平仓期间触发; CLOSE 走 B-3 时无 intent, 不影响 MM.
//==========================================================================
if (_arb_manager)
{
int arb_close_dir = _arb_manager->getArbCloseDirection(tc.code);
if (arb_close_dir > 0)
{
    allow_ask = false;
    WTSLogger::debug("[ARB-SYNC] {} arb buying leg, suppress MM ask", tc.code);
}
else if (arb_close_dir < 0)
{
    allow_bid = false;
    WTSLogger::debug("[ARB-SYNC] {} arb selling leg, suppress MM bid", tc.code);
}

//----------------------------------------------------------------------
// B6: MarketMakingEnhancer 激活 (观测模式) — 计算 adjustment 但暂不注入 skew.
//   聚合 z-score 经 1:N 映射 (一合约属多 pair 时取 |z| 最大者).
//   adjustment 注入报价的效果未经回测验证, 与 B2 抑制叠加可能过度;
//   先以 debug 日志观测其数值分布, C2 阶段再决定是否注入 (见设计文档 §B6).
//----------------------------------------------------------------------
double agg_z = _arb_manager->getAggregateZscore(tc.code);
if (std::abs(agg_z) > 0.1)
{
    auto adj = _arb_manager->getQuotingAdjustmentForLeg(tc.code, tc.timestamp);
    if (adj.confidence > 0.0)
    {
        WTSLogger::debug("[ARB-ENH] {} agg_z={:.2f} adj[bid={:.3f},ask={:.3f},mult={:.2f},supB={},supA={}] (observe-only)",
            tc.code, agg_z, adj.bid_skew_adjustment, adj.ask_skew_adjustment,
            adj.spread_multiplier, adj.suppress_bid, adj.suppress_ask);
    }
}
}
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
// 3.15 涨跌停保护 (P0-2)
//   L1: 距涨跌停 <= 20 ticks → 加宽 spread 2x
//   L2: 距涨跌停 <= 10 ticks → block 加仓侧(义务报价由 validatePrice 判断)
//   L3: 锁板(mid ≈ 涨跌停) → 双边暂停
//==========================================================================
if (tc.upper_limit > 0 && tc.lower_limit > 0 && tc.tick_size > 0) {
    double dist_upper = (tc.upper_limit - tc.mid) / tc.tick_size;
    double dist_lower = (tc.mid - tc.lower_limit) / tc.tick_size;

    // L1: 距涨跌停 <= 20 ticks, 加宽 spread
    if (dist_upper <= 20.0 || dist_lower <= 20.0) {
        spread_mult *= 2.0;
    }

    // L2: 距涨停 <= 10 ticks → block 买单(避免吃到涨停)
    if (dist_upper <= 10.0) {
        allow_bid = false;
        WTSLogger::warn("[LIMIT] {} near UPPER ({} ticks), block bid", tc.code, (int)dist_upper);
    }
    // L2: 距跌停 <= 10 ticks → block 卖单
    if (dist_lower <= 10.0) {
        allow_ask = false;
        WTSLogger::warn("[LIMIT] {} near LOWER ({} ticks), block ask", tc.code, (int)dist_lower);
    }

    // L3: 锁板 → 双边暂停
    if (dist_upper <= 0.5 || dist_lower <= 0.5) {
        allow_bid = false;
        allow_ask = false;
        WTSLogger::error("[LIMIT] {} LOCKED at limit, PAUSE all quotes", tc.code);
    }
}

//==========================================================================
// 3.2 冷启动保护：使用 maxSpreadMult 保守报价，同时满足做市义务
//==========================================================================
if (cold_start && tc.spread_opt)
{double max_mult = tc.spread_opt->getParams().max_spread_mult;
if (spread_mult < max_mult)
{
spread_mult = max_mult;
double half_spread = tc.tick_size * tc.spread_opt->getParams().base_spread * max_mult / 2.0;
l0_bid = tc.mid - half_spread - skew * tc.tick_size;
l0_ask = tc.mid + half_spread - skew * tc.tick_size;
l0_bid = std::floor(l0_bid / tc.tick_size) * tc.tick_size;
        l0_ask = std::ceil(l0_ask / tc.tick_size) * tc.tick_size;
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
tc.quoter->refreshQuotes(ctx, tc.mid, l0_bid, l0_ask, spread_mult,
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

const auto& actions = _order_tracker->checkAutoCancel(tc.code, tc.timestamp, tc.mid, tick_size, false, inventory_hit, current_risk_delta);

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

// 0. 紧急突破：delta > 2*max_delta 时绕过反向保护，但仍受 cooldown 约束
//    在 match_this_tick 回测撮合模式下，如果 emergency 绕过 cooldown，
//    hedge 单秒成交 → 同 tick Delta 变化 → 再次触发 emergency → 无限循环。
//    实盘 FAK 不秒成交，但 cooldown 对实盘也有益（等待成交回流再决策）。
bool is_emergency = max_delta > 0 && std::abs(currentDelta) > max_delta * 2.0;

// 冷却期检查：所有 hedge（含 emergency）都受 cooldown 约束
if (_last_hedge_time > 0 && (now_ms - _last_hedge_time) < _cfg.hedge_cooldown_ms)
{
    if (!is_emergency)
    {
        return false;
    }
    // emergency 也受 cooldown：只记录不执行
    // 防止 match_this_tick 模式下 hedge 秒成交 → 连续 emergency hedge 循环
    WTSLogger::debug("Emergency hedge blocked by cooldown: delta={:.1f}, "
        "remaining {}ms", currentDelta,
        _cfg.hedge_cooldown_ms - (now_ms - _last_hedge_time));
    return false;
}

if (!is_emergency)
{
    // 1. 反向对冲保护：如果上次对冲方向与当前需要方向相反，需要delta翻转幅度超过1.2倍触发阈值
    //    防止：delta=80→SELL对冲→delta=-80→立即BUY对冲→无限循环
    if (_last_hedge_direction != 0)
    {
        bool need_buy = currentDelta < 0;   // delta<0需要BUY对冲
        bool need_sell = currentDelta > 0;  // delta>0需要SELL对冲
        bool is_reverse = (_last_hedge_direction > 0 && need_sell) || 
                          (_last_hedge_direction < 0 && need_buy);
        
        if (is_reverse)
        {
            double reverse_max = _cfg.modules.portfolio_max_delta;
            if (reverse_max <= 0) reverse_max = _cfg.hedge_delta_threshold;
            double reverse_threshold = reverse_max * 1.2;
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
    WTSLogger::warn("EMERGENCY HEDGE: delta={:.1f} > 2*max_delta={:.1f}",
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
// P1-1: reset() replaces old resume() — resets to QUOTING+NORMAL
if (_trading_state) {
    _trading_state->reset();
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