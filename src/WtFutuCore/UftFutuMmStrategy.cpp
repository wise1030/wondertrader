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
 *   - MarketStateDetector: 市场状态检测
 *   - AutoCancelPolicy: 自动撤单策略
 *   - FutuRiskMonitor: 风险监控
 *   - Level2DataAdapter: Level2数据适配
 *   - AdaptiveParamManager: 自适应参数调优
 *   - PerformanceAnalyzer: 绩效分析
 */
#include "UftFutuMmStrategy.h"
#include "../Includes/IUftStraCtx.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSMarcos.h"
#include "../WTSTools/WTSLogger.h"
#include "../Includes/WTSSessionInfo.hpp"

// 业务模块头文件
#include "FutuPortfolio.h"
#include "FutuQuoter.h"
#include "SpreadOptimizer.h"
#include "CorrelationManager.h"
#include "MarketStateDetector.h"
#include "AutoCancelPolicy.h"
#include "OrderBookAnalyzer.h"
#include "FutuRiskMonitor.h"
#include "MicroAlphaEngine.h"
#include "ToxicFlowDetector.h"
#include "AdaptiveParamManager.h"
#include "PerformanceAnalyzer.h"
#include "PerformanceMonitor.h"
#include "SpreadArbitrageManager.h"
#include "SelfTradePrevention.h"
#include "AsyncArbitrageExecutor.h"

// 综合信号组件头文件
#include "TickTransactionInferer.h"
#include "SelfTradeCalibrator.h"
#include "SyntheticSignalFusion.h"

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

} // anonymous namespace

//==========================================================================
// 构造/析构
//==========================================================================

UftFutuMmStrategy::UftFutuMmStrategy(const char* id)
    : UftStrategy(id)
    , _channel_ready(false)
    , _trading_halted(false)
    , _toxicity_paused(false)
    , _toxicity_resume_time(0)
    , _long_blocked(false)
    , _short_blocked(false)
    , _quoting_paused(false)
    , _order_error_count(0)
    , _order_error_threshold(3)    // 连续3次错误暂停
    , _closeout_triggered(false)
    , _closeout_completed(false)
    , _closeout_start_time(0)
    , _tick_count(0)
    , _param_update_interval(100)
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
    
    //------------------------------------------------------------
    // 读取合约配置
    //------------------------------------------------------------
    _config.anchor_code = cfg->getCString("anchorCode");
    
    // 读取合约列表
    // multiplier 和 tickSize 为可选参数，如未配置则从基础数据管理模块自动获取
    WTSVariant* cfgContracts = cfg->get("contracts");
    if (cfgContracts && cfgContracts->type() == WTSVariant::VT_Array)
    {
        for (uint32_t i = 0; i < cfgContracts->size(); i++)
        {
            WTSVariant* cfgItem = cfgContracts->get(i);
            
            ContractInfo ci;
            ci.code = cfgItem->getCString("code");
            // multiplier 和 tickSize 可选，-1 表示未配置，后续从基础数据模块获取
            ci.multiplier = readDouble(cfgItem, "multiplier", -1.0);
            ci.tick_size = readDouble(cfgItem, "tickSize", -1.0);
            // 单合约限制，-1 表示未配置，使用全局默认值
            ci.max_position = readDouble(cfgItem, "maxPosition", -1.0);
            ci.max_delta = readDouble(cfgItem, "maxDelta", -1.0);
            
            _contract_infos.push_back(ci);
            _config.codes.push_back(ci.code);
        }
    }
    
    //------------------------------------------------------------
    // 读取 Delta 风控参数
    //------------------------------------------------------------
    _config.delta_limit = readDouble(cfg, "deltaLimit", 50.0);
    _config.hedge_threshold = readDouble(cfg, "hedgeThreshold", 30.0);
    _config.max_spread_mult = readDouble(cfg, "maxSpreadMult", 3.0);
    
    //------------------------------------------------------------
    // 读取报价参数
    //------------------------------------------------------------
    _config.num_levels = readUInt32(cfg, "numLevels", 3);
    _config.base_spread = readDouble(cfg, "baseSpread", 2.0);
    _config.base_qty = readDouble(cfg, "baseQty", 1.0);
    _config.qty_decay = readDouble(cfg, "qtyDecay", 0.7);
    _config.level_step = readDouble(cfg, "levelStep", 1.0);
    
    //------------------------------------------------------------
    // 读取库存管理参数
    //------------------------------------------------------------
    _config.max_inventory = readDouble(cfg, "maxInventory", 100.0);
    _config.skew_factor = readDouble(cfg, "skewFactor", 0.001);
    _config.max_skew = readDouble(cfg, "maxSkew", 5.0);
    _config.hedge_ratio = readDouble(cfg, "hedgeRatio", 0.5);
    _config.target_inventory = readDouble(cfg, "targetInventory", 0);
    _config.max_delta = readDouble(cfg, "maxDelta", 100.0);
    _config.max_exposure = readDouble(cfg, "maxExposure", 300.0);
    
    // Sticky 策略参数
    _config.sticky_threshold = readDouble(cfg, "stickyThreshold", 1.0);
    
    // 增强 Skew 参数
    _config.skew_sensitivity = readDouble(cfg, "skewSensitivity", 2.0);
    _config.aggressive_skew_threshold = readDouble(cfg, "aggressiveSkewThreshold", 0.5);
    _config.one_sided_threshold = readDouble(cfg, "oneSidedThreshold", 0.8);
    
    // 下单错误处理参数（统一处理所有下单错误）
    _config.order_error_threshold = readUInt32(cfg, "orderErrorThreshold", 3);
    
    // 收盘前平仓参数（收盘时间从基础数据获取）
    _config.closeout_minutes_before = readUInt32(cfg, "closeoutMinutesBefore", 5);
    _config.closeout_flatten_position = readBool(cfg, "closeoutFlattenPosition", true);
    
    //------------------------------------------------------------
    // 读取风控参数
    //------------------------------------------------------------
    _config.max_daily_loss = readDouble(cfg, "maxDailyLoss", -50000.0);
    
    //------------------------------------------------------------
    // 读取高级模块开关
    //------------------------------------------------------------
    _config.use_spread_optimizer = readBool(cfg, "useSpreadOptimizer", true);
    _config.use_auto_cancel = readBool(cfg, "useAutoCancel", true);
    _config.use_market_state = readBool(cfg, "useMarketState", true);
    _config.use_order_book = readBool(cfg, "useOrderBook", false);
    _config.use_alpha_engine = readBool(cfg, "useAlphaEngine", true);
    _config.use_toxicity_detector = readBool(cfg, "useToxicityDetector", true);
    _config.use_adaptive_param = readBool(cfg, "useAdaptiveParam", false);
    _config.use_performance_monitor = readBool(cfg, "usePerformanceMonitor", false);
    _config.use_performance_analyzer = readBool(cfg, "usePerformanceAnalyzer", false);
    
    //------------------------------------------------------------
    // 读取 SpreadOptimizer 参数
    //------------------------------------------------------------
    WTSVariant* cfgSpread = cfg->get("spreadOptimizer");
    if (cfgSpread)
    {
        _config.spread_vol_sensitivity = readDouble(cfgSpread, "volSensitivity", 1.0);
        _config.spread_depth_sensitivity = readDouble(cfgSpread, "depthSensitivity", 0.5);
        _config.spread_vol_window = readUInt32(cfgSpread, "volWindow", 100);
        _config.spread_min_mult = readDouble(cfgSpread, "minSpreadMult", 0.5);
        _config.spread_phi = readDouble(cfgSpread, "phi", 0.01);
        _config.spread_portfolio_skew_weight = readDouble(cfgSpread, "portfolioSkewWeight", 0.5);
        _config.spread_min_correlation = readDouble(cfgSpread, "minCorrelation", 0.5);
    }
    
    //------------------------------------------------------------
    // 读取 AlphaEngine 参数
    //------------------------------------------------------------
    WTSVariant* cfgAlpha = cfg->get("alphaEngine");
    if (cfgAlpha)
    {
        _config.alpha_sensitivity = readDouble(cfgAlpha, "sensitivity", 0.5);
        _config.alpha_ofi_weight = readDouble(cfgAlpha, "ofiWeight", 0.4);
        _config.alpha_trade_weight = readDouble(cfgAlpha, "tradeWeight", 0.3);
        _config.alpha_leadlag_weight = readDouble(cfgAlpha, "leadlagWeight", 0.3);
        _config.alpha_ema_factor = readDouble(cfgAlpha, "emaFactor", 0.3);
        _config.alpha_strong_threshold = readDouble(cfgAlpha, "strongThreshold", 0.7);
    }
    
    //------------------------------------------------------------
    // 读取 ToxicityDetector 参数
    //------------------------------------------------------------
    WTSVariant* cfgToxicity = cfg->get("toxicityDetector");
    if (cfgToxicity)
    {
        _config.toxicity_vpin_threshold = readDouble(cfgToxicity, "vpinThreshold", 0.7);
        _config.toxicity_window = readUInt32(cfgToxicity, "window", 50);
        _config.toxicity_cooloff_ms = readUInt32(cfgToxicity, "cooloffMs", 5000);
    }
    
    //------------------------------------------------------------
    // 读取 MarketStateDetector 参数
    //------------------------------------------------------------
    WTSVariant* cfgMarket = cfg->get("marketState");
    if (cfgMarket)
    {
        _config.market_vol_threshold = readDouble(cfgMarket, "volThreshold", 0.003);
        _config.market_move_threshold = readDouble(cfgMarket, "moveThreshold", 0.005);
        _config.market_spread_threshold = readDouble(cfgMarket, "spreadThreshold", 5.0);
        _config.market_volume_threshold = readDouble(cfgMarket, "volumeThreshold", 10.0);
        _config.market_lookback_ticks = readUInt32(cfgMarket, "lookbackTicks", 50);
        _config.market_cooldown_ticks = readUInt32(cfgMarket, "cooldownTicks", 20);
    }
    
    //------------------------------------------------------------
    // 读取 AutoCancelPolicy 参数
    //------------------------------------------------------------
    WTSVariant* cfgCancel = cfg->get("autoCancel");
    if (cfgCancel)
    {
        _config.cancel_max_age_ms = readUInt32(cfgCancel, "maxAgeMs", 5000);
        _config.cancel_price_deviation = readDouble(cfgCancel, "priceDeviation", 3.0);
        _config.cancel_on_state_change = readBool(cfgCancel, "cancelOnStateChange", true);
        _config.cancel_on_inventory_limit = readBool(cfgCancel, "cancelOnInventoryLimit", true);
        _config.cancel_inventory_limit_cooldown_ms = readUInt32(cfgCancel, "inventoryLimitCooldownMs", 2000);
    }
    
    //------------------------------------------------------------
    // 读取 FutuRiskMonitor 参数
    //------------------------------------------------------------
    WTSVariant* cfgRisk = cfg->get("riskMonitor");
    if (cfgRisk)
    {
        _config.risk_max_orders_per_sec = readUInt32(cfgRisk, "maxOrdersPerSec", 50);
        _config.risk_max_cancels_per_sec = readUInt32(cfgRisk, "maxCancelsPerSec", 30);
        _config.risk_max_trades_per_sec = readUInt32(cfgRisk, "maxTradesPerSec", 20);
        
        // 恢复机制参数
        _config.risk_cooldown_ms = readUInt32(cfgRisk, "cooldownMs", 30000);
        _config.risk_check_interval_ms = readUInt32(cfgRisk, "checkIntervalMs", 5000);
        _config.risk_recovery_threshold = readDouble(cfgRisk, "recoveryThreshold", 0.8);
    }
    
    //------------------------------------------------------------
    // 读取 CorrelationManager 参数
    //------------------------------------------------------------
    WTSVariant* cfgCorrelation = cfg->get("correlationManager");
    if (cfgCorrelation)
    {
        _config.correlation_window_size = readUInt32(cfgCorrelation, "windowSize", 100);
        _config.correlation_min_correlation = readDouble(cfgCorrelation, "minCorrelation", 0.5);
        _config.correlation_spread_z_threshold = readDouble(cfgCorrelation, "spreadZThreshold", 2.0);
    }
    
    //------------------------------------------------------------
    // 读取 AdaptiveParamManager 参数
    //------------------------------------------------------------
    WTSVariant* cfgAdaptive = cfg->get("adaptiveParam");
    if (cfgAdaptive)
    {
        _config.adaptive_update_interval = readUInt32(cfgAdaptive, "updateInterval", 100);
        _config.adaptive_learning_rate = readDouble(cfgAdaptive, "learningRate", 0.01);
        _config.adaptive_min_phi = readDouble(cfgAdaptive, "minPhi", 0.001);
        _config.adaptive_max_phi = readDouble(cfgAdaptive, "maxPhi", 0.1);
    }
    
    //------------------------------------------------------------
    // 读取 PerformanceAnalyzer 参数
    //------------------------------------------------------------
    WTSVariant* cfgPerfAnalyzer = cfg->get("performanceAnalyzer");
    if (cfgPerfAnalyzer)
    {
        _config.perf_analyzer_window_size = readUInt32(cfgPerfAnalyzer, "windowSize", 100);
        _config.perf_analyzer_risk_free_rate = readDouble(cfgPerfAnalyzer, "sharpeRiskFree", 0.0);
    }
    
    //------------------------------------------------------------
    // 读取 PerformanceMonitor 参数
    //------------------------------------------------------------
    WTSVariant* cfgPerfMonitor = cfg->get("performanceMonitor");
    if (cfgPerfMonitor)
    {
        _config.perf_monitor_latency_threshold = (uint64_t)readDouble(cfgPerfMonitor, "latencyThreshold", 100000);
    }
    
    //------------------------------------------------------------
    // 读取 SpreadArbitrage 参数
    //------------------------------------------------------------
    WTSVariant* cfgSpreadArb = cfg->get("spreadArbitrage");
    if (cfgSpreadArb)
    {
        _config.use_spread_arbitrage = readBool(cfgSpreadArb, "enabled", false);
        _config.spread_arb_enhance_mm = readBool(cfgSpreadArb, "enhanceMarketMaking", true);
        _config.spread_arb_max_position = readDouble(cfgSpreadArb, "maxPosition", 20.0);
        _config.spread_arb_entry_z = readDouble(cfgSpreadArb, "entryZScore", 2.0);
        _config.spread_arb_exit_z = readDouble(cfgSpreadArb, "exitZScore", 0.5);
        _config.spread_arb_window = readUInt32(cfgSpreadArb, "windowSize", 200);
    }
    
    //------------------------------------------------------------
    // 参数边界校验（不影响运行时延迟）
    //------------------------------------------------------------
    {
        // Delta 相关参数校验 - 放宽范围支持大资金场景
        if (_config.delta_limit <= 0 || _config.delta_limit > 100000000) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid deltaLimit: {}, expected (0, 100000000]", id(), _config.delta_limit);
            return false;
        }
        if (_config.hedge_threshold <= 0 || _config.hedge_threshold > _config.delta_limit) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid hedgeThreshold: {}, expected (0, deltaLimit={}]", id(), _config.hedge_threshold, _config.delta_limit);
            return false;
        }
        if (_config.max_delta < _config.delta_limit) {
            WTSLogger::warn("UftFutuMmStrategy[{}] maxDelta={} < deltaLimit={}, auto adjusted", id(), _config.max_delta, _config.delta_limit);
            _config.max_delta = _config.delta_limit * 2;
        }
        
        // 报价参数校验
        if (_config.num_levels == 0 || _config.num_levels > 10) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid numLevels: {}, expected [1, 10]", id(), _config.num_levels);
            return false;
        }
        if (_config.base_spread <= 0 || _config.base_spread > 20) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid baseSpread: {}, expected (0, 20]", id(), _config.base_spread);
            return false;
        }
        if (_config.base_qty <= 0 || _config.base_qty > 100) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid baseQty: {}, expected (0, 100]", id(), _config.base_qty);
            return false;
        }
        if (_config.qty_decay < 0.1 || _config.qty_decay > 1.0) {
            WTSLogger::warn("UftFutuMmStrategy[{}] qtyDecay={} out of typical range [0.1, 1.0]", id(), _config.qty_decay);
        }
        
        // 库存参数校验
        if (_config.skew_factor <= 0 || _config.skew_factor > 0.1) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid skewFactor: {}, expected (0, 0.1]", id(), _config.skew_factor);
            return false;
        }
        if (_config.max_skew <= 0 || _config.max_skew > 50) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid maxSkew: {}, expected (0, 50]", id(), _config.max_skew);
            return false;
        }
        if (_config.hedge_ratio < 0 || _config.hedge_ratio > 1.0) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid hedgeRatio: {}, expected [0, 1]", id(), _config.hedge_ratio);
            return false;
        }
        
        // Alpha 参数校验
        double alphaWeightSum = _config.alpha_ofi_weight + _config.alpha_trade_weight + _config.alpha_leadlag_weight;
        if (alphaWeightSum < 0.9 || alphaWeightSum > 1.1) {
            WTSLogger::warn("UftFutuMmStrategy[{}] alpha weights sum={} != 1.0, auto normalizing", id(), alphaWeightSum);
            _config.alpha_ofi_weight /= alphaWeightSum;
            _config.alpha_trade_weight /= alphaWeightSum;
            _config.alpha_leadlag_weight /= alphaWeightSum;
        }
        if (_config.alpha_sensitivity < 0 || _config.alpha_sensitivity > 2.0) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid alphaSensitivity: {}, expected [0, 2]", id(), _config.alpha_sensitivity);
            return false;
        }
        
        // 价差套利参数校验
        if (_config.use_spread_arbitrage) {
            if (_config.spread_arb_entry_z <= _config.spread_arb_exit_z) {
                WTSLogger::error("UftFutuMmStrategy[{}] entryZ={} <= exitZ={}, invalid", id(), _config.spread_arb_entry_z, _config.spread_arb_exit_z);
                return false;
            }
            if (_config.spread_arb_window < 50) {
                WTSLogger::warn("UftFutuMmStrategy[{}] spreadArb window={} < 50, may be unstable", id(), _config.spread_arb_window);
            }
        }
        
        // 流控参数校验
        if (_config.risk_max_orders_per_sec == 0 || _config.risk_max_orders_per_sec > 500) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid maxOrdersPerSec: {}, expected [1, 500]", id(), _config.risk_max_orders_per_sec);
            return false;
        }
        
        WTSLogger::info("UftFutuMmStrategy[{}] parameter validation passed", id());
    }
    
    // 注意：业务模块初始化移到 on_init 中，以便从基础数据管理模块获取合约参数
    
    return true;
}

//==========================================================================
// 业务模块初始化
//==========================================================================

void UftFutuMmStrategy::initBusinessModules()
{
    //------------------------------------------------------------
    // 1. FutuPortfolio（持仓管理）
    //------------------------------------------------------------
    _portfolio = std::make_unique<FutuPortfolio>();
    
    PortfolioParams portfolio_params;
    portfolio_params.skew_factor = _config.skew_factor;
    portfolio_params.max_skew = _config.max_skew;
    portfolio_params.delta_limit = _config.delta_limit;
    portfolio_params.hedge_ratio = _config.hedge_ratio;
    portfolio_params.target_inventory = _config.target_inventory;
    portfolio_params.max_delta = _config.max_delta;
    portfolio_params.max_exposure = _config.max_exposure;
    portfolio_params.max_loss = -_config.max_daily_loss;
    // 增强 skew 参数
    portfolio_params.skew_sensitivity = _config.skew_sensitivity;
    portfolio_params.aggressive_skew_threshold = _config.aggressive_skew_threshold;
    portfolio_params.one_sided_threshold = _config.one_sided_threshold;
    _portfolio->setParams(portfolio_params);
    _portfolio->setAnchorContract(_config.anchor_code);
    
    // 添加合约到 Portfolio（包含单合约限制）
    for (const auto& ci : _contract_infos)
    {
        double max_pos = (ci.max_position > 0) ? ci.max_position : _config.max_inventory;
        double max_del = (ci.max_delta > 0) ? ci.max_delta : 0;  // 0 表示无限制
        _portfolio->addContract(ci.code, ci.multiplier, ci.tick_size, 1.0, max_pos, max_del);
    }
    
    WTSLogger::info("  FutuPortfolio: maxDelta={}, maxExposure={}, maxLoss={}",
        portfolio_params.max_delta, portfolio_params.max_exposure, portfolio_params.max_loss);
    
    //------------------------------------------------------------
    // 2. FutuQuoter（报价引擎）- 每合约一个
    //------------------------------------------------------------
    for (const auto& ci : _contract_infos)
    {
        auto quoter = std::make_unique<FutuQuoter>();
        
        QuoterConfig qcfg;
        qcfg.code = ci.code;
        qcfg.num_levels = _config.num_levels;
        qcfg.base_spread = _config.base_spread;
        qcfg.level_step = _config.level_step;
        qcfg.base_qty = _config.base_qty;
        qcfg.qty_decay = _config.qty_decay;
        qcfg.tick_size = ci.tick_size;
        qcfg.sticky_threshold = _config.sticky_threshold;
        
        quoter->init(qcfg);
        _quoters.emplace_back(ci.code, std::move(quoter));
    }
    
    WTSLogger::info("  FutuQuoter: {} quoters initialized, {} levels, baseSpread={}",
        _quoters.size(), _config.num_levels, _config.base_spread);
    
    //------------------------------------------------------------
    // 3. SpreadOptimizer（价差优化器）- 每合约一个
    //------------------------------------------------------------
    if (_config.use_spread_optimizer)
    {
        for (const auto& ci : _contract_infos)
        {
            auto optimizer = std::make_unique<SpreadOptimizer>();
            
            GLFTParams spread_params;
            spread_params.base_spread = _config.base_spread;
            spread_params.tick_size = ci.tick_size;
            spread_params.vol_sensitivity = _config.spread_vol_sensitivity;
            spread_params.depth_sensitivity = _config.spread_depth_sensitivity;
            spread_params.max_spread_mult = _config.max_spread_mult;
            spread_params.min_spread_mult = _config.spread_min_mult;
            spread_params.vol_window = _config.spread_vol_window;
            spread_params.phi = _config.spread_phi;
            spread_params.portfolio_skew_weight = _config.spread_portfolio_skew_weight;
            spread_params.min_correlation = _config.correlation_min_correlation;
            
            optimizer->setParams(spread_params);
            optimizer->setContract(ci.code);
            
            // 设置统一的增强 skew 参数
            optimizer->setSkewEnhancement(_config.skew_sensitivity, _config.aggressive_skew_threshold);
            
            _spread_optimizers.emplace_back(ci.code, std::move(optimizer));
        }
        
        WTSLogger::info("  SpreadOptimizer: {} optimizers, volWindow={}, phi={}, skewSensitivity={}",
            _spread_optimizers.size(), _config.spread_vol_window, _config.spread_phi, _config.skew_sensitivity);
    }
    
    //------------------------------------------------------------
    // 4. MarketStateDetector（市场状态检测）
    //------------------------------------------------------------
    if (_config.use_market_state)
    {
        DetectionParams detect_params;
        detect_params.vol_threshold = _config.market_vol_threshold;
        detect_params.move_threshold = _config.market_move_threshold;
        detect_params.spread_threshold = _config.market_spread_threshold;
        detect_params.volume_threshold = _config.market_volume_threshold;
        detect_params.lookback_ticks = _config.market_lookback_ticks;
        detect_params.cooldown_ticks = _config.market_cooldown_ticks;
        
        for (const auto& ci : _contract_infos)
        {
            auto detector = std::make_unique<MarketStateDetector>();
            detector->setParams(detect_params);
            _market_states.emplace(ci.code, std::move(detector));
        }
        
        WTSLogger::info("  MarketStateDetector: volThreshold={}, moveThreshold={}",
            _config.market_vol_threshold, _config.market_move_threshold);
    }
    
    //------------------------------------------------------------
    // 5. AutoCancelPolicy（自动撤单）
    //------------------------------------------------------------
    if (_config.use_auto_cancel)
    {
        _auto_cancel = std::make_unique<AutoCancelPolicy>();
        
        CancelParams cancel_params;
        cancel_params.max_age_ms = _config.cancel_max_age_ms;
        cancel_params.price_deviation = _config.cancel_price_deviation;
        cancel_params.cancel_on_state_change = _config.cancel_on_state_change;
        cancel_params.cancel_on_inventory_limit = _config.cancel_on_inventory_limit;
        cancel_params.sticky_threshold = _config.sticky_threshold;
        cancel_params.inventory_limit_cooldown_ms = _config.cancel_inventory_limit_cooldown_ms;
        
        _auto_cancel->setParams(cancel_params);
        
        WTSLogger::info("  AutoCancelPolicy: maxAgeMs={}, priceDeviation={}, stickyThreshold={}, invLimitCooldownMs={}",
            _config.cancel_max_age_ms, _config.cancel_price_deviation, _config.sticky_threshold,
            _config.cancel_inventory_limit_cooldown_ms);
    }
    
    //------------------------------------------------------------
    // 6. FutuRiskMonitor（风险监控）
    //------------------------------------------------------------
    _risk_monitor = std::make_unique<FutuRiskMonitor>();
    
    RateLimits rate_limits;
    rate_limits.max_orders_per_sec = _config.risk_max_orders_per_sec;
    rate_limits.max_cancels_per_sec = _config.risk_max_cancels_per_sec;
    rate_limits.max_trades_per_sec = _config.risk_max_trades_per_sec;
    _risk_monitor->setRateLimits(rate_limits);
    
    // 设置恢复配置
    RecoveryConfig recovery_cfg;
    recovery_cfg.cooldown_ms = _config.risk_cooldown_ms;
    recovery_cfg.check_interval_ms = _config.risk_check_interval_ms;
    recovery_cfg.recovery_threshold = _config.risk_recovery_threshold;
    _risk_monitor->setRecoveryConfig(recovery_cfg);
    
    WTSLogger::info("  FutuRiskMonitor: maxOrdersPerSec={}, maxCancelsPerSec={}, cooldownMs={}, recoveryThreshold={}",
        _config.risk_max_orders_per_sec, _config.risk_max_cancels_per_sec,
        _config.risk_cooldown_ms, _config.risk_recovery_threshold);
    
    //------------------------------------------------------------
    // 7. OrderBookAnalyzer（订单簿分析）
    //------------------------------------------------------------
    if (_config.use_order_book)
    {
        for (const auto& ci : _contract_infos)
        {
            _order_books.emplace(ci.code, std::make_unique<OrderBookAnalyzer>());
        }
        WTSLogger::info("  OrderBookAnalyzer: enabled");
    }
    
    //------------------------------------------------------------
    // 8. MicroAlphaEngine（微结构Alpha预测引擎）- GLFT+Alpha
    //------------------------------------------------------------
    for (const auto& ci : _contract_infos)
    {
        auto engine = std::make_unique<MicroAlphaEngine>();
        
        AlphaConfig alpha_cfg;
        alpha_cfg.ofi_window = 50;
        alpha_cfg.trade_window = 100;
        alpha_cfg.ema_alpha = _config.alpha_ema_factor;
        alpha_cfg.ofi_weight = _config.alpha_ofi_weight;
        alpha_cfg.trade_weight = _config.alpha_trade_weight;
        alpha_cfg.lead_lag_weight = _config.alpha_leadlag_weight;
        alpha_cfg.strong_alpha_threshold = _config.alpha_strong_threshold;
        engine->setConfig(alpha_cfg);
        
        _alpha_engines.emplace_back(ci.code, std::move(engine));
    }
    WTSLogger::info("  MicroAlphaEngine: {} engines initialized (GLFT+Alpha), eta={}",
        _alpha_engines.size(), _config.alpha_sensitivity);
    
    //------------------------------------------------------------
    // 9. ToxicFlowDetector（毒性流动检测器）
    //------------------------------------------------------------
    if (_config.use_toxicity_detector)
    {
        _toxicity_detector = std::make_unique<ToxicFlowDetector>();
        ToxicityParams toxic_params;
        toxic_params.adverse_threshold = _config.toxicity_vpin_threshold;
        // Note: lookback_trades, price_follow_window, record_expiry_secs removed
        // ToxicityParams now uses SelfTradeCalibrator for realized toxicity
        
        _toxicity_detector->setParams(toxic_params);
        WTSLogger::info("  ToxicFlowDetector: adverseThreshold={}", 
            _config.toxicity_vpin_threshold);
    }
    else
    {
        WTSLogger::info("  ToxicFlowDetector: disabled");
    }
    
    //------------------------------------------------------------
    // 9.5 SelfTradeCalibrator（统一管理自身成交，供毒性检测和综合信号使用）
    //------------------------------------------------------------
    _self_trade_calibrator = std::make_unique<SelfTradeCalibrator>();
    SelfTradeCalibratorConfig calib_cfg;
    calib_cfg.lookback_trades = 50;
    calib_cfg.toxicity_window_ms = 5000;
    calib_cfg.adverse_threshold = 0.6;
    calib_cfg.min_samples = _config.synthetic_min_samples;
    _self_trade_calibrator->setConfig(calib_cfg);
    
    // 将校准器设置到毒性检测器（统一使用 SelfTradeCalibrator 管理 Fill 记录）
    if (_toxicity_detector)
    {
        _toxicity_detector->setSelfTradeCalibrator(_self_trade_calibrator.get());
    }
    WTSLogger::info("  SelfTradeCalibrator: lookbackTrades={}, toxicityWindow={}ms",
        calib_cfg.lookback_trades, calib_cfg.toxicity_window_ms);
    
    //------------------------------------------------------------
    // 9.6 综合信号组件 (Synthetic Transaction for markets without L2)
    //------------------------------------------------------------
    if (_config.use_synthetic_transaction)
    {
        // 9.6.1 TickTransactionInferer - 每合约一个
        for (const auto& ci : _contract_infos)
        {
            auto inferer = std::make_unique<TickTransactionInferer>();
            
            TickInfererConfig inferer_cfg;
            inferer_cfg.tick_size = ci.tick_size;
            inferer_cfg.imbalance_window_ms = 5000;
            inferer_cfg.min_confidence = 0.3;
            inferer_cfg.large_trade_threshold = 50.0;
            inferer->setConfig(inferer_cfg);
            inferer->setContract(ci.code);
            
            _tick_inferers.emplace(ci.code, std::move(inferer));
        }
        
        // 9.6.2 SyntheticSignalFusion - 每合约一个
        for (const auto& ci : _contract_infos)
        {
            auto fusion = std::make_unique<SyntheticSignalFusion>();
            
            FusionConfig fusion_cfg;
            fusion_cfg.tick_inference_base_weight = _config.synthetic_tick_weight;
            fusion_cfg.book_signal_base_weight = _config.synthetic_book_weight;
            fusion_cfg.self_trade_base_weight = _config.synthetic_self_trade_weight;
            fusion_cfg.min_self_trade_samples = _config.synthetic_min_samples;
            fusion_cfg.adaptive_weights = true;
            fusion->setConfig(fusion_cfg);
            fusion->setContract(ci.code);
            
            _signal_fusions.emplace(ci.code, std::move(fusion));
        }
        
        WTSLogger::info("  SyntheticTransaction: {} inferers, {} fusions, weights=({:.1f},{:.1f},{:.1f})",
            _tick_inferers.size(), _signal_fusions.size(),
            _config.synthetic_tick_weight, _config.synthetic_book_weight, 
            _config.synthetic_self_trade_weight);
        
        // 设置 Alpha 引擎使用综合信号数据源
        for (auto& [code, engine] : _alpha_engines)
        {
            engine->setTradeImbalanceSource(TradeImbalanceSource::SYNTHETIC);
        }
    }
    else
    {
        WTSLogger::info("  SyntheticTransaction: disabled (using real L2 data if available)");
    }
    
    //------------------------------------------------------------
    // 10. AdaptiveParamManager（自适应参数管理器）
    //------------------------------------------------------------
    if (_config.use_adaptive_param)
    {
        _param_manager = std::make_unique<AdaptiveParamManager>();
        ParamBounds bounds;
        bounds.min_val = 0.5;
        bounds.max_val = 3.0;
        bounds.step = 0.1;
        bounds.current = 1.0;
        _param_manager->registerParam(ParamType::SPREAD_BASE, bounds);
        bounds.min_val = _config.adaptive_min_phi;
        bounds.max_val = _config.adaptive_max_phi;
        bounds.step = 0.01;
        bounds.current = _config.spread_phi;
        _param_manager->registerParam(ParamType::SPREAD_PHI, bounds);
        _param_update_interval = _config.adaptive_update_interval;
        WTSLogger::info("  AdaptiveParamManager: updateInterval={}, minPhi={}, maxPhi={}",
            _config.adaptive_update_interval, _config.adaptive_min_phi, _config.adaptive_max_phi);
    }
    else
    {
        WTSLogger::info("  AdaptiveParamManager: disabled");
    }
    
    //------------------------------------------------------------
    // 11. PerformanceAnalyzer（绩效分析器）
    //------------------------------------------------------------
    if (_config.use_performance_analyzer)
    {
        _perf_analyzer = std::make_unique<PerformanceAnalyzer>();
        AnalyzerConfig analyzer_cfg;
        analyzer_cfg.history_size = _config.perf_analyzer_window_size;
        _perf_analyzer->setConfig(analyzer_cfg);
        WTSLogger::info("  PerformanceAnalyzer: windowSize={}", _config.perf_analyzer_window_size);
    }
    else
    {
        WTSLogger::info("  PerformanceAnalyzer: disabled");
    }
    
    //------------------------------------------------------------
    // 12. PerformanceMonitor（性能监控）
    //------------------------------------------------------------
    if (_config.use_performance_monitor)
    {
        _performance_monitor = std::make_unique<PerformanceMonitor>();
        WTSLogger::info("  PerformanceMonitor: latencyThreshold={}ns", _config.perf_monitor_latency_threshold);
    }
    else
    {
        WTSLogger::info("  PerformanceMonitor: disabled");
    }
    
    //------------------------------------------------------------
    // 13. CorrelationManager（跨合约相关性管理器）
    //------------------------------------------------------------
    _correlation_manager = std::make_unique<CorrelationManager>();
    CorrelationConfig corr_cfg;
    corr_cfg.window_size = _config.correlation_window_size;
    corr_cfg.min_correlation = _config.correlation_min_correlation;
    corr_cfg.spread_z_threshold = _config.correlation_spread_z_threshold;
    _correlation_manager->setConfig(corr_cfg);
    
    // 添加所有合约到相关性管理器
    for (const auto& ci : _contract_infos)
    {
        _correlation_manager->addContract(ci.code, ci.multiplier);
    }
    WTSLogger::info("  CorrelationManager: {} contracts tracked, windowSize={}", 
        _contract_infos.size(), _config.correlation_window_size);
    
    //------------------------------------------------------------
    // 14. SpreadArbitrageManager（跨期价差套利管理器）
    //------------------------------------------------------------
    if (_config.use_spread_arbitrage)
    {
        _spread_arb_manager = std::make_unique<SpreadArbitrageManager>();
        
        SpreadArbitrageConfig arb_cfg;
        arb_cfg.enabled = true;
        arb_cfg.enhance_market_making = _config.spread_arb_enhance_mm;
        arb_cfg.max_total_position = _config.spread_arb_max_position;
        _spread_arb_manager->setConfig(arb_cfg);
        
        // 配置价差计算器
        SpreadCalculatorConfig calc_cfg;
        calc_cfg.window_size = _config.spread_arb_window;
        _spread_arb_manager->setCalculatorConfig(calc_cfg);
        
        // 配置风险管理
        SpreadRiskConfig risk_cfg;
        risk_cfg.max_total_position = _config.spread_arb_max_position;
        _spread_arb_manager->setRiskConfig(risk_cfg);
        
        WTSLogger::info("  SpreadArbitrageManager: enabled, enhanceMM={}, maxPos={}",
            _config.spread_arb_enhance_mm, _config.spread_arb_max_position);
    }
    else
    {
        WTSLogger::info("  SpreadArbitrageManager: disabled");
    }
    
    //------------------------------------------------------------
    // 15. SelfTradePrevention（自成交防护模块）
    //------------------------------------------------------------
    _stp = std::make_unique<SelfTradePrevention>();
    
    StpConfig stp_cfg;
    stp_cfg.enabled = true;
    stp_cfg.strategy = StpConfig::Strategy::CANCEL_MM;  // 默认策略：先撤做市单
    stp_cfg.min_price_gap = 1.0;  // 最小价格间隔（tick）
    _stp->setConfig(stp_cfg);
    
    WTSLogger::info("  SelfTradePrevention: enabled, strategy=CANCEL_MM");
    
    //------------------------------------------------------------
    // 16. AsyncArbitrageExecutor（异步套利执行器）
    //------------------------------------------------------------
    if (_config.use_spread_arbitrage)
    {
        _async_arb = std::make_unique<AsyncArbitrageExecutor>();
        
        AsyncArbConfig arb_cfg;
        arb_cfg.enabled = true;
        arb_cfg.signal_interval_us = 5000;     // 5ms 信号检查间隔
        arb_cfg.max_wait_us = 10000;           // 10ms 最大等待
        arb_cfg.ticks_per_signal = 5;          // 每5个tick检查一次
        arb_cfg.tick_queue_size = 1024;
        arb_cfg.order_queue_size = 256;
        _async_arb->setConfig(arb_cfg);
        _async_arb->setArbitrageManager(_spread_arb_manager.get());
        _async_arb->setSelfTradePrevention(_stp.get());
        
        WTSLogger::info("  AsyncArbitrageExecutor: enabled, signalInterval={}us, ticksPerSignal={}",
            arb_cfg.signal_interval_us, arb_cfg.ticks_per_signal);
    }
    else
    {
        WTSLogger::info("  AsyncArbitrageExecutor: disabled");
    }
    
    //------------------------------------------------------------
    // 初始化计数器
    //------------------------------------------------------------
    _tick_count = 0;
    _param_update_interval = _config.adaptive_update_interval;
    
    // 从配置更新下单错误处理参数
    _order_error_threshold = _config.order_error_threshold;
}

//==========================================================================
// UFT 策略回调
//==========================================================================

void UftFutuMmStrategy::on_init(IUftStraCtx* ctx)
{
    // 从基础数据管理模块获取合约参数（如果配置文件未指定）
    for (auto& ci : _contract_infos)
    {
        if (ci.multiplier <= 0 || ci.tick_size <= 0)
        {
            WTSCommodityInfo* commInfo = ctx->stra_get_comminfo(ci.code.c_str());
            if (commInfo)
            {
                if (ci.multiplier <= 0)
                    ci.multiplier = commInfo->getVolScale();
                if (ci.tick_size <= 0)
                    ci.tick_size = commInfo->getPriceTick();
                
                WTSLogger::info("UftFutuMmStrategy[{}] contract {} from base: multiplier={}, tickSize={}",
                    id(), ci.code, ci.multiplier, ci.tick_size);
            }
            else
            {
                // 使用默认值
                if (ci.multiplier <= 0) ci.multiplier = 1.0;
                if (ci.tick_size <= 0) ci.tick_size = 0.2;
                WTSLogger::warn("UftFutuMmStrategy[{}] contract {} not found in base data, using defaults",
                    id(), ci.code);
            }
        }
    }
    
    // 初始化业务模块（需要合约参数）
    initBusinessModules();
    
    // 输出初始化日志
    WTSLogger::info("UftFutuMmStrategy[{}] initialized: {} contracts, {} levels",
        id(), _contract_infos.size(), _config.num_levels);
    WTSLogger::info("  DeltaLimit={}, HedgeThreshold={}, MaxSpreadMult={}",
        _config.delta_limit, _config.hedge_threshold, _config.max_spread_mult);
    WTSLogger::info("  MaxInventory={}, SkewFactor={}, MaxSkew={}",
        _config.max_inventory, _config.skew_factor, _config.max_skew);
    WTSLogger::info("  Modules: spreadOpt={}, autoCancel={}, marketState={}, orderBook={}",
        _config.use_spread_optimizer, _config.use_auto_cancel, 
        _config.use_market_state, _config.use_order_book);
    
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
    
    // 订阅所有合约行情
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
    _risk_monitor->resetCloseout();  // 重置收盘前平仓状态
    
    // 重置本地状态
    _closeout_triggered = false;
    _blocked_contracts.clear();
    
    for (auto& [code, ms] : _market_states)
        ms->onSessionBegin(uTDate);
    
    // 启动异步套利执行器
    if (_async_arb)
    {
        _async_arb->start();
    }
    
    // 清空自成交防护模块
    if (_stp)
    {
        _stp->clear();
    }
    
    WTSLogger::info("UftFutuMmStrategy[{}] session begin: {}", id(), uTDate);
}

void UftFutuMmStrategy::on_session_end(IUftStraCtx* ctx, uint32_t uTDate)
{
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
    
    WTSLogger::info("UftFutuMmStrategy[{}] session end: {}, Delta: {}", 
        id(), uTDate, _portfolio->getTotalDelta());
}

void UftFutuMmStrategy::on_tick(IUftStraCtx* ctx, const char* stdCode, WTSTickData* tick)
{
    if (!_channel_ready || !tick)
        return;
    
    // 获取当前时间 (HHMMSS 格式)
    uint32_t currentTime = ctx->stra_get_time();
    
    //============================================================
    // 收盘前平仓检查 (使用 FutuRiskMonitor + 从基础数据获取收盘时间)
    //============================================================
    if (_config.closeout_minutes_before > 0 && 
        !_risk_monitor->isCloseoutCompleted())
    {
        // 从锚定合约的基础数据获取收盘时间
        WTSCommodityInfo* commInfo = ctx->stra_get_comminfo(_config.anchor_code.c_str());
        uint32_t closeTime = 151500;  // 默认收盘时间 15:15:00
        
        if (commInfo && commInfo->getSessionInfo())
        {
            closeTime = commInfo->getSessionInfo()->getCloseTime(true) * 100;  // HHMM -> HHMMSS
        }
        
        // 使用 FutuRiskMonitor 检查是否触发收盘前平仓
        bool triggered = _risk_monitor->checkCloseout(currentTime, closeTime);
        
        if (triggered && !_closeout_triggered)
        {
            _closeout_triggered = true;
            
            // 撤销所有未成交订单
            for (auto& [code, quoter] : _quoters)
            {
                quoter->cancelAll(ctx);
            }
            
            // 如果配置了平仓，执行对冲
            if (_config.closeout_flatten_position)
            {
                executeCloseoutHedge(ctx);
            }
        }
        
        // 如果已触发但未完成，暂停报价处理
        if (_risk_monitor->isCloseoutTriggered() && !_risk_monitor->isCloseoutCompleted())
        {
            return;
        }
    }
    
    if (_trading_halted)
        return;
    
    // 记录 tick 处理开始时间（仅在性能监控启用时）
    std::chrono::high_resolution_clock::time_point tick_start;
    if (_performance_monitor)
    {
        tick_start = std::chrono::high_resolution_clock::now();
    }
    
    // 1. 更新 Portfolio 行情数据
    _portfolio->onTick(stdCode, tick);
    
    // 2. 计算中间价
    double bid_px = tick->bidprice(0);
    double ask_px = tick->askprice(0);
    if (bid_px <= 0 || ask_px <= 0)
        return;
    
    double mid = (bid_px + ask_px) / 2.0;
    _last_mid[stdCode] = mid;
    
    // Note: ToxicFlowDetector no longer has onTick method.
    // It uses SelfTradeCalibrator for market state updates instead.
    
    // 3. 更新 SpreadOptimizer
    if (_config.use_spread_optimizer)
    {
        for (auto& [code, optimizer] : _spread_optimizers)
        {
            if (code == stdCode)
            {
                optimizer->onTick(tick);
                break;
            }
        }
    }
    
    // 4. 更新 MarketStateDetector
    if (_config.use_market_state)
    {
        auto it = _market_states.find(stdCode);
        if (it != _market_states.end())
        {
            const ContractState* cs = _portfolio->getContract(stdCode);
            if (cs)
            {
                it->second->setContract(stdCode, cs->tick_size);
                it->second->onTick(tick);
                
                // 检查市场状态
                auto detection = it->second->detect();
                if (detection.should_pause)
                {
                    // 暂停报价
                    for (auto& [code, quoter] : _quoters)
                    {
                        if (code == stdCode)
                        {
                            quoter->cancelAll(ctx);
                            break;
                        }
                    }
                    return;
                }
            }
        }
    }
    
    // 4.5 更新 MicroAlphaEngine 的 OFI 数据
    if (_config.use_alpha_engine)
    {
        uint64_t timestamp = tick->actiontime();
        double bid_vol = tick->bidqty(0);
        double ask_vol = tick->askqty(0);
        
        for (auto& [code, engine] : _alpha_engines)
        {
            if (code == stdCode)
            {
                engine->onTick(bid_px, ask_px, bid_vol, ask_vol, timestamp);
                break;
            }
        }
    }
    
    // 4.6 综合信号处理 (for markets without L2 transaction data)
    if (_config.use_synthetic_transaction)
    {
        // 4.6.1 Tick推断
        auto inferer_it = _tick_inferers.find(stdCode);
        if (inferer_it != _tick_inferers.end())
        {
            auto inferred = inferer_it->second->inferFromTick(
                bid_px, ask_px,
                tick->bidqty(0), tick->askqty(0),
                tick->price(), tick->totalvolume(),
                tick->actiontime()
            );
            
            // 4.6.2 信号融合
            auto fusion_it = _signal_fusions.find(stdCode);
            if (fusion_it != _signal_fusions.end())
            {
                // 添加Tick推断信号
                fusion_it->second->addTickInference(inferred);
                
                // 添加订单簿信号（如果有）
                if (_config.use_order_book)
                {
                    auto book_it = _order_books.find(stdCode);
                    if (book_it != _order_books.end())
                    {
                        auto book_analysis = book_it->second->analyze();
                        DepthImbalanceSignal book_sig;
                        book_sig.weighted_imbalance = book_analysis.imbalance_score;
                        book_sig.pressure_intensity = book_analysis.liquidity_score;
                        book_sig.bid_dominant = book_analysis.imbalance_score > 0.2;
                        book_sig.ask_dominant = book_analysis.imbalance_score < -0.2;
                        book_sig.confidence = book_analysis.direction_clear ? 0.8 : 0.4;
                        book_sig.timestamp = tick->actiontime();
                        fusion_it->second->addBookSignal(book_sig);
                    }
                }
                
                // 添加自身成交校准
                if (_self_trade_calibrator)
                {
                    auto calibration = _self_trade_calibrator->getCalibration(stdCode);
                    fusion_it->second->addSelfTradeCalibration(calibration);
                    
                    // 更新市场状态用于自适应权重
                    fusion_it->second->onTick(stdCode, mid, tick->actiontime());
                }
                
                // 执行信号融合
                auto synthetic_trans = fusion_it->second->fuse();
                
                // 更新 Alpha 引擎使用综合信号
                if (_config.use_alpha_engine)
                {
                    for (auto& [code, engine] : _alpha_engines)
                    {
                        if (code == stdCode)
                        {
                            engine->onSyntheticTransaction(synthetic_trans);
                            
                            // 更新毒性检测
                            if (_config.use_toxicity_detector && _toxicity_detector)
                            {
                                auto alpha_result = engine->getAlpha();
                                _toxicity_detector->onSyntheticAlpha(synthetic_trans, alpha_result);
                            }
                            break;
                        }
                    }
                }
            }
        }
        
        // 更新自身成交校准器的市场状态
        if (_self_trade_calibrator)
        {
            _self_trade_calibrator->onTick(stdCode, mid, tick->actiontime());
        }
    }
    
    // 7. 更新 OrderBookAnalyzer
    if (_config.use_order_book)
    {
        auto it = _order_books.find(stdCode);
        if (it != _order_books.end())
            it->second->onTick(tick);
    }
    
    // 6. 检查风险限制
    checkRiskLimits(ctx);
    if (_trading_halted)
        return;
    
    // 7. 处理报价逻辑
    processQuoting(ctx, stdCode, tick);
    
    // 8. 处理自动撤单
    processAutoCancel(ctx, stdCode, mid);
    
    // 9. 检查对冲
    checkAndHedge(ctx);
    
    // 10. 处理跨期价差套利（低频执行，每N个tick检查一次）
    if (_spread_arb_manager && _config.use_spread_arbitrage)
    {
        processSpreadArbitrage(ctx, stdCode, tick);
    }
    
    // 11. 定期更新自适应参数
    _tick_count++;
    if (_param_manager && _tick_count % _param_update_interval == 0)
    {
        // 记录当前绩效
        PerformanceSample sample;
        sample.realized_pnl = _portfolio->getTotalPnL();
        sample.unrealized_pnl = _portfolio->getTotalUnrealizedPnL();
        sample.volatility = 0;
        if (!_market_states.empty())
        {
            auto it = _market_states.find(_config.anchor_code);
            if (it != _market_states.end())
                sample.volatility = it->second->detect().vol_estimate;
        }
        
        _param_manager->recordPerformance(sample);
        
        // 更新参数
        auto adjustments = _param_manager->updateParameters();
        for (const auto& adj : adjustments)
        {
            // 应用参数调整到 SpreadOptimizer
            if (adj.type == ParamType::SPREAD_PHI)
            {
                for (auto& [code, optimizer] : _spread_optimizers)
                {
                    auto params = optimizer->getParams();
                    params.phi = adj.new_value;
                    optimizer->setParams(params);
                }
            }
            else if (adj.type == ParamType::SPREAD_BASE)
            {
                for (auto& [code, optimizer] : _spread_optimizers)
                {
                    auto params = optimizer->getParams();
                    params.base_spread = adj.new_value;
                    optimizer->setParams(params);
                }
            }
        }
    }
    
    // 记录 tick 处理延迟
    if (_performance_monitor)
    {
        auto tick_end = std::chrono::high_resolution_clock::now();
        auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tick_end - tick_start).count();
        _performance_monitor->recordTickToQuote(latency_ns);
        _performance_monitor->recordTickProcessed();
    }
}

void UftFutuMmStrategy::processQuoting(IUftStraCtx* ctx, const char* stdCode, WTSTickData* tick)
{
    // 找到对应的 Quoter
    FutuQuoter* quoter = nullptr;
    for (auto& [code, q] : _quoters)
    {
        if (code == stdCode)
        {
            quoter = q.get();
            break;
        }
    }
    
    if (!quoter)
        return;
    
    // 检查单合约限制 (由下方的 allow_bid / allow_ask 逻辑进行精确的方向级拦截)
    const ContractState* cs = _portfolio->getContract(stdCode);
    
    // 计算中间价
    double mid = (tick->bidprice(0) + tick->askprice(0)) / 2.0;
    
    // 获取 Alpha 信号
    double alpha = 0.0;
    if (_config.use_alpha_engine)
    {
        for (auto& [code, engine] : _alpha_engines)
        {
            if (code == stdCode)
            {
                auto alpha_result = engine->getAlpha();
                alpha = alpha_result.alpha;
                break;
            }
        }
    }
    
    // 检查毒性熔断
    bool toxicity_pause = false;
    if (_config.use_toxicity_detector && _toxicity_detector)
    {
        if (_toxicity_detector->isToxicFlow())
        {
            toxicity_pause = true;
            double tox_score = _toxicity_detector->getToxicityScore();
            ctx->stra_log_debug(fmt::format("Toxicity pause: score={}", tox_score).c_str());
        }
    }
    
    // 检查价差套利信号是否应该暂停单边报价
    bool spread_arb_pause_bid = false;
    bool spread_arb_pause_ask = false;
    double spread_arb_skew_adj = 0.0;
    double spread_arb_spread_mult = 1.0;
    
    if (_spread_arb_manager && _config.spread_arb_enhance_mm)
    {
        spread_arb_pause_bid = _spread_arb_manager->shouldPauseQuoting(stdCode, true);
        spread_arb_pause_ask = _spread_arb_manager->shouldPauseQuoting(stdCode, false);
        
        // 获取报价调整
        uint64_t now = ctx->stra_get_date() * 1000000ULL + ctx->stra_get_time() * 100ULL + ctx->stra_get_secs();
        auto adj = _spread_arb_manager->getQuotingAdjustment(stdCode, now);
        if (adj.confidence > 0)
        {
            spread_arb_skew_adj = adj.bid_skew_adjustment;
            spread_arb_spread_mult = adj.spread_multiplier;
        }
    }
    
    // 使用 GLFT 计算报价（包含组合级库存偏移）
    double skew = 0.0;
    double spread_mult = 1.0;
    
    if (_config.use_spread_optimizer)
    {
        for (auto& [code, optimizer] : _spread_optimizers)
        {
            if (code == stdCode)
            {
                // 构建组合上下文（包含相关合约库存）
                PortfolioContext port_ctx;
                port_ctx.total_delta = _portfolio->getTotalDelta();
                port_ctx.total_exposure = _portfolio->getTotalExposure();
                const ContractState* current_cs = _portfolio->getContract(stdCode);
                if (current_cs)
                {
                    port_ctx.current_multiplier = current_cs->multiplier;
                    port_ctx.current_hedge_ratio = current_cs->hedge_ratio;
                    port_ctx.current_price = current_cs->last_price > 0 ? current_cs->last_price : 1.0;
                }
                
                // 添加相关合约库存
                for (const auto& contract : _portfolio->getAllContracts())
                {
                    if (contract.code != stdCode && contract.position != 0)
                    {
                        // 从相关性管理器获取相关系数（如果有）
                        double corr = 0.0;
                        double hedge_ratio = contract.hedge_ratio;
                        
                        if (_correlation_manager)
                        {
                            auto stats = _correlation_manager->getCorrelation(stdCode, contract.code);
                            corr = stats.correlation;
                            hedge_ratio = stats.beta > 0 ? stats.beta : contract.hedge_ratio;
                        }
                        
                        double px = contract.last_price > 0 ? contract.last_price : 1.0;
                        port_ctx.addRelated(contract.code, contract.position, corr, hedge_ratio, contract.multiplier, px);
                    }
                }
                
                // 计算当前合约库存
                double inventory = _portfolio->getPosition(stdCode);
                
                // 使用组合级 GLFT 计算报价
                GLFTResult glft_result = optimizer->computeOptimalQuoteWithPortfolio(
                    mid, 
                    inventory, 
                    port_ctx,
                    alpha,
                    _config.alpha_sensitivity
                );
                
                // 提取 GLFT 结果
                skew = glft_result.inventory_skew;
                spread_mult = glft_result.spread_mult;
                
                // 应用价差套利调整
                skew += spread_arb_skew_adj;
                spread_mult *= spread_arb_spread_mult;
                
                // 检查是否应该暂停
                if (glft_result.pause_quoting || toxicity_pause)
                {
                    // 暂停时撤销所有报价
                    quoter->cancelAll(ctx);
                    return;
                }
                
                // 如果毒性较高，扩大价差
                if (_toxicity_detector)
                {
                    double tox_score = _toxicity_detector->getToxicityScore();
                    if (tox_score > 0.5)
                    {
                        spread_mult *= (1.0 + tox_score * 0.5);
                    }
                }
                
                break;
            }
        }
    }
    else
    {
        // 回退到旧逻辑：使用增强的 Portfolio skew 计算
        skew = _portfolio->computeEnhancedSkew(stdCode);
        spread_mult = _portfolio->getSpreadMultiplierByRisk(_config.max_spread_mult);
    }
    
    // 检查组合 Delta 限制
    bool allow_bid = _portfolio->getTotalDelta() < _config.delta_limit;
    bool allow_ask = _portfolio->getTotalDelta() > -_config.delta_limit;
    
    // 应用风险监控的方向禁用状态
    if (_long_blocked)
        allow_bid = false;
    if (_short_blocked)
        allow_ask = false;
        
    // 单合约风控封锁：BLOCK_CONTRACT_OPENING = 只允许平仓，不允许开仓
    // 这意味着只能报有利于减仓的方向
    if (_blocked_contracts[stdCode])
    {
        const ContractState* blocked_cs = _portfolio->getContract(stdCode);
        if (blocked_cs)
        {
            if (blocked_cs->position > 0)
            {
                // 多头持仓：允许卖单（平多），不允许买单（开多）
                allow_bid = false;
                // allow_ask 保持原样（允许平多）
            }
            else if (blocked_cs->position < 0)
            {
                // 空头持仓：允许买单（平空），不允许卖单（开空）
                allow_ask = false;
                // allow_bid 保持原样（允许平空）
            }
            else
            {
                // 无持仓：完全禁止开新仓
                allow_bid = false;
                allow_ask = false;
            }
        }
        else
        {
            // 无法获取合约状态，保守处理：完全禁止
            allow_bid = false;
            allow_ask = false;
        }
    }
    
    // 应用增强的单向报价逻辑
    // 当库存偏离严重时，只报有利于减仓的一边
    int oneSidedSuggestion = _portfolio->getOneSidedQuoteSuggestion();
    if (oneSidedSuggestion != 0)
    {
        if (oneSidedSuggestion == 1)
        {
            // 只报买单（减空头）
            allow_ask = false;
        }
        else if (oneSidedSuggestion == -1)
        {
            // 只报卖单（减多头）
            allow_bid = false;
        }
    }
    
    // 额外的单向报价检查（基于增强的 shouldQuoteSide 方法）
    if (allow_bid && !_portfolio->shouldQuoteSide(true))
        allow_bid = false;
    if (allow_ask && !_portfolio->shouldQuoteSide(false))
        allow_ask = false;
    
    // 检查报价暂停状态
    if (_quoting_paused)
    {
        quoter->cancelAll(ctx);
        return;
    }
    
    // 应用价差套利的单边暂停信号
    if (spread_arb_pause_bid) allow_bid = false;
    if (spread_arb_pause_ask) allow_ask = false;
    
    // 单合约持仓方向限制
    if (cs)
    {
        // 累加计算当前Pending的挂单数量，反映真实的 Exposure 头寸压力
        double pending_bid = 0.0;
        double pending_ask = 0.0;
        for (const auto& q : quoter->getBidLevels()) { if (q.order_id != 0) pending_bid += q.qty; }
        for (const auto& q : quoter->getAskLevels()) { if (q.order_id != 0) pending_ask += q.qty; }
        
        double long_exposure = std::max(0.0, cs->position) + pending_bid;
        double short_exposure = std::max(0.0, -cs->position) + pending_ask;
        
        // 如果持仓接近上限，停止该方向的报价
        if (cs->max_position > 0)
        {
            if (long_exposure > cs->max_position * 0.8)
                allow_bid = false;  // 多头接近上限，停止买入
            if (short_exposure > cs->max_position * 0.8)
                allow_ask = false;  // 空头接近上限，停止卖出
        }
        if (cs->max_delta > 0)
        {
            double contract_delta = cs->delta();
            if (contract_delta > cs->max_delta * 0.8)
                allow_bid = false;
            if (contract_delta < -cs->max_delta * 0.8)
                allow_ask = false;
        }
    }
    
    // 检查是否应该暂停报价
    if (_portfolio->shouldPauseQuoting())
        return;
    
    // 刷新报价，返回下单数量
    uint32_t orders_placed = quoter->refreshQuotes(ctx, mid, skew, spread_mult, allow_bid, allow_ask);
    
    // 记录下单频率（用于风险监控）
    if (_risk_monitor)
    {
        for (uint32_t i = 0; i < orders_placed; i++)
        {
            _risk_monitor->recordOrder();
        }
    }
    
    // 记录订单到自动撤单策略
    if (_config.use_auto_cancel && _auto_cancel)
    {
        uint64_t now = ctx->stra_get_date() * 1000000ULL + ctx->stra_get_time() * 100ULL + ctx->stra_get_secs();
        
        for (const auto& level : quoter->getBidLevels())
        {
            if (level.order_id != 0)
            {
                _auto_cancel->trackOrder(level.order_id, stdCode, level.price, 
                                         level.qty, true, now, mid);
                
                // 同时记录到自成交防护模块
                if (_stp)
                {
                    _stp->trackMMOrder(stdCode, level.order_id, level.price, 
                                       level.qty, true, now);
                }
            }
        }
        for (const auto& level : quoter->getAskLevels())
        {
            if (level.order_id != 0)
            {
                _auto_cancel->trackOrder(level.order_id, stdCode, level.price,
                                         level.qty, false, now, mid);
                
                // 同时记录到自成交防护模块
                if (_stp)
                {
                    _stp->trackMMOrder(stdCode, level.order_id, level.price, 
                                       level.qty, false, now);
                }
            }
        }
    }
    else if (_stp)
    {
        // 即使不使用自动撤单，也要记录到自成交防护模块
        uint64_t now = ctx->stra_get_date() * 1000000ULL + ctx->stra_get_time() * 100ULL + ctx->stra_get_secs();
        
        for (const auto& level : quoter->getBidLevels())
        {
            if (level.order_id != 0)
            {
                _stp->trackMMOrder(stdCode, level.order_id, level.price, 
                                   level.qty, true, now);
            }
        }
        for (const auto& level : quoter->getAskLevels())
        {
            if (level.order_id != 0)
            {
                _stp->trackMMOrder(stdCode, level.order_id, level.price, 
                                   level.qty, false, now);
            }
        }
    }
}

void UftFutuMmStrategy::checkAndHedge(IUftStraCtx* ctx)
{
    if (!_portfolio->needsHedging())
        return;
    
    auto hedge_action = _portfolio->computeHedge();
    if (hedge_action.qty == 0)
        return;
    
    if (hedge_action.qty > 0)
    {
        ctx->stra_enter_long(hedge_action.code.c_str(), hedge_action.price, hedge_action.qty);
        WTSLogger::info("UftFutuMmStrategy[{}] HEDGE BUY {} @ {}", 
            id(), hedge_action.qty, hedge_action.code);
    }
    else
    {
        ctx->stra_enter_short(hedge_action.code.c_str(), hedge_action.price, -hedge_action.qty);
        WTSLogger::info("UftFutuMmStrategy[{}] HEDGE SELL {} @ {}", 
            id(), -hedge_action.qty, hedge_action.code);
    }
}

void UftFutuMmStrategy::checkRiskLimits(IUftStraCtx* ctx)
{
    if (!_risk_monitor)
        return;
    
    // 更新时间
    uint64_t now = ctx->stra_get_date() * 1000000ULL + ctx->stra_get_time() * 100ULL + ctx->stra_get_secs();
    _risk_monitor->setCurrentTime(now);
    
    // 尝试恢复（对于可逆风险）
    if (_trading_halted || _quoting_paused || _long_blocked || _short_blocked || !_blocked_contracts.empty())
    {
        if (_risk_monitor->checkAndRecover(_portfolio.get()))
        {
            // 同步状态
            _trading_halted = _risk_monitor->isTradingHalted();
            _quoting_paused = _risk_monitor->isQuotingPaused();
            _long_blocked = _risk_monitor->isLongBlocked();
            _short_blocked = _risk_monitor->isShortBlocked();
            _blocked_contracts.clear();
            ctx->stra_log_info("Risk: auto-recovery successful");
        }
    }
    
    // 检查风险限制
    auto violations = _risk_monitor->checkRiskLimits(_portfolio.get());
    
    if (violations.empty())
        return;
    
    // 确定风险动作和类别
    RiskCategory category;
    auto action = _risk_monitor->determineActionWithCategory(violations, category);
    
    switch (action)
    {
        case RiskAction::WARN:
            ctx->stra_log_info("Risk warning triggered");
            break;
            
        case RiskAction::WIDEN_SPREAD:
            ctx->stra_log_info("Risk: widening spread");
            break;
            
        case RiskAction::REDUCE_SIZE:
            ctx->stra_log_info("Risk: reducing order sizes");
            break;
            
        case RiskAction::BLOCK_SIDE_LONG:
            ctx->stra_log_info("Risk: blocking LONG side");
            _long_blocked = true;
            _risk_monitor->blockLong();
            break;
            
        case RiskAction::BLOCK_SIDE_SHORT:
            ctx->stra_log_info("Risk: blocking SHORT side");
            _short_blocked = true;
            _risk_monitor->blockShort();
            break;
            
        case RiskAction::BLOCK_CONTRACT_OPENING:
            {
                ctx->stra_log_info("Risk: blocking specific contract opening (Only Close strategy active)");
                const ContractState* breached = _portfolio->getBreachedContract();
                if (breached)
                    _blocked_contracts[breached->code] = true;
            }
            break;
            
        case RiskAction::PAUSE_QUOTING:
            ctx->stra_log_info("Risk: pausing quoting");
            _quoting_paused = true;
            _risk_monitor->pauseQuoting();
            for (auto& [code, quoter] : _quoters)
                quoter->cancelAll(ctx);
            break;
            
        case RiskAction::FLATTEN_POSITION:
            ctx->stra_log_error("Risk: flattening positions");
            ctx->stra_cancel_all("");
            break;
            
        case RiskAction::HALT_TRADING:
            {
                const char* cat_str = (category == RiskCategory::IRREVERSIBLE) ? "IRREVERSIBLE" : "REVERSIBLE";
                char msg[128];
                snprintf(msg, sizeof(msg), "Risk: HALTING TRADING (%s)", cat_str);
                ctx->stra_log_error(msg);
                _trading_halted = true;
                _risk_monitor->haltTrading(category);
                ctx->stra_cancel_all("");
            }
            break;
            
        default:
            break;
    }
}

void UftFutuMmStrategy::processAutoCancel(IUftStraCtx* ctx, const char* stdCode, double mid)
{
    if (!_config.use_auto_cancel || !_auto_cancel)
        return;
    
    uint64_t now = ctx->stra_get_date() * 1000000ULL + ctx->stra_get_time() * 100ULL + ctx->stra_get_secs();
    
    bool state_changed = false;
    auto ms_it = _market_states.find(stdCode);
    if (ms_it != _market_states.end())
    {
        auto detection = ms_it->second->detect();
        state_changed = (detection.state != MarketState::NORMAL);
    }
    
    bool inventory_hit = _portfolio->isAnyLimitBreached();
    
    double tick_size = 0.2; // default
    const ContractState* cs = _portfolio->getContract(stdCode);
    if (cs) tick_size = cs->tick_size;
    
    auto cancel_actions = _auto_cancel->checkOrders(now, stdCode, mid, tick_size, state_changed, inventory_hit);
    _auto_cancel->executeCancellations(ctx, cancel_actions);
}

void UftFutuMmStrategy::executeCloseoutHedge(IUftStraCtx* ctx)
{
    //============================================================
    // 收盘前对冲所有敞口
    // 1. 计算当前总 Delta
    // 2. 使用锚定合约对冲
    //============================================================
    
    double totalDelta = _portfolio->getTotalDelta();
    
    if (std::abs(totalDelta) < 0.01)
    {
        WTSLogger::info("UftFutuMmStrategy[{}] Closeout: No position to hedge (Delta=0)", id());
        _risk_monitor->markCloseoutCompleted();
        return;
    }
    
    // 获取锚定合约信息
    const ContractState* anchorState = _portfolio->getContract(_config.anchor_code);
    if (!anchorState)
    {
        WTSLogger::error("UftFutuMmStrategy[{}] Closeout failed: anchor contract {} not found",
            id(), _config.anchor_code);
        return;
    }
    
    // 计算需要交易的手数
    // Delta = position * multiplier * hedge_ratio
    // 需要的手数 = -Delta / (multiplier * hedge_ratio)
    double multiplier = anchorState->multiplier;
    double hedgeRatio = anchorState->hedge_ratio;
    
    if (multiplier <= 0)
    {
        WTSLogger::error("UftFutuMmStrategy[{}] Closeout failed: invalid multiplier={}",
            id(), multiplier);
        return;
    }
    
    double hedgeQty = -totalDelta / (multiplier * hedgeRatio);
    int32_t lots = static_cast<int32_t>(std::round(hedgeQty));
    
    if (lots == 0)
    {
        WTSLogger::info("UftFutuMmStrategy[{}] Closeout: Hedge quantity too small (lots=0)", id());
        _risk_monitor->markCloseoutCompleted();
        return;
    }
    
    // 获取当前市场价格
    double bidPrice = anchorState->bid1;
    double askPrice = anchorState->ask1;
    
    if (bidPrice <= 0 || askPrice <= 0)
    {
        WTSLogger::error("UftFutuMmStrategy[{}] Closeout failed: invalid market prices bid={}, ask={}",
            id(), bidPrice, askPrice);
        return;
    }
    
    // 执行对冲交易
    bool isBuy = (lots > 0);  // 正数表示需要买入来对冲空头
    int32_t absLots = std::abs(lots);
    
    // 使用对手价确保成交
    double executePrice = isBuy ? askPrice : bidPrice;
    
    WTSLogger::warn("UftFutuMmStrategy[{}] CLOSEOUT HEDGE: {} {} @ {} (Delta={})",
        id(), 
        isBuy ? "BUY" : "SELL",
        absLots, 
        executePrice,
        totalDelta);
    
    if (isBuy)
    {
        ctx->stra_enter_long(_config.anchor_code.c_str(), executePrice, absLots);
    }
    else
    {
        ctx->stra_enter_short(_config.anchor_code.c_str(), executePrice, absLots);
    }
    
    // 标记完成
    _risk_monitor->markCloseoutCompleted();
}

void UftFutuMmStrategy::on_order_queue(IUftStraCtx* ctx, const char* stdCode, WTSOrdQueData* newOrdQue)
{
    // Level2: 更新 OrderBookAnalyzer
    if (_config.use_order_book)
    {
        auto it = _order_books.find(stdCode);
        if (it != _order_books.end())
            it->second->onOrderQueue(newOrdQue);
    }
}

void UftFutuMmStrategy::on_order_detail(IUftStraCtx* ctx, const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
    // Level2: 更新 OrderBookAnalyzer
    if (_config.use_order_book)
    {
        auto it = _order_books.find(stdCode);
        if (it != _order_books.end())
            it->second->onOrderDetail(newOrdDtl);
    }
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
    
    // 更新 MicroAlphaEngine 的 Trade Imbalance
    if (_config.use_alpha_engine)
    {
        for (auto& [code, engine] : _alpha_engines)
        {
            if (code == stdCode)
            {
                engine->onTrade(price, qty, isBuy, timestamp);
                break;
            }
        }
    }
    
    // 更新 SpreadOptimizer 的成交数据
    if (_config.use_spread_optimizer)
    {
        for (auto& [code, optimizer] : _spread_optimizers)
        {
            if (code == stdCode)
            {
                optimizer->onTrade(price, qty, isBuy);
                break;
            }
        }
    }
    
    // 更新 CorrelationManager 的价格数据
    if (_correlation_manager)
    {
        _correlation_manager->onTick(stdCode, price, timestamp);
    }
    
    // 移除不安全的直连调用：_spread_arb_manager->onTick(stdCode, price, 1.0, timestamp);
    // 因为套利主计算已由 _async_arb 在子线程专门负责，主线程的 on_transaction 强行调用会导致致命的 Map 并发读写冲突 (Core Dump)
    
    // 更新 OrderBookAnalyzer (可选)
    if (_config.use_order_book)
    {
        auto it = _order_books.find(stdCode);
        if (it != _order_books.end())
            it->second->onTransaction(newTrans);
    }
    
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
    
    // 更新 Portfolio 持仓
    double current_pos = _portfolio->getPosition(stdCode);
    double delta_pos = isLong ? vol : -vol;
    _portfolio->onPositionUpdate(stdCode, current_pos + delta_pos);
    
    // 更新 Quoter 订单状态
    for (auto& [code, quoter] : _quoters)
    {
        if (quoter->isMyOrder(localid))
        {
            quoter->onTrade(localid, vol, price);
            break;
        }
    }
    
    // 更新 SpreadOptimizer 成交统计
    if (_config.use_spread_optimizer)
    {
        for (auto& [code, optimizer] : _spread_optimizers)
        {
            if (code == stdCode)
            {
                // 检查是否为 cross（成交价格穿过对手价）
                auto it = _last_mid.find(stdCode);
                bool wasCrossed = false;
                if (it != _last_mid.end())
                {
                    double mid = it->second;
                    wasCrossed = (isLong && price > mid) || (!isLong && price < mid);
                }
                optimizer->onFill(vol, wasCrossed);
                break;
            }
        }
    }
    
    // 从自动撤单中移除
    if (_auto_cancel)
        _auto_cancel->untrackOrder(localid);
    
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
        }
        _perf_analyzer->recordTrade(trade);
        _perf_analyzer->updatePosition(stdCode, _portfolio->getPosition(stdCode), 0);
    }
    
    // 记录到自身成交校准器 (统一管理成交记录，供毒性检测使用)
    if (_self_trade_calibrator)
    {
        uint64_t timestamp = ctx->stra_get_date() * 1000000ULL + ctx->stra_get_time() * 100ULL + ctx->stra_get_secs();
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
        if (_config.use_toxicity_detector && _toxicity_detector)
        {
            auto calibration = _self_trade_calibrator->getCalibration(stdCode);
            _toxicity_detector->onSelfTradeCalibration(calibration);
        }
    }
    
    WTSLogger::info("UftFutuMmStrategy[{}] TRADE: {} {} {}@{} | Delta: {}",
        id(), stdCode, isLong ? "BUY" : "SELL", vol, price, _portfolio->getTotalDelta());
}

void UftFutuMmStrategy::on_order(IUftStraCtx* ctx, uint32_t localid, const char* stdCode,
                                  bool isLong, uint32_t offset, double totalQty, 
                                  double leftQty, double price, bool isCanceled)
{
    // 更新频率统计
    if (_risk_monitor && isCanceled)
        _risk_monitor->recordCancel();
    
    // 更新 Quoter 订单状态
    for (auto& [code, quoter] : _quoters)
    {
        if (quoter->isMyOrder(localid))
        {
            quoter->onOrder(localid, isCanceled, leftQty);
            break;
        }
    }
    
    // 从自动撤单中移除
    if (isCanceled && _auto_cancel)
        _auto_cancel->untrackOrder(localid);
    
    // 从自成交防护模块中移除（订单撤销或完全成交）
    if (isCanceled && _stp)
        _stp->untrackOrder(localid);
}

void UftFutuMmStrategy::on_position(IUftStraCtx* ctx, const char* stdCode, bool isLong,
                                     double prevol, double preavail, double newvol, double newavail)
{
    // 同步 Portfolio 持仓
    double current = _portfolio->getPosition(stdCode);
    double actual = isLong ? (newvol + prevol) : -(newvol + prevol);
    
    if (std::abs(current - actual) > 0.01)
    {
        WTSLogger::info("UftFutuMmStrategy[{}] Position sync: {} local={}, actual={}",
            id(), stdCode, current, actual);
        _portfolio->onPositionUpdate(stdCode, actual);
    }
}

void UftFutuMmStrategy::on_channel_ready(IUftStraCtx* ctx)
{
    _channel_ready = true;
    
    //============================================================
    // 同步持仓和未成交订单
    //============================================================
    WTSLogger::info("UftFutuMmStrategy[{}] syncing positions and pending orders...", id());
    
    for (const auto& ci : _contract_infos)
    {
        // 同步多头持仓
        double longPos = ctx->stra_get_position(ci.code.c_str(), true);
        // 同步空头持仓
        double shortPos = ctx->stra_get_position(ci.code.c_str(), false);
        // 计算净持仓
        double netPos = longPos - shortPos;
        
        // 更新 Portfolio
        double currentPos = _portfolio->getPosition(ci.code);
        if (std::abs(currentPos - netPos) > 0.01)
        {
            _portfolio->updatePosition(ci.code, netPos, 0);
            WTSLogger::info("UftFutuMmStrategy[{}] Position sync: {} local={}, actual={} (long={}, short={})",
                id(), ci.code, currentPos, netPos, longPos, shortPos);
        }
        
        // 同步未成交订单
        // 获取当前合约的未完成订单并记录到 AutoCancel 和自成交防护模块
        // 注意：UFT 框架通常会在 channel ready 后重新推送订单状态
        // 这里我们主动查询并记录
        uint64_t now = ctx->stra_get_date() * 1000000ULL + ctx->stra_get_time() * 100ULL + ctx->stra_get_secs();
        double mid = 0;
        
        // 尝试获取当前中间价
        auto midIt = _last_mid.find(ci.code);
        if (midIt != _last_mid.end())
        {
            mid = midIt->second;
        }
        
        // 记录日志
        WTSLogger::info("UftFutuMmStrategy[{}] Contract {} synced: netPos={}, multiplier={}, tickSize={}",
            id(), ci.code, netPos, ci.multiplier, ci.tick_size);
    }
    
    // 同步后检查风控状态
    double totalDelta = _portfolio->getTotalDelta();
    WTSLogger::info("UftFutuMmStrategy[{}] Total delta after sync: {}", id(), totalDelta);
    
    // 只有在非不可逆风险状态下才恢复
    if (_risk_monitor)
    {
        // 检查是否是可逆风险
        if (_risk_monitor->getHaltCategory() != RiskCategory::IRREVERSIBLE)
        {
            _trading_halted = false;
            _quoting_paused = false;
            _long_blocked = false;
            _short_blocked = false;
            _risk_monitor->resumeTrading();
            _risk_monitor->resumeQuoting();
            _risk_monitor->unblockLong();
            _risk_monitor->unblockShort();
        }
        else
        {
            WTSLogger::warn("UftFutuMmStrategy[{}] channel ready but trading remains halted (IRREVERSIBLE)", id());
        }
    }
    else
    {
        _trading_halted = false;
    }
    
    WTSLogger::info("UftFutuMmStrategy[{}] channel ready", id());
}

void UftFutuMmStrategy::on_channel_lost(IUftStraCtx* ctx)
{
    _channel_ready = false;
    WTSLogger::error("UftFutuMmStrategy[{}] channel lost", id());
}

//==========================================================================
// 跨期价差套利执行逻辑 - 异步版本
//==========================================================================

void UftFutuMmStrategy::processSpreadArbitrage(IUftStraCtx* ctx, const char* stdCode, WTSTickData* tick)
{
    if (!_async_arb || !_config.use_spread_arbitrage)
        return;
    
    // ============================================================
    // 主线程：快速推送 tick 数据到异步队列（~50ns，非阻塞）
    // ============================================================
    _async_arb->pushTick(stdCode, tick->price(), 1.0, tick->actiontime());
    
    // ============================================================
    // 主线程：更新 MM 订单状态到异步执行器（用于自成交检测）
    // ============================================================
    if (_stp)
    {
        _async_arb->updateMMOrders(stdCode, 
            _stp->getMMBuyOrders(stdCode), 
            _stp->getMMSellOrders(stdCode));
    }
    
    // ============================================================
    // 主线程：处理异步执行器返回的订单请求（执行订单）
    // ============================================================
    _async_arb->processPendingOrders([this, ctx](const ArbOrderRequest& order) {
        // 执行订单
        if (order.is_buy)
        {
            ctx->stra_enter_long(order.code.c_str(), order.price, order.qty);
            WTSLogger::info("AsyncArb BUY {} {}@{}", order.code, order.qty, order.price);
        }
        else
        {
            ctx->stra_enter_short(order.code.c_str(), order.price, order.qty);
            WTSLogger::info("AsyncArb SELL {} {}@{}", order.code, order.qty, order.price);
        }
        
        // 记录到风险监控
        if (_risk_monitor)
        {
            _risk_monitor->recordOrder();
        }
    });
}

void UftFutuMmStrategy::executeSpreadSignal(IUftStraCtx* ctx, const SpreadSignal& signal)
{
    if (!_stp)
    {
        WTSLogger::warn("SelfTradePrevention not initialized");
        return;
    }
    
    // 确定买卖方向
    // OPEN_LONG_SPREAD: 买leg1, 卖leg2
    // OPEN_SHORT_SPREAD: 卖leg1, 买leg2
    bool leg1_is_buy = (signal.type == SpreadSignalType::OPEN_LONG_SPREAD || 
                        signal.type == SpreadSignalType::CLOSE_SHORT_SPREAD);
    bool leg2_is_buy = !leg1_is_buy;
    
    // ============================================================
    // 自成交检测 - Leg 1
    // ============================================================
    auto check1 = _stp->checkOrder(signal.leg1_code, leg1_is_buy, signal.leg1_price, false);
    
    // ============================================================
    // 自成交检测 - Leg 2
    // ============================================================
    auto check2 = _stp->checkOrder(signal.leg2_code, leg2_is_buy, signal.leg2_price, false);
    
    // ============================================================
    // 处理自成交风险
    // ============================================================
    if (check1.has_risk || check2.has_risk)
    {
        WTSLogger::warn("Self-trade risk detected for spread signal: pair={}, leg1_risk={}, leg2_risk={}",
            signal.pair_id, check1.has_risk, check2.has_risk);
        
        // 根据策略处理：先撤销冲突的做市订单
        if (check1.has_risk && check1.recommended_action == SelfTradeCheckResult::Action::CANCEL_FIRST)
        {
            for (uint32_t order_id : check1.conflicting_order_ids)
            {
                ctx->stra_cancel(order_id);
                _stp->untrackOrder(order_id);
                WTSLogger::info("Cancelled MM order {} for spread arbitrage", order_id);
            }
        }
        
        if (check2.has_risk && check2.recommended_action == SelfTradeCheckResult::Action::CANCEL_FIRST)
        {
            for (uint32_t order_id : check2.conflicting_order_ids)
            {
                ctx->stra_cancel(order_id);
                _stp->untrackOrder(order_id);
                WTSLogger::info("Cancelled MM order {} for spread arbitrage", order_id);
            }
        }
        
        // 如果策略是拒绝，则不执行
        if (check1.recommended_action == SelfTradeCheckResult::Action::REJECT ||
            check2.recommended_action == SelfTradeCheckResult::Action::REJECT)
        {
            WTSLogger::warn("Spread signal rejected due to self-trade risk");
            return;
        }
    }
    
    // 使用调整后的价格（如果有）
    double leg1_price = signal.leg1_price;
    double leg2_price = signal.leg2_price;
    
    if (check1.has_risk && check1.recommended_action == SelfTradeCheckResult::Action::ADJUST_PRICE)
    {
        leg1_price = check1.adjusted_price;
    }
    if (check2.has_risk && check2.recommended_action == SelfTradeCheckResult::Action::ADJUST_PRICE)
    {
        leg2_price = check2.adjusted_price;
    }
    
    // ============================================================
    // 执行套利订单
    // ============================================================
    double leg1_qty = signal.leg1_qty > 0 ? signal.leg1_qty : signal.suggested_size;
    double leg2_qty = signal.leg2_qty > 0 ? signal.leg2_qty : signal.suggested_size;
    
    if (leg1_is_buy)
    {
        ctx->stra_enter_long(signal.leg1_code.c_str(), leg1_price, leg1_qty);
        WTSLogger::info("SpreadArb BUY {} {}@{}", signal.leg1_code, leg1_qty, leg1_price);
    }
    else
    {
        ctx->stra_enter_short(signal.leg1_code.c_str(), leg1_price, leg1_qty);
        WTSLogger::info("SpreadArb SELL {} {}@{}", signal.leg1_code, leg1_qty, leg1_price);
    }
    
    if (leg2_is_buy)
    {
        ctx->stra_enter_long(signal.leg2_code.c_str(), leg2_price, leg2_qty);
        WTSLogger::info("SpreadArb BUY {} {}@{}", signal.leg2_code, leg2_qty, leg2_price);
    }
    else
    {
        ctx->stra_enter_short(signal.leg2_code.c_str(), leg2_price, leg2_qty);
        WTSLogger::info("SpreadArb SELL {} {}@{}", signal.leg2_code, leg2_qty, leg2_price);
    }
}

void UftFutuMmStrategy::onSpreadTrade(IUftStraCtx* ctx, const std::string& pair_id,
                                       const std::string& code, bool is_buy, 
                                       double qty, double price)
{
    // 更新套利仓位
    if (_spread_arb_manager)
    {
        // 这里需要根据实际的仓位跟踪逻辑更新
        // 暂时只记录日志
        WTSLogger::info("SpreadArb trade: pair={}, code={}, {} {}@{}",
            pair_id, code, is_buy ? "BUY" : "SELL", qty, price);
    }
}

void UftFutuMmStrategy::on_entrust(uint32_t localid, bool bSuccess, const char* message)
{
    if (bSuccess)
    {
        // 报单成功，重置错误计数
        _order_error_count = 0;
        return;
    }
    
    // 报单失败，统一处理所有下单错误
    std::string errMsg = message ? message : "";
    
    // 所有下单错误统一处理：暂停报价，记录错误
    _order_error_count++;
    
    WTSLogger::error("UftFutuMmStrategy[{}] Order failed (count={}): localid={}, error={}",
        id(), _order_error_count, localid, errMsg);
    
    // 连续下单错误达到阈值，暂停交易
    if (_order_error_count >= _order_error_threshold)
    {
        _quoting_paused = true;
        _trading_halted = true;
        
        WTSLogger::error("UftFutuMmStrategy[{}] Trading HALTED due to consecutive order errors (threshold={})",
            id(), _order_error_threshold);
        
        // 如果有风控模块，通知它
        if (_risk_monitor)
        {
            _risk_monitor->haltTrading(RiskCategory::IRREVERSIBLE);
        }
    }
    else
    {
        // 临时暂停报价
        _quoting_paused = true;
        WTSLogger::warn("UftFutuMmStrategy[{}] Quoting paused due to order error", id());
    }
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