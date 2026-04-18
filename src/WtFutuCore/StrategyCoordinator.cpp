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
#include "AdaptiveParamManager.h"
#include "SignalAggregator.h"  // 新增：信号聚合器
#include "../WTSUtils/WTSCfgLoader.h"  // YAML 加载器
#include "../Includes/IUftStraCtx.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../WTSTools/WTSLogger.h"

#include <algorithm>
#include <cmath>

namespace futu {

StrategyCoordinator::StrategyCoordinator()
    : _trading_halted(false)
    , _quoting_paused(false)
    , _long_blocked(false)
    , _short_blocked(false)
    , _market_state_paused(false)
    , _toxicity_paused(false)
    , _channel_ready(true)
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
        _cfg.use_auto_cancel = readModuleEnabled("autoCancel", _cfg.use_auto_cancel);
        _cfg.use_synthetic_transaction = readModuleEnabled("syntheticTransaction", _cfg.use_synthetic_transaction);
        _cfg.use_adaptive_params = readModuleEnabled("adaptiveParam", _cfg.use_adaptive_params);
        _cfg.use_self_trade_prevention = readModuleEnabled("selfTradePrevention", _cfg.use_self_trade_prevention);
        
        // Also check flat keys for backward compatibility
        if (!_cfg.use_toxicity_detector) _cfg.use_toxicity_detector = readBool(modules, "use_toxicity_detector", false);
        if (!_cfg.use_spread_optimizer) _cfg.use_spread_optimizer = readBool(modules, "use_spread_optimizer", false);
        if (!_cfg.use_auto_cancel) _cfg.use_auto_cancel = readBool(modules, "use_auto_cancel", false);
        
        // If market making is disabled, disable all MM-specific modules
        if (!_cfg.use_market_making) {
            _cfg.use_toxicity_detector = false;
            _cfg.use_spread_optimizer = false;
            _cfg.use_auto_cancel = false;
            _cfg.use_synthetic_transaction = false;
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
        _cfg.closeout_minutes_before = readUInt32(pipeline, "closeoutMinutesBefore", _cfg.closeout_minutes_before);
        _cfg.close_time = readUInt32(pipeline, "closeTime", _cfg.close_time);
        _cfg.closeout_flatten_position = readBool(pipeline, "closeoutFlattenPosition", _cfg.closeout_flatten_position);
        _cfg.toxicity_cooloff_ms = readUInt32(pipeline, "globalToxicityCooldownMs", _cfg.toxicity_cooloff_ms);
    }
    
    // Read performance section
    wtp::WTSVariant* perf = cfg->get("performance");
    if (perf) {
        _cfg.perf_enabled = readBool(perf, "enabled", _cfg.perf_enabled);
        _cfg.perf_log_interval = readUInt32(perf, "logInterval", _cfg.perf_log_interval);
        _cfg.perf_warn_threshold_ns = readUInt32(perf, "warnThresholdNs", _cfg.perf_warn_threshold_ns);
        _cfg.perf_critical_threshold_ns = readUInt32(perf, "criticalThresholdNs", _cfg.perf_critical_threshold_ns);
    }
    
    // signal_pipeline section removed - weights not used in current implementation
    
    // Read module parameters (for strategy to create modules)
    if (modules) {
        // ToxicityDetector parameters
        wtp::WTSVariant* toxicity = modules->get("toxicityDetector");
        if (toxicity) {
            _cfg.modules.toxicity_vpin_threshold = readDouble(toxicity, "vpinThreshold", _cfg.modules.toxicity_vpin_threshold);
            _cfg.modules.toxicity_window = readUInt32(toxicity, "window", _cfg.modules.toxicity_window);
            _cfg.modules.toxicity_cooloff_ms = readUInt32(toxicity, "cooloffMs", _cfg.modules.toxicity_cooloff_ms);
        }
        
        // SpreadOptimizer parameters (含市场状态检测参数)
        wtp::WTSVariant* spread = modules->get("spreadOptimizer");
        if (spread) {
            _cfg.modules.spread_vol_sensitivity = readDouble(spread, "volSensitivity", _cfg.modules.spread_vol_sensitivity);
            _cfg.modules.spread_depth_sensitivity = readDouble(spread, "depthSensitivity", _cfg.modules.spread_depth_sensitivity);
            _cfg.modules.spread_vol_window = readUInt32(spread, "volWindow", _cfg.modules.spread_vol_window);
            _cfg.modules.spread_min_mult = readDouble(spread, "minSpreadMult", _cfg.modules.spread_min_mult);
            _cfg.modules.spread_phi = readDouble(spread, "phi", _cfg.modules.spread_phi);
            _cfg.modules.spread_portfolio_skew_weight = readDouble(spread, "portfolioSkewWeight", _cfg.modules.spread_portfolio_skew_weight);
            _cfg.modules.spread_min_correlation = readDouble(spread, "minCorrelation", _cfg.modules.spread_min_correlation);
            // Delta-aware skew parameters
            _cfg.modules.delta_skew_threshold = readDouble(spread, "deltaSkewThreshold", _cfg.modules.delta_skew_threshold);
            _cfg.modules.delta_skew_factor = readDouble(spread, "deltaSkewFactor", _cfg.modules.delta_skew_factor);
            // Note: delta_limit is set from strategy config (portfolio delta_limit), not from yaml
            
            // MarketState parameters (子节点，融合后)
            wtp::WTSVariant* market = spread->get("marketState");
            if (market) {
                _cfg.modules.market_vol_threshold = readDouble(market, "volThreshold", _cfg.modules.market_vol_threshold);
                _cfg.modules.market_move_threshold = readDouble(market, "moveThreshold", _cfg.modules.market_move_threshold);
                _cfg.modules.market_spread_threshold = readDouble(market, "spreadThreshold", _cfg.modules.market_spread_threshold);
                _cfg.modules.market_volume_threshold = readDouble(market, "volumeThreshold", _cfg.modules.market_volume_threshold);
                _cfg.modules.market_cooldown_ticks = readUInt32(market, "cooldownTicks", _cfg.modules.market_cooldown_ticks);
            }
        }
        
        // AutoCancel parameters
        wtp::WTSVariant* autoCancel = modules->get("autoCancel");
        if (autoCancel) {
            _cfg.modules.auto_cancel_max_age_ms = readUInt32(autoCancel, "maxAgeMs", _cfg.modules.auto_cancel_max_age_ms);
            _cfg.modules.auto_cancel_price_deviation = readDouble(autoCancel, "priceDeviation", _cfg.modules.auto_cancel_price_deviation);
            _cfg.modules.auto_cancel_on_state_change = readBool(autoCancel, "cancelOnStateChange", _cfg.modules.auto_cancel_on_state_change);
            _cfg.modules.auto_cancel_on_inventory_limit = readBool(autoCancel, "cancelOnInventoryLimit", _cfg.modules.auto_cancel_on_inventory_limit);
            _cfg.modules.auto_cancel_inventory_cooldown_ms = readUInt32(autoCancel, "inventoryLimitCooldownMs", _cfg.modules.auto_cancel_inventory_cooldown_ms);
        }
        
        // SelfTradePrevention parameters
        wtp::WTSVariant* stp = modules->get("selfTradePrevention");
        if (stp) {
            _cfg.modules.stp_enabled = readBool(stp, "enabled", _cfg.modules.stp_enabled);
            _cfg.modules.stp_min_price_gap = readDouble(stp, "minPriceGap", _cfg.modules.stp_min_price_gap);
            _cfg.modules.stp_allow_same_price = readBool(stp, "allowSamePrice", _cfg.modules.stp_allow_same_price);
            _cfg.modules.stp_price_adjust_ticks = readDouble(stp, "priceAdjustTicks", _cfg.modules.stp_price_adjust_ticks);
        }
        
        // SyntheticTransaction parameters
        wtp::WTSVariant* synthetic = modules->get("syntheticTransaction");
        if (synthetic) {
            _cfg.modules.synthetic_enabled = readBool(synthetic, "enabled", _cfg.modules.synthetic_enabled);
            _cfg.modules.synthetic_tick_weight = readDouble(synthetic, "tickWeight", _cfg.modules.synthetic_tick_weight);
            _cfg.modules.synthetic_book_weight = readDouble(synthetic, "bookWeight", _cfg.modules.synthetic_book_weight);
            _cfg.modules.synthetic_self_trade_weight = readDouble(synthetic, "selfTradeWeight", _cfg.modules.synthetic_self_trade_weight);
            _cfg.modules.synthetic_min_samples = readUInt32(synthetic, "minSamples", _cfg.modules.synthetic_min_samples);
        }
    }
    
    //------------------------------------------------------------
    // SelfTradeCalibrator (做市专用)
    //------------------------------------------------------------
    WTSVariant* calibrator = modules->get("selfTradeCalibrator");
    if (calibrator) {
        _cfg.modules.calibrator_lookback_trades = readUInt32(calibrator, "lookbackTrades", _cfg.modules.calibrator_lookback_trades);
        _cfg.modules.calibrator_toxicity_window_ms = readUInt32(calibrator, "toxicityWindowMs", _cfg.modules.calibrator_toxicity_window_ms);
        _cfg.modules.calibrator_adverse_threshold = readDouble(calibrator, "adverseThreshold", _cfg.modules.calibrator_adverse_threshold);
    }
    
    //------------------------------------------------------------
    // TickTransactionInferer (做市专用)
    //------------------------------------------------------------
    WTSVariant* inferer = modules->get("tickInferer");
    if (inferer) {
        _cfg.modules.inferer_imbalance_window_ms = readUInt32(inferer, "imbalanceWindowMs", _cfg.modules.inferer_imbalance_window_ms);
        _cfg.modules.inferer_large_trade_threshold = readDouble(inferer, "largeTradeThreshold", _cfg.modules.inferer_large_trade_threshold);
        _cfg.modules.inferer_min_confidence = readDouble(inferer, "minConfidence", _cfg.modules.inferer_min_confidence);
    }
    
    //------------------------------------------------------------
    // AdaptiveParamManager (做市专用)
    //------------------------------------------------------------
    WTSVariant* adaptive = modules->get("adaptiveParam");
    if (adaptive) {
        _cfg.modules.adaptive_update_interval = readUInt32(adaptive, "updateInterval", _cfg.modules.adaptive_update_interval);
        _cfg.modules.adaptive_learning_rate = readDouble(adaptive, "learningRate", _cfg.modules.adaptive_learning_rate);
        _cfg.modules.adaptive_min_phi = readDouble(adaptive, "minPhi", _cfg.modules.adaptive_min_phi);
        _cfg.modules.adaptive_max_phi = readDouble(adaptive, "maxPhi", _cfg.modules.adaptive_max_phi);
    }
    
    //------------------------------------------------------------
    // CorrelationManager (做市+套利共用)
    //------------------------------------------------------------
    WTSVariant* correlation = modules->get("correlationManager");
    if (correlation) {
        _cfg.modules.correlation_window_size = readUInt32(correlation, "windowSize", _cfg.modules.correlation_window_size);
        _cfg.modules.correlation_min_correlation = readDouble(correlation, "minCorrelation", _cfg.modules.correlation_min_correlation);
        _cfg.modules.correlation_spread_z_threshold = readDouble(correlation, "spreadZThreshold", _cfg.modules.correlation_spread_z_threshold);
    }
    
    //------------------------------------------------------------
    // SignalAggregator (信号聚合器)
    //------------------------------------------------------------
    WTSVariant* signalAgg = modules->get("signalAggregator");
    if (signalAgg) {
        // 信号源开关
        _cfg.modules.signal_use_volatility = readBool(signalAgg, "useVolatility", _cfg.modules.signal_use_volatility);
        _cfg.modules.signal_use_ofi = readBool(signalAgg, "useOfi", _cfg.modules.signal_use_ofi);
        _cfg.modules.signal_use_trade_flow = readBool(signalAgg, "useTradeFlow", _cfg.modules.signal_use_trade_flow);
        _cfg.modules.signal_use_book_imbalance = readBool(signalAgg, "useBookImbalance", _cfg.modules.signal_use_book_imbalance);
        _cfg.modules.signal_use_momentum = readBool(signalAgg, "useMomentum", _cfg.modules.signal_use_momentum);
        _cfg.modules.signal_use_lead_lag = readBool(signalAgg, "useLeadLag", _cfg.modules.signal_use_lead_lag);
        
        // 窗口参数
        _cfg.modules.signal_volatility_window = readUInt32(signalAgg, "volatilityWindow", _cfg.modules.signal_volatility_window);
        _cfg.modules.signal_ofi_window = readUInt32(signalAgg, "ofiWindow", _cfg.modules.signal_ofi_window);
        _cfg.modules.signal_trade_flow_window = readUInt32(signalAgg, "tradeFlowWindow", _cfg.modules.signal_trade_flow_window);
        _cfg.modules.signal_momentum_window = readUInt32(signalAgg, "momentumWindow", _cfg.modules.signal_momentum_window);
        _cfg.modules.signal_lead_lag_window = readUInt32(signalAgg, "leadLagWindow", _cfg.modules.signal_lead_lag_window);
        
        // 信号权重
        _cfg.modules.signal_ofi_weight = readDouble(signalAgg, "ofiWeight", _cfg.modules.signal_ofi_weight);
        _cfg.modules.signal_trade_weight = readDouble(signalAgg, "tradeWeight", _cfg.modules.signal_trade_weight);
        _cfg.modules.signal_book_imbalance_weight = readDouble(signalAgg, "bookImbalanceWeight", _cfg.modules.signal_book_imbalance_weight);
        _cfg.modules.signal_momentum_weight = readDouble(signalAgg, "momentumWeight", _cfg.modules.signal_momentum_weight);
        _cfg.modules.signal_lead_lag_weight = readDouble(signalAgg, "leadLagWeight", _cfg.modules.signal_lead_lag_weight);
        
        // 阈值参数
        _cfg.modules.signal_strong_threshold = readDouble(signalAgg, "strongThreshold", _cfg.modules.signal_strong_threshold);
        _cfg.modules.signal_vol_threshold = readDouble(signalAgg, "volThreshold", _cfg.modules.signal_vol_threshold);
        _cfg.modules.signal_spread_threshold = readDouble(signalAgg, "spreadThreshold", _cfg.modules.signal_spread_threshold);
        _cfg.modules.signal_book_imbalance_threshold = readDouble(signalAgg, "bookImbalanceThreshold", _cfg.modules.signal_book_imbalance_threshold);
        _cfg.modules.signal_large_trade_threshold = readDouble(signalAgg, "largeTradeThreshold", _cfg.modules.signal_large_trade_threshold);
        _cfg.modules.signal_momentum_ema_alpha = readDouble(signalAgg, "momentumEmaAlpha", _cfg.modules.signal_momentum_ema_alpha);
    }
    

    WTSLogger::info("StrategyCoordinator: loaded config from variant (toxicity={}, perf={})",
        _cfg.use_toxicity_detector, _cfg.perf_enabled);
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
    tc.timestamp = tc.date * 1000000ULL + tc.time_hms * 100ULL + ctx->stra_get_secs();
    
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
    
    // Stage 8: Update adaptive parameters
    updateAdaptiveParams(ctx, tc);
    
    result.processed = true;
    _tick_count++;
    
    auto end_time = std::chrono::high_resolution_clock::now();
    result.processing_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        end_time - start_time).count();
    
    // Record to performance monitor
    if (_perf_monitor) {
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
    if (_cfg.closeout_minutes_before <= 0 || !_risk_monitor) {
        return false;
    }
    
    CloseoutState state = _risk_monitor->getCloseoutState();
    uint32_t closeTime = _cfg.close_time;
    
    switch (state)
    {
        case CloseoutState::IDLE:
        {
            // Check if closeout should be triggered
            bool triggered = _risk_monitor->checkCloseout(tc.time_hms, closeTime);
            if (triggered)
            {
                // Cancel all outstanding orders
                if (_quoters) {
                    for (auto& [code, quoter] : *_quoters) {
                        if (quoter) quoter->cancelAll(ctx);
                    }
                }
                
                // Execute closeout hedge if configured
                if (_cfg.closeout_flatten_position && _portfolio) {
                    _risk_monitor->markCloseoutFlattening(tc.time_hms * 100);
                    // Hedge execution is delegated to portfolio
                    // Portfolio handles the actual position flattening
                } else {
                    _risk_monitor->markCloseoutCompleted(tc.time_hms * 100);
                }
                return true;
            }
            return false;
        }
        
        case CloseoutState::TRIGGERED:
        case CloseoutState::FLATTENING:
        case CloseoutState::COMPLETED:
            // In closeout - skip normal tick processing
            return true;
            
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
    
    // Extract prices
    tc.bid_px = tick->bidprice(0);
    tc.ask_px = tick->askprice(0);
    
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
    
    // 真正的交易时段检查（修复休市期间报价问题）
    auto sess_it = _session_info.find(tc.code);
    if (sess_it != _session_info.end() && sess_it->second)
    {
        wtp::WTSSessionInfo* sessInfo = sess_it->second;
        uint32_t currentTime = ctx->stra_get_time();  // HHMMSS 格式
        tc.is_trading_session = sessInfo->isInTradingTime(currentTime / 100);  // 需要HHMM格式
        
        if (!tc.is_trading_session)
        {            WTSLogger::debug("StrategyCoordinator: {} not in trading session at {:06d}, skipping", 
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
        
        // P0-2.3: Refresh global portfolio context once per tick to avoid redundant calculations
        _global_portfolio_ctx.total_delta = _portfolio->getTotalDelta();
        _global_portfolio_ctx.total_exposure = _portfolio->getTotalExposure();
        _global_portfolio_ctx.related.clear();  // 仅清空相关合约列表
        _portfolio_ctx_dirty = false;
    }
    
    // Update toxicity detector
    if (_toxicity)
    {
        _toxicity->onTickVolume(tc.code.c_str(), tick);
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
    
    // 3. 更新市场状态暂停标志
    _market_state_paused = sig_ctx.shouldHedge();
    
    if (_market_state_paused && _quoters) {
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
        _trading_halted = true;
        return false;
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
                _trading_halted = true;
                _risk_monitor->haltTrading(category, _portfolio->getTotalPnL());
                if (_arb_executor) {
                    AsyncArbConfig arbCfg = _arb_executor->getConfig();
                    arbCfg.enabled = false;
                    _arb_executor->setConfig(arbCfg);
                    WTSLogger::error("StrategyCoordinator[{}]: Arbitrage executor disabled due to HALT_TRADING", tc.code);
                }
                break;

            case RiskAction::PAUSE_QUOTING:
                _quoting_paused = true;
                if (_arb_executor) {
                    AsyncArbConfig arbCfg = _arb_executor->getConfig();
                    arbCfg.enabled = false;
                    _arb_executor->setConfig(arbCfg);
                    WTSLogger::warn("StrategyCoordinator[{}]: Arbitrage executor disabled due to PAUSE_QUOTING", tc.code);
                }
                break;

            case RiskAction::BLOCK_SIDE_LONG:
                _long_blocked = true;
                break;

            case RiskAction::BLOCK_SIDE_SHORT:
                _short_blocked = true;
                break;

            default:
                break;
        }
        }
        else
        {
        // Auto-recovery check (if previously paused/blocked)
        if (_quoting_paused || _long_blocked || _short_blocked)
        {
            if (_risk_monitor->canRecover(_portfolio))
            {
                _quoting_paused = false;
                _long_blocked = false;
                _short_blocked = false;
                if (_arb_executor) {
                    AsyncArbConfig arbCfg = _arb_executor->getConfig();
                    arbCfg.enabled = true;
                    _arb_executor->setConfig(arbCfg);
                }
                WTSLogger::info("StrategyCoordinator[{}]: Risk normalized, resuming operations", tc.code);
            }
        }    }
    
    // Check toxicity cooldown
    if (_toxicity && tc.timestamp < _toxicity_resume_time) {
        _toxicity_paused = true;
        if (_self_trade_calibrator) {
            _self_trade_calibrator->decayCalibration(tc.code, tc.timestamp, _cfg.toxicity_cooloff_ms);
        }
    } else {
        _toxicity_paused = false;
    }
    
    return !_trading_halted;
}

//==========================================================================
// Stage 5: Process Quoting
//==========================================================================

bool StrategyCoordinator::processQuoting(
    wtp::IUftStraCtx* ctx, const TickContext& tc, wtp::WTSTickData* tick)
{
    if (_quoting_paused || _market_state_paused || _toxicity_paused) {
        return false;
    }
    
    if (!_quoters || !_portfolio) return false;
    
    auto it = _quoters->find(tc.code);
    if (it == _quoters->end() || !it->second) return false;
    
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
        
        // 注入全组合合约持仓，以便计算相关性 Skew
        for (const auto& rel_c : _portfolio->getAllContracts())
        {
            if (rel_c.code == tc.code) continue;
            
            // 从 CorrelationManager 获取实时的相关性和对冲比率 (Beta)
            double corr = 0.0;
            double hedge_ratio = rel_c.hedge_ratio;
            
            if (_correlation_manager)
            {
                auto stats = _correlation_manager->getCorrelation(tc.code, rel_c.code);
                corr = stats.correlation;
                // 如果 Beta 自动计算开启，使用实时的 Beta
                if (stats.sample_count > 10) {
                    hedge_ratio = stats.beta;
                }
            }
            
            p_ctx.addRelated(rel_c.code, rel_c.position, corr, hedge_ratio, rel_c.multiplier, rel_c.last_price);
        }
    }
    
    //==========================================================================
    // 3. 使用 SpreadOptimizer 计算动态 Skew 和价差倍数
    //==========================================================================
    double skew = 0.0;
    double spread_mult = 1.0;
    double l0_bid = tc.mid - 1.5 * tc.tick_size;
    double l0_ask = tc.mid + 1.5 * tc.tick_size;
    
    if (_spread_opts && _cfg.use_spread_optimizer && sig_ctx)
    {
        auto opt_it = _spread_opts->find(tc.code);
        if (opt_it != _spread_opts->end() && opt_it->second)
        {
            double inventory = _portfolio->getPosition(tc.code);
            
            // 获取自成交校准结果（如果有）
            CalibrationResult calib;
            const CalibrationResult* calib_ptr = nullptr;
            if (_self_trade_calibrator) {
                calib = _self_trade_calibrator->getCalibration(tc.code);
                if (calib.sample_size > 0) {
                    calib_ptr = &calib;
                }
            }
            
            // 调用统一的函数式计算接口 (整合 Alpha + 波动率 + 组合 Skew + Delta + 校准)
            GLFTResult res = opt_it->second->computeOptimalQuote(
                tc.mid, inventory, *sig_ctx, _cfg.modules.alpha_sensitivity, &p_ctx, calib_ptr);
                
            skew = res.inventory_skew;
            spread_mult = res.spread_mult;
            l0_bid = res.bid_price;
            l0_ask = res.ask_price;
        }
    }
    
    //==========================================================================
    // 3.5 毒性风控检查 (Toxicity Risk Control)
    //==========================================================================
    bool allow_bid = !_long_blocked;
    bool allow_ask = !_short_blocked;
    
    if (_cfg.use_toxicity_detector && _toxicity) {
        ToxicityMetrics tox = _toxicity->analyze();
        
        if (tox.is_toxic) {
            // 根据毒性方向暂停单边报价
            if (tox.toxic_side == 1) {
                // 买方毒性高（买后价格下跌），禁止买入
                allow_bid = false;
                WTSLogger::warn("[TOXIC] {} Buy-side toxic (score={:.2f}), pausing bid quotes",
                    tc.code, tox.toxic_score);
            } else if (tox.toxic_side == -1) {
                // 卖方毒性高（卖后价格上涨），禁止卖出
                allow_ask = false;
                WTSLogger::warn("[TOXIC] {} Sell-side toxic (score={:.2f}), pausing ask quotes",
                    tc.code, tox.toxic_score);
            } else {
                // 双边毒性（极端情况），暂停双边
                allow_bid = false;
                allow_ask = false;
                WTSLogger::warn("[TOXIC] {} Both-side toxic (score={:.2f}), pausing all quotes",
                    tc.code, tox.toxic_score);
            }
            
            // 毒性时扩大价差
            spread_mult = std::max(spread_mult, 1.0 + tox.toxic_score * 0.5);
        }
    }

            //==========================================================================
            // 4.1 应用风险价差倍数到 Level 0 (保证全档位同步拓宽)
            //==========================================================================
            double risk_spread_mult = _portfolio ? _portfolio->getSpreadMultiplierByRisk(3.0) : 1.0;
            if (risk_spread_mult > 1.0) {
                double current_half_spread = (l0_ask - l0_bid) / 2.0;
                double mid = (l0_ask + l0_bid) / 2.0;
                l0_bid = mid - current_half_spread * risk_spread_mult;
                l0_ask = mid + current_half_spread * risk_spread_mult;
                spread_mult *= risk_spread_mult;
            }

            //==========================================================================
            // 4.2 执行报价发布
            //==========================================================================
            it->second->refreshQuotes(ctx, tc.mid, l0_bid, l0_ask, spread_mult,
            allow_bid, allow_ask, tc.timestamp,
            tick->upperlimit(), tick->lowerlimit(), tick->bidprice(0), tick->askprice(0));

            return true;
            }
//==========================================================================
// Stage 6: Process Auto-cancel
//==========================================================================

bool StrategyCoordinator::processAutoCancel(wtp::IUftStraCtx* ctx, const TickContext& tc)
{
    if (!_cfg.use_auto_cancel || !_order_tracker) return false;
    
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
    if (!_portfolio) return false;
    // Hedge logic is delegated to portfolio
    return false;
}

//==========================================================================
// Stage 8: Update Adaptive Parameters
//==========================================================================

void StrategyCoordinator::updateAdaptiveParams(wtp::IUftStraCtx* ctx, const TickContext& tc)
{
    if (!_param_manager || _tick_count % _cfg.param_update_interval != 0) {
        return;
    }
    
    // Adaptive parameter update is handled by AdaptiveParamManager
    // This is a placeholder - actual implementation would record performance
    // and update parameters based on the manager's logic
}

//==========================================================================
// Reset
//==========================================================================


void StrategyCoordinator::resetSession()
{
    _trading_halted = false;
    _quoting_paused = false;
    _long_blocked = false;
    _short_blocked = false;
    _market_state_paused = false;
    _toxicity_paused = false;
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