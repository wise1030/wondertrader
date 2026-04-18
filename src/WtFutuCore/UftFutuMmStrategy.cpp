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
#include "../Share/CodeHelper.hpp"

// 业务模块头文件
#include "FutuPortfolio.h"
#include "FutuQuoter.h"
#include "SpreadOptimizer.h"

#include "UnifiedOrderTracker.h"
#include "MarketDataContext.h"
#include "FutuRiskMonitor.h"
#include "ToxicFlowDetector.h"
#include "AdaptiveParamManager.h"
#include "PerformanceAnalyzer.h"
#include "PerformanceMonitor.h"
#include "SpreadArbitrageManager.h"
#include "FutuComponentFactory.h"
#include "SelfTradePrevention.h"
#include "StrategyCoordinator.h"
#include "AsyncArbitrageExecutor.h"
#include "BilateralQuoteStats.h"
#include "CorrelationManager.h"

// 综合信号组件头文件
#include "TickTransactionInferer.h"
#include "SelfTradeCalibrator.h"
#include "SignalAggregator.h"  // 新增：信号聚合器
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
    , _trading_halted(false)
    , _use_coordinator_mode(false)
    , _toxicity_paused(false)
    , _toxicity_resume_time(0)
    , _long_blocked(false)
    , _short_blocked(false)
    , _quoting_paused(false)
    , _order_error_count(0)
    , _order_error_threshold(3)    // 连续3次错误暂停
    , _tick_count(0)
    , _param_update_interval(100)
    // P0-2.3: PortfolioContext cache - dirty on start to force first build
    , _portfolio_ctx_dirty(true)
    // 热更新参数指针初始化
    , _hot_base_spread(nullptr)
    , _hot_spread_mult(nullptr)
    , _hot_base_qty(nullptr)
    , _hot_skew_factor(nullptr)
    , _hot_max_skew(nullptr)
    , _hot_max_inventory(nullptr)
    , _hot_target_inventory(nullptr)
    , _hot_max_delta(nullptr)
    , _hot_hedge_threshold(nullptr)
    , _hot_max_daily_loss(nullptr)
    , _hot_order_error_threshold(nullptr)
    , _hot_toxicity_threshold(nullptr)
    , _hot_toxicity_cooloff_ms(nullptr)
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
    
    //------------------------------------------------------------
    // 读取配置文件路径
    //------------------------------------------------------------
    {
        _config.coordinator_config = readString(cfg, "coordinatorConfig", "");
        _config.spread_arbitrage_config = readString(cfg, "spreadArbitrageConfig", "");
    }
    
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
            // 单合约目标持仓，默认0（平衡），超过时报价倾向于减仓
            ci.target_position = readDouble(cfgItem, "targetPosition", 0.0);
            
            _contract_infos.push_back(ci);
            _config.codes.push_back(ci.code);
        }
    }
    
    //------------------------------------------------------------
    // 读取 Delta 软指标参数（用于 skew 和对冲决策，不触发风控）
    //------------------------------------------------------------
    WTSVariant* cfgRisk = cfg->get("risk");
    if (cfgRisk) {
        _config.max_delta = readDouble(cfgRisk, "maxDelta", 5000000.0);  // Delta 软限制
        _config.hedge_threshold = readDouble(cfgRisk, "hedgeThreshold", 3000000.0);
        _config.max_spread_mult = readDouble(cfgRisk, "maxSpreadMult", 3.0);
        _config.max_daily_loss = readDouble(cfgRisk, "maxDailyLoss", -50000.0);
        _config.max_exposure = readDouble(cfgRisk, "maxExposure", 300000000.0);  // 硬风控
    }
    
    //------------------------------------------------------------
    // 读取报价参数（嵌套在 quoting 节点下）
    //------------------------------------------------------------
    WTSVariant* cfgQuoting = cfg->get("quoting");
    if (cfgQuoting) {
        _config.num_levels = readUInt32(cfgQuoting, "numLevels", 3);
        _config.base_spread = readDouble(cfgQuoting, "baseSpread", 2.0);
        _config.base_qty = readDouble(cfgQuoting, "baseQty", 1.0);
        _config.qty_decay = readDouble(cfgQuoting, "qtyDecay", 0.7);
        _config.level_step = readDouble(cfgQuoting, "levelStep", 1.0);
        
        // 新增双边报价参数
        _config.use_bilateral_quote = readBool(cfgQuoting, "useBilateralQuote", false);
        _config.max_obligation_spread = readDouble(cfgQuoting, "maxObligationSpread", 100.0);
    }
    
    //------------------------------------------------------------
    // 读取库存管理参数（嵌套在 inventory 节点下）
    //------------------------------------------------------------
    WTSVariant* cfgInventory = cfg->get("inventory");
    if (cfgInventory) {
        _config.max_inventory = readDouble(cfgInventory, "maxInventory", 100.0);
        _config.skew_factor = readDouble(cfgInventory, "skewFactor", 0.001);
        _config.max_skew = readDouble(cfgInventory, "maxSkew", 5.0);
        _config.hedge_ratio = readDouble(cfgInventory, "hedgeRatio", 0.5);
        _config.target_inventory = readDouble(cfgInventory, "targetInventory", 0);
        _config.sticky_threshold = readDouble(cfgInventory, "stickyThreshold", 1.0);
        _config.improve_retreat_ratio = readDouble(cfgInventory, "improveRetreatRatio", 2.0);
        _config.max_price_deviation = readDouble(cfgInventory, "maxPriceDeviation", 20.0);
        _config.skew_sensitivity = readDouble(cfgInventory, "skewSensitivity", 2.0);
        _config.aggressive_skew_threshold = readDouble(cfgInventory, "aggressiveSkewThreshold", 0.5);
        _config.one_sided_threshold = readDouble(cfgInventory, "oneSidedThreshold", 0.8);
    }
    
    // 下单错误处理参数（统一处理所有下单错误）
    _config.order_error_threshold = readUInt32(cfg, "orderErrorThreshold", 3);
    
    // 收盘前平仓参数（嵌套在 closeout 节点下）
    WTSVariant* cfgCloseout = cfg->get("closeout");
    if (cfgCloseout) {
        _config.closeout_minutes_before = readUInt32(cfgCloseout, "minutesBefore", 5);
        _config.closeout_flatten_position = readBool(cfgCloseout, "flattenPosition", true);
    }
    _config.close_time = 151500;  // 默认值，on_init 中会从 anchor_code 更新
    
    //------------------------------------------------------------
    // 读取风控参数
    //------------------------------------------------------------
    _config.max_daily_loss = readDouble(cfg, "maxDailyLoss", -50000.0);
    
    //------------------------------------------------------------
    // 读取策略模式开关和性能监控开关
    // 注意：其他模块开关从 coordinator.yaml 读取，此处仅保留策略级配置
    //------------------------------------------------------------
    WTSVariant* cfgModules = cfg->get("modules");
    if (cfgModules) {
        // 策略模式开关（有效配置，不会被 coordinator 覆盖）
        _config.use_market_making = readBool(cfgModules, "useMarketMaking", true);
        _config.use_spread_arbitrage = readBool(cfgModules, "useSpreadArbitrage", false);
        
        // 性能监控开关（策略级配置，coordinator 不管理）
        _config.use_performance_monitor = readBool(cfgModules, "usePerformanceMonitor", false);
        _config.use_performance_analyzer = readBool(cfgModules, "usePerformanceAnalyzer", false);
        
        // 注意：以下模块开关已移至 coordinator.yaml:
        //   - useSpreadOptimizer, useAutoCancel, useMarketState
        //   - useAlphaEngine, useToxicityDetector, useAdaptiveParam
        //   - useOrderBook (SignalAggregator 数据源，自动启用)
    }
    
    //------------------------------------------------------------
    // 读取 FutuRiskMonitor 参数（嵌套在 risk.frequency 节点下）
    //------------------------------------------------------------
    if (cfgRisk) {
        WTSVariant* cfgFrequency = cfgRisk->get("frequency");
        if (cfgFrequency) {
            _config.risk_max_orders_per_sec = readUInt32(cfgFrequency, "maxOrdersPerSec", 50);
            _config.risk_max_cancels_per_sec = readUInt32(cfgFrequency, "maxCancelsPerSec", 30);
            _config.risk_max_trades_per_sec = readUInt32(cfgFrequency, "maxTradesPerSec", 20);
            _config.risk_cooldown_ms = readUInt32(cfgFrequency, "cooldownMs", 30000);
            _config.risk_check_interval_ms = readUInt32(cfgFrequency, "checkIntervalMs", 5000);
            _config.risk_recovery_threshold = readDouble(cfgFrequency, "recoveryThreshold", 0.8);
        }
    }
    
    //------------------------------------------------------------
    // 读取 PerformanceMonitor 参数（嵌套在 performance 节点下）
    //------------------------------------------------------------
    WTSVariant* cfgPerformance = cfg->get("performance");
    if (cfgPerformance) {
        _config.perf_analyzer_window_size = readUInt32(cfgPerformance, "analyzerWindowSize", 100);
        _config.perf_analyzer_risk_free_rate = readDouble(cfgPerformance, "analyzerRiskFreeRate", 0.0);
        _config.perf_monitor_latency_threshold = (uint64_t)readDouble(cfgPerformance, "latencyThreshold", 100000);
    }
    
    //------------------------------------------------------------
    // 注意：以下模块参数已移至独立配置文件:
    //   - SpreadArbitrage -> spread_arbitrage.yaml
    //   - SelfTradePrevention -> coordinator.yaml modules
    //   - AsyncExecutor -> spread_arbitrage.yaml
    //------------------------------------------------------------
    
    //------------------------------------------------------------
    // 参数边界校验（不影响运行时延迟）
    //------------------------------------------------------------
    {
        // Delta 软指标参数校验（用于 skew 和对冲决策，不触发风控）
        if (_config.max_delta <= 0 || _config.max_delta > 100000000) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid maxDelta: {}, expected (0, 100000000]", id(), _config.max_delta);
            return false;
        }
        if (_config.hedge_threshold <= 0 || _config.hedge_threshold > _config.max_delta) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid hedgeThreshold: {}, expected (0, maxDelta={}]", id(), _config.hedge_threshold, _config.max_delta);
            return false;
        }
        
        // 硬风控指标校验
        if (_config.max_exposure <= 0) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid maxExposure: {}, expected > 0", id(), _config.max_exposure);
            return false;
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
        
        // P1优化: 增强 skew 参数校验
        if (_config.skew_sensitivity <= 0 || _config.skew_sensitivity > 10.0) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid skewSensitivity: {}, expected (0, 10]", id(), _config.skew_sensitivity);
            return false;
        }
        if (_config.aggressive_skew_threshold <= 0 || _config.aggressive_skew_threshold >= 1.0) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid aggressiveSkewThreshold: {}, expected (0, 1)", id(), _config.aggressive_skew_threshold);
            return false;
        }
        if (_config.one_sided_threshold <= 0 || _config.one_sided_threshold >= 1.0) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid oneSidedThreshold: {}, expected (0, 1)", id(), _config.one_sided_threshold);
            return false;
        }
        
        // P1优化: Sticky 和价格验证参数校验
        if (_config.sticky_threshold <= 0 || _config.sticky_threshold > 10.0) {
            WTSLogger::warn("UftFutuMmStrategy[{}] stickyThreshold={} out of typical range (0, 10]", id(), _config.sticky_threshold);
        }
        if (_config.max_price_deviation < 0 || _config.max_price_deviation > 100.0) {
            WTSLogger::warn("UftFutuMmStrategy[{}] maxPriceDeviation={} out of typical range [0, 100]", id(), _config.max_price_deviation);
        }
        
        // 注意：Alpha 参数校验已移至 coordinator.yaml 加载时
        // 注意：SpreadArbitrage 参数校验已移至 spread_arbitrage.yaml 加载时
        
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
        coord_cfg.closeout_minutes_before = _config.closeout_minutes_before;
        coord_cfg.close_time = _config.close_time;
        coord_cfg.closeout_flatten_position = _config.closeout_flatten_position;
        coord_cfg.toxicity_cooloff_ms = mp.toxicity_cooloff_ms;
        _coordinator->setConfig(coord_cfg);
    }
    
    // 从 Coordinator 获取模块开关（来自 coordinator.yaml）
    const auto& coord_cfg = _coordinator->getConfig();
    
    // 模块开关（从 coordinator.yaml 读取，而非 config.yaml）
    // 注：use_alpha_engine 和 use_market_state 已移除，由 SignalAggregator 管理
    _config.use_toxicity_detector = coord_cfg.use_toxicity_detector;
    _config.use_spread_optimizer = coord_cfg.use_spread_optimizer;
    _config.use_auto_cancel = coord_cfg.use_auto_cancel;
    _config.use_synthetic_transaction = coord_cfg.use_synthetic_transaction;
    _config.use_adaptive_param = coord_cfg.use_adaptive_params;

    // 注意：use_market_making, use_spread_arbitrage, use_performance_monitor, use_performance_analyzer    // 从 config.yaml 读取，保留原值
        
        WTSLogger::info("Strategy mode: MM={}, Arb={}", 
            _config.use_market_making ? "ON" : "OFF",
            _config.use_spread_arbitrage ? "ON" : "OFF");
    
    //------------------------------------------------------------
    // 1. FutuPortfolio（持仓管理）- 始终需要
    //------------------------------------------------------------
    _portfolio = std::make_unique<FutuPortfolio>();
    
    PortfolioParams portfolio_params;
    portfolio_params.skew_factor = _config.skew_factor;
    portfolio_params.max_skew = _config.max_skew;
    portfolio_params.portfolio_max_delta = _config.max_delta;  // 组合级 Delta 软指标
    portfolio_params.hedge_ratio = _config.hedge_ratio;
    portfolio_params.target_inventory = _config.target_inventory;
    portfolio_params.max_exposure = _config.max_exposure;  // 硬风控
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
        double contract_max_del = (ci.max_delta > 0) ? ci.max_delta : 0;  // 单合约 Delta 软指标，0 表示无限制
        _portfolio->addContract(ci.code, ci.multiplier, ci.tick_size, 1.0, max_pos, contract_max_del, ci.target_position);
    }
    
    WTSLogger::info("FutuPortfolio: portfolioMaxDelta={} (soft), maxExposure={} (hard), maxLoss={}",
        portfolio_params.portfolio_max_delta, portfolio_params.max_exposure, portfolio_params.max_loss);

    //------------------------------------------------------------
    // 2. CorrelationManager (相关性与组合极度套利)
    //------------------------------------------------------------
    if (_config.use_market_making || _config.use_spread_arbitrage)
    {
        _correlation_manager = std::make_unique<CorrelationManager>();
        
        CorrelationConfig corr_cfg;
        const auto& mp = coord_cfg.modules;
        corr_cfg.window_size = mp.correlation_window_size > 0 ? mp.correlation_window_size : 100;
        corr_cfg.min_correlation = mp.correlation_min_correlation;
        corr_cfg.spread_z_threshold = mp.correlation_spread_z_threshold;
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
    if (_config.use_market_making)
    {
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
            qcfg.improve_retreat_ratio = _config.improve_retreat_ratio;
            qcfg.max_price_deviation = _config.max_price_deviation;
            
            // 价格保护参数
            qcfg.price_protection = _config.price_protection;
            qcfg.protect_ticks = _config.protect_ticks;
            
            // 双边报价参数
            qcfg.use_bilateral_quote = _config.use_bilateral_quote;
            qcfg.min_valid_qty = _config.base_qty;  // 有效挂单最小数量 = 基础挂单量
            qcfg.max_obligation_spread = _config.max_obligation_spread;
            
            quoter->init(qcfg);
            // Note: UnifiedOrderTracker will be set after it's created (in section 5)
            _quoters[ci.code] = std::move(quoter);
        }
        
        WTSLogger::info("FutuQuoter: {} quoters initialized, {} levels, baseSpread={}",
            _quoters.size(), _config.num_levels, _config.base_spread);
    }
    else
    {
        WTSLogger::info("FutuQuoter: skipped (market making disabled)");
    }
    
    //------------------------------------------------------------
    // 3. SpreadOptimizer（价差优化器）- 每合约一个 (仅做市)
    //------------------------------------------------------------
    if (_config.use_market_making && _config.use_spread_optimizer)
    {
        const auto& mp = coord_cfg.modules;
        
        // 设置 portfolio_max_delta 用于 delta 敏感 skew 计算（软指标）
        // 这是从策略配置的 max_delta 传递到 SpreadOptimizer
        const_cast<ModuleParams&>(mp).portfolio_max_delta = _config.max_delta;
        
        for (const auto& ci : _contract_infos)
        {
            // 传入 base_spread 和 tick_size 到 SpreadOptimizer
            auto optimizer = FutuComponentFactory::createSpreadOptimizer(
                coord_cfg, ci.code, _config.base_spread, ci.tick_size);
            
            // 设置统一的增强 skew 参数
            optimizer->setSkewEnhancement(_config.skew_sensitivity, _config.aggressive_skew_threshold);
            
            _spread_optimizers[ci.code] = std::move(optimizer);
        }
        
        WTSLogger::info("SpreadOptimizer: {} optimizers, baseSpread={}, phi={}, skewSensitivity={}, maxDelta={} (soft)",
            _spread_optimizers.size(), _config.base_spread, mp.spread_phi, _config.skew_sensitivity, _config.max_delta);
    }
    
    //------------------------------------------------------------
    // 4. UnifiedOrderTracker + AutoCancelPolicy（统一订单跟踪）
    //------------------------------------------------------------
    _order_tracker = std::make_unique<UnifiedOrderTracker>();
    
    {
        const auto& mp = coord_cfg.modules;
        
        UnifiedTrackerConfig tracker_cfg;
        tracker_cfg.max_orders = 32;
        tracker_cfg.max_age_ms = mp.auto_cancel_max_age_ms;
        tracker_cfg.price_deviation = mp.auto_cancel_price_deviation;
        tracker_cfg.sticky_threshold = _config.sticky_threshold;
        tracker_cfg.inv_limit_cooldown_ms = mp.auto_cancel_inventory_cooldown_ms;
        // Self-trade prevention config
        tracker_cfg.stp_enabled = _config.use_spread_arbitrage;
        tracker_cfg.stp_min_price_gap = mp.stp_min_price_gap;
        _order_tracker->setConfig(tracker_cfg);
    }
    
    // 为所有 FutuQuoter 设置共享 tracker
    for (auto& [code, quoter] : _quoters)
    {
        quoter->setOrderTracker(_order_tracker.get());
    }
    
    // 初始化双边报价统计模块
    _bilateral_stats = std::make_unique<BilateralQuoteStats>();
    
    // 从第一个 quoter 的配置读取双边报价统计参数（做市义务要求）
    BilateralStatsConfig bilateral_cfg;
    if (!_quoters.empty())
    {
        const auto& first_quoter_cfg = _quoters.begin()->second->config();
        bilateral_cfg.min_valid_qty = first_quoter_cfg.min_valid_qty;
        bilateral_cfg.max_obligation_spread = first_quoter_cfg.max_obligation_spread;
    }
    _bilateral_stats->setConfig(bilateral_cfg);
    
    WTSLogger::info("BilateralQuoteStats: min_valid_qty={}, max_obligation_spread={:.1f}ticks",
        bilateral_cfg.min_valid_qty, bilateral_cfg.max_obligation_spread);
    
    // 为所有 FutuQuoter 设置双边报价统计
    for (auto& [code, quoter] : _quoters)
    {
        quoter->setBilateralStats(_bilateral_stats.get());
    }
    

    
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
    
    WTSLogger::info("FutuRiskMonitor: maxOrdersPerSec={}, maxCancelsPerSec={}, cooldownMs={}, recoveryThreshold={}",
        _config.risk_max_orders_per_sec, _config.risk_max_cancels_per_sec,
        _config.risk_cooldown_ms, _config.risk_recovery_threshold);
    
    // Register with coordinator
    if (_coordinator) {
        _coordinator->setRiskMonitor(_risk_monitor.get());
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
    if (_config.use_market_making && coord_cfg.use_signal_aggregator)
    {
        const auto& mp = coord_cfg.modules;
        
        SignalAggregatorConfig sig_cfg;
        
        // 信号源开关 - 从 coordinator.yaml modules.signalAggregator 读取
        sig_cfg.use_volatility = mp.signal_use_volatility;
        sig_cfg.use_ofi = mp.signal_use_ofi;
        sig_cfg.use_trade_flow = mp.signal_use_trade_flow;
        sig_cfg.use_book_imbalance = mp.signal_use_book_imbalance;
        sig_cfg.use_momentum = mp.signal_use_momentum;
        sig_cfg.use_lead_lag = mp.signal_use_lead_lag;
        
        // 窗口参数
        sig_cfg.volatility_window = mp.signal_volatility_window;
        sig_cfg.ofi_window = mp.signal_ofi_window;
        sig_cfg.trade_flow_window = mp.signal_trade_flow_window;
        sig_cfg.momentum_window = mp.signal_momentum_window;
        sig_cfg.lead_lag_window = mp.signal_lead_lag_window;
        
        // 信号权重
        sig_cfg.ofi_weight = mp.signal_ofi_weight;
        sig_cfg.trade_weight = mp.signal_trade_weight;
        sig_cfg.book_imbalance_weight = mp.signal_book_imbalance_weight;
        sig_cfg.momentum_weight = mp.signal_momentum_weight;
        sig_cfg.lead_lag_weight = mp.signal_lead_lag_weight;
        sig_cfg.strong_threshold = mp.signal_strong_threshold;
        
        // 阈值参数
        sig_cfg.vol_threshold = mp.signal_vol_threshold;
        sig_cfg.spread_threshold = mp.signal_spread_threshold;
        sig_cfg.book_imbalance_threshold = mp.signal_book_imbalance_threshold;
        sig_cfg.large_trade_threshold = mp.signal_large_trade_threshold;
        sig_cfg.momentum_ema_alpha = mp.signal_momentum_ema_alpha;
        
        for (const auto& ci : _contract_infos)
        {
            auto aggregator = std::make_unique<SignalAggregator>(sig_cfg);
            _signal_aggregators[ci.code] = std::move(aggregator);
        }
        
        WTSLogger::info("SignalAggregator: {} aggregators initialized "
            "(ofi={:.2f}, trade={:.2f}, book={:.2f}, mom={:.2f})",
            _signal_aggregators.size(),
            sig_cfg.ofi_weight, sig_cfg.trade_weight,
            sig_cfg.book_imbalance_weight, sig_cfg.momentum_weight);
    }
    else if (_config.use_market_making)
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
    if (_config.use_market_making && _config.use_toxicity_detector)
    {
        const auto& mp = _coordinator ? _coordinator->getConfig().modules : ModuleParams();
        
        _toxicity_detector = FutuComponentFactory::createToxicFlowDetector(coord_cfg);
        
        // Register with coordinator
        if (_coordinator) {
            _coordinator->setToxicityDetector(_toxicity_detector.get());
        }
        
        WTSLogger::info("ToxicFlowDetector: adverseThreshold={}", 
            mp.toxicity_vpin_threshold);
    }
    else
    {
        WTSLogger::info("ToxicFlowDetector: disabled");
    }
    
    //------------------------------------------------------------
    // 9.5 SelfTradeCalibrator（统一管理自身成交，供毒性检测和综合信号使用）
    //------------------------------------------------------------
    {
        const auto& mp = _coordinator ? _coordinator->getConfig().modules : ModuleParams();
        
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
        const auto& mp = _coordinator ? _coordinator->getConfig().modules : ModuleParams();
        WTSLogger::info("SelfTradeCalibrator: lookbackTrades={}, toxicityWindow={}ms, adverseThreshold={}",
            mp.calibrator_lookback_trades, mp.calibrator_toxicity_window_ms, mp.calibrator_adverse_threshold);
    }
    
    //------------------------------------------------------------
    // 10. AdaptiveParamManager（自适应参数管理器）
    //------------------------------------------------------------
    if (_config.use_adaptive_param)
    {
        const auto& mp = _coordinator ? _coordinator->getConfig().modules : ModuleParams();
        
        _param_manager = FutuComponentFactory::createAdaptiveParamManager(coord_cfg);
        _param_update_interval = mp.adaptive_update_interval;
        WTSLogger::info("AdaptiveParamManager: updateInterval={}, minPhi={}, maxPhi={}",
            mp.adaptive_update_interval, mp.adaptive_min_phi, mp.adaptive_max_phi);
    }
    else
    {
        WTSLogger::info("AdaptiveParamManager: disabled");
    }
    
    //------------------------------------------------------------
    // 11. PerformanceAnalyzer（绩效分析器）
    //------------------------------------------------------------
    if (_config.use_performance_analyzer)
    {
        _perf_analyzer = FutuComponentFactory::createPerformanceAnalyzer(coord_cfg);
        WTSLogger::info("PerformanceAnalyzer: windowSize={}", _config.perf_analyzer_window_size);
    }
    else
    {
        WTSLogger::info("PerformanceAnalyzer: disabled");
    }
    
    //------------------------------------------------------------
    // 12. PerformanceMonitor（性能监控）
    //------------------------------------------------------------
    if (_config.use_performance_monitor)
    {
        _performance_monitor = FutuComponentFactory::createPerformanceMonitor(coord_cfg);
        WTSLogger::info("PerformanceMonitor: latencyThreshold={}ns", _config.perf_monitor_latency_threshold);
    }
    else
    {
        WTSLogger::info("PerformanceMonitor: disabled");
    }
    

    //------------------------------------------------------------
    // 14. SpreadArbitrageManager（跨期价差套利管理器）
    // 独立配置文件: 从 config 中读取 spread_arbitrage_config
    //------------------------------------------------------------
    if (_config.use_spread_arbitrage)
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
    }
    else
    {
        WTSLogger::info("SpreadArbitrageManager: disabled");
    }    
    //------------------------------------------------------------
    // 15. SelfTradePrevention（自成交防护模块）
    //------------------------------------------------------------
    if (_config.use_spread_arbitrage)
    {
        const auto& mp = _coordinator ? _coordinator->getConfig().modules : ModuleParams();
        
        _stp = FutuComponentFactory::createSelfTradePrevention(coord_cfg, _order_tracker.get());
        
        WTSLogger::info("SelfTradePrevention: enabled, strategy=CANCEL_MM, minPriceGap={}, using UnifiedOrderTracker", 
            mp.stp_min_price_gap);
    }
    
    //------------------------------------------------------------
    // 16. AsyncArbitrageExecutor（异步套利执行器）
    //------------------------------------------------------------
    if (_config.use_spread_arbitrage)
    {
        _async_arb = FutuComponentFactory::createAsyncArbitrageExecutor(coord_cfg);
        _async_arb->setArbitrageManager(_spread_arb_manager.get());
        _async_arb->setSelfTradePrevention(_stp.get());
        if (_coordinator) {
            _coordinator->setArbExecutor(_async_arb.get());
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
    }
    else
    {
        WTSLogger::info("AsyncArbitrageExecutor: disabled");
    }
    
    //------------------------------------------------------------
    // 初始化计数器
    //------------------------------------------------------------
    _tick_count = 0;
    // _param_update_interval 已在 AdaptiveParamManager 初始化时设置
    
    // 从配置更新下单错误处理参数
    _order_error_threshold = _config.order_error_threshold;
}

//==========================================================================
// UFT 策略回调
//==========================================================================

void UftFutuMmStrategy::on_init(IUftStraCtx* ctx)
{
    //============================================================
    // 注册热更新参数（运行时可修改，无需重启策略）
    //============================================================
    
    // 报价参数
    _hot_base_spread = ctx->sync_param("base_spread", _config.base_spread);
    _hot_spread_mult = ctx->sync_param("spread_mult", 1.0);
    _hot_base_qty = ctx->sync_param("base_qty", _config.base_qty);
    _hot_skew_factor = ctx->sync_param("skew_factor", _config.skew_factor);
    _hot_max_skew = ctx->sync_param("max_skew", _config.max_skew);
    _hot_max_inventory = ctx->sync_param("max_inventory", _config.max_inventory);
    _hot_target_inventory = ctx->sync_param("target_inventory", _config.target_inventory);
    
    // Delta 软指标
    _hot_max_delta = ctx->sync_param("max_delta", _config.max_delta);
    _hot_hedge_threshold = ctx->sync_param("hedge_threshold", _config.hedge_threshold);
    
    // 硬风控参数
    _hot_max_daily_loss = ctx->sync_param("max_daily_loss", _config.max_daily_loss);
    _hot_order_error_threshold = ctx->sync_param("order_error_threshold", _config.order_error_threshold);
    
    // 毒性冷却时间 (运行时参数)
    _hot_toxicity_cooloff_ms = ctx->sync_param("toxicity_cooloff_ms", _config.toxicity_cooloff_ms);
    
    // 注册参数监控（启用热更新检测）
    ctx->commit_param_watcher();
    
    WTSLogger::info("UftFutuMmStrategy[{}] hot-update params registered", id());
    
    // 默认收盘时间
    _config.close_time = 150000;  // 15:00:00
    
    // 从基础数据管理模块获取合约参数（如果配置文件未指定）
    for (auto& ci : _contract_infos)
    {
        // 将 fullCode 转换为 stdCode 格式调用基础数据 API
        std::string stdCode = fullCodeToStdCode(ci.code);
        WTSCommodityInfo* commInfo = ctx->stra_get_comminfo(stdCode.c_str());
        
        if (commInfo)
        {
            // 缓存品种和交易时段信息（用于 StrategyCoordinator::preCheck 快速查询）
            _session_cache[ci.code] = {commInfo, commInfo->getSessionInfo()};
            WTSLogger::debug("UftFutuMmStrategy[{}] Session cache added: {} -> sessInfo={}", 
                id(), ci.code, (void*)commInfo->getSessionInfo());
            
            // 获取 multiplier 和 tick_size
            if (ci.multiplier <= 0)
                ci.multiplier = commInfo->getVolScale();
            if (ci.tick_size <= 0)
                ci.tick_size = commInfo->getPriceTick();
            
            // 获取收盘时间
            // 注意：对于有夜盘的品种（如 ag），需要找到日盘收盘时间
            // 交易时段按时间顺序排列，最后一个时段可能是夜盘（凌晨收盘）
            // 我们需要找到最后一个非凌晨时段的收盘时间
            if (commInfo->getSessionInfo())
            {
                const auto& sections = commInfo->getSessionInfo()->getTradingSections();
                uint32_t dayCloseTime = 150000;  // 默认日盘收盘时间 15:00:00
                
                // 遍历所有交易时段，找到最后一个非凌晨时段
                for (const auto& section : sections)
                {
                    uint32_t endTime = section.second_raw;  // 原始结束时间 (HHMM)
                    // 排除凌晨时段（夜盘收盘）：0:00-6:00
                    if (endTime > 600 && endTime <= 2359)
                    {
                        // 这是一个日间时段，更新收盘时间
                        dayCloseTime = endTime * 100;  // HHMM -> HHMMSS
                    }
                }
                
                ci.close_time = dayCloseTime;
            }
            else
            {
                ci.close_time = 150000;  // 默认 15:00:00
            }
            
            // 如果是 anchor_code，更新全局收盘时间
            if (ci.code == _config.anchor_code)
            {
                _config.close_time = ci.close_time;
            }
            
            WTSLogger::info("UftFutuMmStrategy[{}] contract {} from base: multiplier={}, tickSize={}, closeTime={}",
                id(), ci.code, ci.multiplier, ci.tick_size, ci.close_time);
        }
        else
        {
            // 使用默认值
            if (ci.multiplier <= 0) ci.multiplier = 1.0;
            if (ci.tick_size <= 0) ci.tick_size = 0.2;
            ci.close_time = 150000;
            WTSLogger::warn("UftFutuMmStrategy[{}] contract {} not found in base data, using defaults",
                id(), ci.code);
        }
    }
    
    // 初始化业务模块（需要合约参数）
    initBusinessModules();
    
    // 输出初始化日志
    WTSLogger::info("UftFutuMmStrategy[{}] initialized: {} contracts, {} levels",
        id(), _contract_infos.size(), _config.num_levels);
    WTSLogger::info("MaxDelta={} (soft), HedgeThreshold={}, MaxSpreadMult={}",
        _config.max_delta, _config.hedge_threshold, _config.max_spread_mult);
    WTSLogger::info("MaxExposure={} (hard), MaxInventory={}, SkewFactor={}, MaxSkew={}",
        _config.max_exposure, _config.max_inventory, _config.skew_factor, _config.max_skew);
    WTSLogger::info("Modules: spreadOpt={}, autoCancel={}, marketData=CORE_MANDATORY",
        _config.use_spread_optimizer, _config.use_auto_cancel);
    
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
    _risk_monitor->resetCloseout();  // 重置收盘前平仓状态
    
    // 重置本地状态
    _blocked_contracts.clear();
    
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
    
    // 初始化双边报价统计
    if (_bilateral_stats)
    {
        uint64_t now = ctx->stra_get_secs() * 1000ULL;
        _bilateral_stats->onSessionStart(now);
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
    
    // 输出双边报价统计结果
    if (_bilateral_stats)
    {
        uint64_t now = ctx->stra_get_secs() * 1000ULL;
        _bilateral_stats->onSessionEnd(now);
        WTSLogger::info("[BILATERAL_STATS] {}", _bilateral_stats->formatString());
    }
    
    WTSLogger::info("UftFutuMmStrategy[{}] session end: {}, Delta: {}", 
        id(), uTDate, _portfolio->getTotalDelta());
}

void UftFutuMmStrategy::on_tick(IUftStraCtx* ctx, const char* stdCode, WTSTickData* tick)
{
    if (!_channel_ready || !tick)
        return;
    
    //============================================================
    // 第一个 tick 到来时的风控检查
    // 如果在 on_channel_ready 时因为缺少价格而暂停，现在恢复
    //============================================================
    if (_last_mid.empty() && _trading_halted && _risk_monitor)
    {
        WTSLogger::info("UftFutuMmStrategy[{}] First tick received, updating prices and checking risk", id());
        
        // 更新所有合约的价格
        bool all_prices_valid = true;
        for (const auto& ci : _contract_infos)
        {
            std::unique_ptr<WTSTickData, void(*)(WTSTickData*)> contractTick(
                ctx->stra_get_last_tick(ci.code.c_str()), 
                [](WTSTickData* p){ if(p) p->release(); }
            );

            if (contractTick)
            {
                double mid = (contractTick->bidprice(0) + contractTick->askprice(0)) / 2.0;
                if (mid > 0)
                {
                    _portfolio->markToMarket(ci.code, mid);
                    _last_mid[ci.code] = mid;  // 填充 _last_mid，避免重复触发
                    WTSLogger::info("UftFutuMmStrategy[{}] First tick: {} price={:.2f}",
                        id(), ci.code, mid);
                }
                else
                {
                    all_prices_valid = false;
                }
            }
            else
            {
                all_prices_valid = false;
            }
        }        
        // 如果所有价格都有效，检查风控并可能恢复交易
        if (all_prices_valid)
        {
            double totalDelta = _portfolio->getTotalDelta();
            WTSLogger::info("UftFutuMmStrategy[{}] Delta after first tick: {:.2f}", id(), totalDelta);
            
            auto violations = _risk_monitor->checkRiskLimits(_portfolio.get());
            if (violations.empty() && _risk_monitor->getHaltCategory() != RiskCategory::IRREVERSIBLE)
            {
                _trading_halted = false;
                _quoting_paused = false;
                _long_blocked = false;
                _short_blocked = false;
                _blocked_contracts.clear();
                _risk_monitor->resumeTrading();
                _risk_monitor->resumeQuoting();
                _risk_monitor->unblockLong();
                _risk_monitor->unblockShort();
                WTSLogger::info("UftFutuMmStrategy[{}] Trading resumed after first tick (risk check passed)", id());
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
                            int32_t reduction = _portfolio->getPositionReductionToLimit(*breached);
                            if (reduction != 0)
                            {
                                // 获取当前价格
                                std::unique_ptr<WTSTickData, void(*)(WTSTickData*)> reduceTick(
                                    ctx->stra_get_last_tick(breached->code.c_str()), 
                                    [](WTSTickData* p){ if(p) p->release(); }
                                );
                                
                                if (reduceTick)
                                {
                                    double price = reduceTick->price();
                                    if (reduction > 0)
                                    {
                                        // 多头超限，卖出平仓
                                        ctx->stra_sell(breached->code.c_str(), price, std::abs(reduction));
                                        WTSLogger::warn("UftFutuMmStrategy[{}] AUTO REDUCE: SELL {} x {} @ {} (position breach)", 
                                            id(), breached->code, reduction, price);
                                    }
                                    else
                                    {
                                        // 空头超限，买入平仓
                                        ctx->stra_buy(breached->code.c_str(), price, std::abs(reduction));
                                        WTSLogger::warn("UftFutuMmStrategy[{}] AUTO REDUCE: BUY {} x {} @ {} (position breach)", 
                                            id(), breached->code, std::abs(reduction), price);
                                    }
                                }
                            }
                        }
                        break;  // 一次只处理一个超限
                    }
                }
                
                if (!has_position_breach)
                {
                    WTSLogger::warn("UftFutuMmStrategy[{}] First tick received but risk still exists, keeping halted", id());
                }
                else
                {
                    WTSLogger::info("UftFutuMmStrategy[{}] Auto position reduction triggered, will retry on next tick", id());
                    
                    // 减仓后重新检查风控状态，如果没有硬指标违规则恢复交易
                    auto newViolations = _risk_monitor->checkRiskLimits(_portfolio.get());
                    bool hasHardBreach = false;
                    for (const auto& v : newViolations)
                    {
                        if (v.type == RiskLimitType::POSITION_NET || 
                            v.type == RiskLimitType::EXPOSURE ||
                            v.type == RiskLimitType::DAILY_LOSS)
                        {
                            hasHardBreach = true;
                            break;
                        }
                    }
                    
                    if (!hasHardBreach && _risk_monitor->getHaltCategory() != RiskCategory::IRREVERSIBLE)
                    {
                        _trading_halted = false;
                        _quoting_paused = false;
                        _long_blocked = false;
                        _short_blocked = false;
                        _blocked_contracts.clear();
                        _risk_monitor->resumeTrading();
                        _risk_monitor->resumeQuoting();
                        _risk_monitor->unblockLong();
                        _risk_monitor->unblockShort();
                        WTSLogger::info("UftFutuMmStrategy[{}] Trading resumed after position reduction (no hard breach)", id());
                    }
                    else
                    {
                        WTSLogger::warn("UftFutuMmStrategy[{}] Risk still exists after reduction, keeping halted", id());
                    }
                }
            }
        }
        else
        {
            WTSLogger::warn("UftFutuMmStrategy[{}] Not all contracts have valid prices yet, keeping paused", id());
        }
    }
    
    if (_correlation_manager) {
        _correlation_manager->onTick(tick);
        
        if (_portfolio) {
            std::string anchor = _config.anchor_code;
            if (stdCode != anchor) {
                double beta = _correlation_manager->getHedgeRatio(stdCode, anchor);
                if (auto* cs = _portfolio->getContract(stdCode)) {
                    cs->hedge_ratio = beta;
                }
            }
        }
    }

    //============================================================
    // 使用 StrategyCoordinator 处理主做市业务逻辑
    //============================================================
    if (_coordinator)
    {
        _coordinator->setTradingHalted(_trading_halted);
        
        auto result = _coordinator->processTick(ctx, stdCode, tick);
        
        // Update state from coordinator
        _trading_halted = _coordinator->isTradingHalted();
        _quoting_paused = _coordinator->isQuotingPaused();
        _long_blocked = _coordinator->isLongBlocked();
        _short_blocked = _coordinator->isShortBlocked();
        _market_state_paused = _coordinator->isMarketStatePaused();
        _toxicity_paused = _coordinator->isToxicityPaused();
        
        // Execute closeout hedge if triggered
        if (result.closeout_executed && _config.closeout_flatten_position)
        {
            executeCloseoutHedge(ctx);
        }
    }
    else
    {
        // Fallback: no coordinator, trigger fail-safe and cancel all orders
        WTSLogger::error("UftFutuMmStrategy[{}] Coordinator is null, triggering FAIL-SAFE!", id());
        _trading_halted = true;
        _quoting_paused = true;
        
        // Cancel all outstanding orders directly via ctx to ensure safety
        for (auto& [code, quoter] : _quoters)
        {
            quoter->cancelAll(ctx);
        }

        return;
    }
    
    //============================================================
    // 跨期价差套利处理 (与做市业务平级，独立处理)
    //============================================================
    if (_spread_arb_manager && _config.use_spread_arbitrage)
    {
        processSpreadArbitrage(ctx, stdCode, tick);
    }
    
    //============================================================
    // 定期更新自适应参数
    //============================================================
    _tick_count++;
    if (_param_manager && _tick_count % _param_update_interval == 0)
    {
        // 记录当前绩效
        PerformanceSample sample;
        sample.realized_pnl = _portfolio->getTotalPnL();
        sample.unrealized_pnl = _portfolio->getTotalUnrealizedPnL();
        sample.volatility = 0;
        // P0-2.1: Volatility is now managed by SignalAggregator
        
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
        uint64_t now = ctx->stra_get_date() * 1000000ULL + ctx->stra_get_time() * 100ULL + ctx->stra_get_secs();
        _risk_monitor->markCloseoutCompleted(now);
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
    // Delta = position * hedge_ratio
    // 需要的手数 = -Delta / hedge_ratio
    double hedgeRatio = anchorState->hedge_ratio;
    
    if (hedgeRatio <= 0)
    {
        WTSLogger::error("UftFutuMmStrategy[{}] Closeout failed: invalid hedgeRatio={}",
            id(), hedgeRatio);
        return;
    }
    
    double hedgeQty = -totalDelta / hedgeRatio;
    int32_t lots = static_cast<int32_t>(std::round(hedgeQty));
    
    if (lots == 0)
    {
        WTSLogger::info("UftFutuMmStrategy[{}] Closeout: Hedge quantity too small (lots=0)", id());
        uint64_t now = ctx->stra_get_date() * 1000000ULL + ctx->stra_get_time() * 100ULL + ctx->stra_get_secs();
        _risk_monitor->markCloseoutCompleted(now);
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
    uint64_t now = ctx->stra_get_date() * 1000000ULL + ctx->stra_get_time() * 100ULL + ctx->stra_get_secs();
    _risk_monitor->markCloseoutCompleted(now);
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
    if (_config.use_spread_optimizer)
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
    
    // 更新 Portfolio 持仓（使用策略本地持仓）
    double current = _portfolio->getPosition(stdCode);
    if (std::abs(current - local_net) > 0.01)
    {
        WTSLogger::debug("UftFutuMmStrategy[{}] Portfolio sync on trade: {} {}->{} (local)",
            id(), stdCode, current, local_net);
        _portfolio->onPositionUpdate(stdCode, local_net);
        _portfolio_ctx_dirty = true;
    }
    
    // 更新 Quoter 订单状态
    for (auto& [code, quoter] : _quoters)
    {
        if (quoter->isMyOrder(localid))
        {
            quoter->onTrade(localid, vol, price);
            break;
        }
    }
    
    // 更新 SpreadOptimizer 成交统计 (P0-2.1: Legacy onFill removed)
    if (_config.use_spread_optimizer)
    {
        // NO-OP
    }
    
    // 从共享订单跟踪器中移除（成交后不再需要跟踪）
    if (_order_tracker)
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
    
    // 改进日志格式：显示开平方向，更容易理解
    // isLong + OPEN -> 开多, isLong + CLOSE -> 平多
    // !isLong + OPEN -> 开空, !isLong + CLOSE -> 平空
    bool isOpen = (offset == '0');  // WOT_OPEN = '0'
    const char* actionStr = "";
    if (isLong) {
        actionStr = isOpen ? "OPEN_LONG" : "CLOSE_LONG";
    } else {
        actionStr = isOpen ? "OPEN_SHORT" : "CLOSE_SHORT";
    }
    WTSLogger::info("UftFutuMmStrategy[{}] TRADE: {} {} {}@{} | Delta: {}",
        id(), stdCode, actionStr, vol, price, _portfolio->getTotalDelta());
    
    // ============================================================
    // 成交后检查风控状态，如果没有硬指标违规则恢复交易
    // ============================================================
    if (_trading_halted && _risk_monitor)
    {
        auto violations = _risk_monitor->checkRiskLimits(_portfolio.get());
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
            _trading_halted = false;
            _quoting_paused = false;
            _long_blocked = false;
            _short_blocked = false;
            _blocked_contracts.clear();
            _risk_monitor->resumeTrading();
            _risk_monitor->resumeQuoting();
            _risk_monitor->unblockLong();
            _risk_monitor->unblockShort();
            WTSLogger::info("UftFutuMmStrategy[{}] Trading resumed after trade (risk check passed)", id());
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
    
    // 计算当前时间戳（毫秒），使用框架时间接口
    // stra_get_time() 返回 HHMMSS，stra_get_secs() 返回毫秒部分
    uint32_t date = ctx->stra_get_date();
    uint32_t time = ctx->stra_get_time();
    uint32_t secs = ctx->stra_get_secs();
    uint32_t h = time / 10000;
    uint32_t m = (time / 100) % 100;
    uint32_t s = time % 100;
    uint64_t now_ms = (static_cast<uint64_t>(h) * 3600 + m * 60 + s) * 1000 + secs;
    now_ms += static_cast<uint64_t>(date) * 86400000ULL;
    
    // 更新 Quoter 订单状态（内部会从 UnifiedOrderTracker 移除）
    // 同时触发双边报价统计更新
    for (auto& [code, quoter] : _quoters)
    {
        if (quoter->isMyOrder(localid))
        {
            quoter->onOrder(localid, isCanceled, leftQty, now_ms);
            break;
        }
    }
    
    // 从自成交防护模块中移除（订单撤销或完全成交）
    if ((isCanceled || leftQty == 0) && _stp)
        _stp->untrackOrder(localid);
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
        // 检查是否是可逆风险
        if (_risk_monitor->getHaltCategory() != RiskCategory::IRREVERSIBLE)
        {
            // ============================================================
            // 关键修复：如果没有有效价格，保持暂停状态
            // 等待第一个 tick 到来时（on_tick 中）再检查风控并恢复交易
            // 这样可以确保 delta 计算准确，避免资金不足等问题
            // ============================================================
            if (!has_valid_price)
            {
                WTSLogger::warn("UftFutuMmStrategy[{}] No valid price for delta calculation, "
                    "keeping trading paused until first tick arrives", id());
                // 保持当前暂停状态，等待第一个 tick
                return;
            }
            
            // 再次检查风险状态，确保风险已经消除
            auto violations = _risk_monitor->checkRiskLimits(_portfolio.get());
            if (violations.empty())
            {
                _trading_halted = false;
                _quoting_paused = false;
                _long_blocked = false;
                _short_blocked = false;
                _blocked_contracts.clear();
                _risk_monitor->resumeTrading();
                _risk_monitor->resumeQuoting();
                _risk_monitor->unblockLong();
                _risk_monitor->unblockShort();
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
                            int32_t reduction = _portfolio->getPositionReductionToLimit(*breached);
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
                                        // 多头超限，卖出平仓
                                        ctx->stra_sell(breached->code.c_str(), price, std::abs(reduction));
                                        WTSLogger::warn("UftFutuMmStrategy[{}] AUTO REDUCE: SELL {} x {} @ {} (position breach)", 
                                            id(), breached->code, reduction, price);
                                    }
                                    else
                                    {
                                        // 空头超限，买入平仓
                                        ctx->stra_buy(breached->code.c_str(), price, std::abs(reduction));
                                        WTSLogger::warn("UftFutuMmStrategy[{}] AUTO REDUCE: BUY {} x {} @ {} (position breach)", 
                                            id(), breached->code, std::abs(reduction), price);
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
                _trading_halted = _risk_monitor->isTradingHalted();
                _quoting_paused = _risk_monitor->isQuotingPaused();
                _long_blocked = _risk_monitor->isLongBlocked();
                _short_blocked = _risk_monitor->isShortBlocked();
            }
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
        
        // ==========================================================
        // 【核心风控与防死循环】 检查是否有挂单，并防止同价位无限撤单替换
        // ==========================================================
        double undone = ctx->stra_get_undone(order.code.c_str());
        if (undone > 0)
        {
            auto it = _arb_last_order_price.find(order.code);
            if (it != _arb_last_order_price.end())
            {
                // 如果挂单仍在，且新信号要求的价格和目前挂单价完全一致，
                // 则直接丢弃新信号，避免触发 WT 底层的【自动撤销并重下相同单】机制，
                // 从而保护订单在交易所撮合队列中的排队优先级。
                if (std::abs(it->second - order.price) < 1e-6)
                {
                    WTSLogger::debug("AsyncArb skipped: {} already has {} pending at identical price {}",
                        order.code, undone, order.price);
                    return;
                }
            }
        }
        
        _arb_last_order_price[order.code] = order.price;

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
        // 报单成功，重置错误计数并恢复报价状态
        if (_order_error_count > 0)
        {
            WTSLogger::info("UftFutuMmStrategy[{}] Order success, resetting error count and resuming quoting", id());
        }
        _order_error_count = 0;
        
        // 恢复报价状态（如果是因为下单错误暂停的）
        if (_quoting_paused && !_trading_halted)
        {
            _quoting_paused = false;
            WTSLogger::info("UftFutuMmStrategy[{}] Quoting resumed after successful order", id());
        }
        return;
    }
    
    // 报单失败，统一处理所有下单错误（不论什么原因）
    std::string errMsg = message ? message : "";
    
    // 判断错误类型：区分可恢复错误和不可恢复错误
    bool is_recoverable = true;
    
    // 平仓手数超过可用持仓 - 这是持仓同步问题，应该重新同步而不是暂停
    if (errMsg.find("平仓") != std::string::npos && 
        (errMsg.find("可用") != std::string::npos || errMsg.find("超过") != std::string::npos))
    {
        WTSLogger::warn("UftFutuMmStrategy[{}] Position sync error detected: {}, will re-sync position on next tick", id(), errMsg);
        is_recoverable = true;
        // 重置错误计数，因为这是持仓同步问题而不是系统问题
        _order_error_count = 0;
        
        // 标记需要持仓重新同步（在下次 on_tick 中处理）
        _portfolio_ctx_dirty = true;
        
        // 暂停报价但不停止交易，等待下次tick时恢复
        _quoting_paused = true;
        return;
    }
    
    // 交易所报单数量限制 - 需要等待冷却
    if (errMsg.find("报单数量") != std::string::npos || errMsg.find("限制") != std::string::npos)
    {
        WTSLogger::warn("UftFutuMmStrategy[{}] Exchange order rate limit hit: {}, cooling down", id(), errMsg);
        is_recoverable = true;
        // 重置错误计数，因为这是频率限制而不是系统错误
        _order_error_count = 0;
        
        // 暂停报价一段时间
        _quoting_paused = true;
        return;
    }
    
    // 所有下单错误统一处理：暂停报价，记录错误
    _order_error_count++;
    
    WTSLogger::error("UftFutuMmStrategy[{}] Order FAILED (count={}/{}): localid={}, error={}",
        id(), _order_error_count, _order_error_threshold, localid, errMsg);
    
    // 连续下单错误达到阈值，暂停交易
    // 修复：标记为可恢复风险，允许自动恢复
    if (_order_error_count >= _order_error_threshold)
    {
        _quoting_paused = true;
        _trading_halted = true;
        
        WTSLogger::error("UftFutuMmStrategy[{}] Trading HALTED due to consecutive order errors (count={}/threshold={})",
            id(), _order_error_count, _order_error_threshold);
        
        // 通知风控模块暂停交易（标记为可恢复风险）
        if (_risk_monitor)
        {
            _risk_monitor->haltTrading(RiskCategory::REVERSIBLE);
        }
        
        // 撤销所有未成交订单，防止进一步错误
        for (auto& [code, quoter] : _quoters)
        {
            quoter->cancelAll(nullptr);
        }
    }
    else
    {
        // 临时暂停报价，等待下次报单成功时重置
        _quoting_paused = true;
        WTSLogger::warn("UftFutuMmStrategy[{}] Quoting temporarily paused due to order error ({}/{})",
            id(), _order_error_count, _order_error_threshold);
    }
}

void UftFutuMmStrategy::on_params_updated()
{
    //============================================================
    // 参数热更新回调
    // 当外部修改共享内存中的参数时，框架自动触发此回调
    // sync_param 返回的指针指向共享内存，值已经更新
    //============================================================
    
    WTSLogger::info("UftFutuMmStrategy[{}] === PARAMS HOT UPDATE ===", id());
    
    // 报价参数
    if (_hot_base_spread) {
        WTSLogger::info("  base_spread: {} -> {}", _config.base_spread, *_hot_base_spread);
        _config.base_spread = *_hot_base_spread;
    }
    if (_hot_base_qty) {
        WTSLogger::info("  base_qty: {} -> {}", _config.base_qty, *_hot_base_qty);
        _config.base_qty = *_hot_base_qty;
    }
    if (_hot_skew_factor) {
        WTSLogger::info("  skew_factor: {} -> {}", _config.skew_factor, *_hot_skew_factor);
        _config.skew_factor = *_hot_skew_factor;
    }
    if (_hot_max_skew) {
        WTSLogger::info("  max_skew: {} -> {}", _config.max_skew, *_hot_max_skew);
        _config.max_skew = *_hot_max_skew;
    }
    if (_hot_max_inventory) {
        WTSLogger::info("  max_inventory: {} -> {}", _config.max_inventory, *_hot_max_inventory);
        _config.max_inventory = *_hot_max_inventory;
    }
    if (_hot_target_inventory) {
        WTSLogger::info("  target_inventory: {} -> {}", _config.target_inventory, *_hot_target_inventory);
        _config.target_inventory = *_hot_target_inventory;
    }
    
    // Delta 软指标
    if (_hot_max_delta) {
        WTSLogger::info("  max_delta: {} -> {}", _config.max_delta, *_hot_max_delta);
        _config.max_delta = *_hot_max_delta;
    }
    if (_hot_hedge_threshold) {
        WTSLogger::info("  hedge_threshold: {} -> {}", _config.hedge_threshold, *_hot_hedge_threshold);
        _config.hedge_threshold = *_hot_hedge_threshold;
    }
    
    // 硬风控参数
    if (_hot_max_daily_loss) {
        WTSLogger::info("  max_daily_loss: {} -> {}", _config.max_daily_loss, *_hot_max_daily_loss);
        _config.max_daily_loss = *_hot_max_daily_loss;
    }
    if (_hot_order_error_threshold) {
        WTSLogger::info("  order_error_threshold: {} -> {}", _config.order_error_threshold, *_hot_order_error_threshold);
        _config.order_error_threshold = *_hot_order_error_threshold;
    }
    
    // 毒性冷却时间 (运行时参数)
    if (_hot_toxicity_cooloff_ms) {
        WTSLogger::info("  toxicity_cooloff_ms: {} -> {}", _config.toxicity_cooloff_ms, *_hot_toxicity_cooloff_ms);
        _config.toxicity_cooloff_ms = *_hot_toxicity_cooloff_ms;
    }
    
    // 同步更新到相关模块
    if (_portfolio)
    {
        PortfolioParams& pp = const_cast<PortfolioParams&>(_portfolio->getParams());
        pp.skew_factor = _config.skew_factor;
        pp.max_skew = _config.max_skew;
        pp.portfolio_max_delta = _config.max_delta;  // 组合级 Delta 软指标
        pp.target_inventory = _config.target_inventory;
        pp.max_loss = _config.max_daily_loss;
    }
    
    WTSLogger::info("UftFutuMmStrategy[{}] === HOT UPDATE COMPLETE ===", id());
}

//==========================================================================
// 重构辅助方法实现
//==========================================================================


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