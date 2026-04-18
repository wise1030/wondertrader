#include "FutuComponentFactory.h"
#include "../WTSTools/WTSLogger.h"

namespace futu {

//==============================================================================
// Market Making Components
//==============================================================================



std::unique_ptr<SpreadOptimizer> FutuComponentFactory::createSpreadOptimizer(
    const CoordinatorConfig& config,
    const std::string& code,
    double base_spread,
    double tick_size)
{
    auto optimizer = std::make_unique<SpreadOptimizer>();
    const auto& mp = config.modules;
    
    // Build GLFTParams from ModuleParams
    GLFTParams glft_cfg;
    
    // 核心报价参数（必须正确设置）
    glft_cfg.base_spread = base_spread;     // 基础价差，默认 2.0
    glft_cfg.tick_size = tick_size;         // 合约最小变动价位，默认 1.0
    
    // 价差调整参数
    glft_cfg.vol_sensitivity = mp.spread_vol_sensitivity;
    glft_cfg.depth_sensitivity = mp.spread_depth_sensitivity;
    glft_cfg.min_spread_mult = mp.spread_min_mult;
    glft_cfg.phi = mp.spread_phi;
    glft_cfg.portfolio_skew_weight = mp.spread_portfolio_skew_weight;
    glft_cfg.min_correlation = mp.spread_min_correlation;
    
    // Delta-aware skew parameters
    glft_cfg.portfolio_max_delta = mp.portfolio_max_delta;  // 组合级 Delta 软限制
    glft_cfg.delta_skew_threshold = mp.delta_skew_threshold;  // Default 0.1
    glft_cfg.delta_skew_factor = mp.delta_skew_factor;        // Default 3.0
    glft_cfg.max_skew = mp.spread_max_skew;                   // 最大 skew 限制
    
    optimizer->setParams(glft_cfg);
    
    WTSLogger::debug("SpreadOptimizer[{}]: base_spread={}, tick_size={}, phi={}", 
        code, glft_cfg.base_spread, glft_cfg.tick_size, glft_cfg.phi);
    
    return optimizer;
}

std::unique_ptr<MarketDataContext> FutuComponentFactory::createMarketDataContext(
    const CoordinatorConfig& config)
{
    // MarketDataContext has no config parameters in ModuleParams
    // Uses default configuration
    return std::make_unique<MarketDataContext>();
}

std::unique_ptr<ToxicFlowDetector> FutuComponentFactory::createToxicFlowDetector(
    const CoordinatorConfig& config)
{
    auto detector = std::make_unique<ToxicFlowDetector>();
    const auto& mp = config.modules;
    
    ToxicityParams params;
    params.adverse_threshold = mp.toxicity_vpin_threshold;
    params.vpin_threshold = mp.toxicity_vpin_threshold;
    // Note: ToxicityParams now uses SelfTradeCalibrator for realized toxicity
    
    detector->setParams(params);
    return detector;
}

std::unique_ptr<SelfTradeCalibrator> FutuComponentFactory::createSelfTradeCalibrator(
    const CoordinatorConfig& config)
{
    auto calibrator = std::make_unique<SelfTradeCalibrator>();
    const auto& mp = config.modules;
    
    SelfTradeCalibratorConfig cfg;
    cfg.lookback_trades = mp.calibrator_lookback_trades;
    cfg.toxicity_window_ms = mp.calibrator_toxicity_window_ms;
    cfg.adverse_threshold = mp.calibrator_adverse_threshold;
    cfg.min_samples = mp.synthetic_min_samples;
    
    calibrator->setConfig(cfg);
    return calibrator;
}

//==============================================================================
// Adaptive & Performance Components
//==============================================================================

std::unique_ptr<AdaptiveParamManager> FutuComponentFactory::createAdaptiveParamManager(
    const CoordinatorConfig& config)
{
    auto manager = std::make_unique<AdaptiveParamManager>();
    const auto& mp = config.modules;
    
    // Register default parameters with bounds from config
    ParamBounds bounds;
    
    // SPREAD_BASE
    bounds.min_val = 0.5;
    bounds.max_val = 3.0;
    bounds.step = 0.1;
    bounds.current = 1.0;
    manager->registerParam(ParamType::SPREAD_BASE, bounds);
    
    // SPREAD_PHI
    bounds.min_val = mp.adaptive_min_phi;
    bounds.max_val = mp.adaptive_max_phi;
    bounds.step = 0.01;
    bounds.current = mp.spread_phi;
    manager->registerParam(ParamType::SPREAD_PHI, bounds);
    
    return manager;
}

std::unique_ptr<PerformanceMonitor> FutuComponentFactory::createPerformanceMonitor(
    const CoordinatorConfig& config)
{
    // PerformanceMonitor uses default configuration
    // Latency thresholds can be set via setEnabled() if needed
    return std::make_unique<PerformanceMonitor>();
}

std::unique_ptr<PerformanceAnalyzer> FutuComponentFactory::createPerformanceAnalyzer(
    const CoordinatorConfig& config)
{
    auto analyzer = std::make_unique<PerformanceAnalyzer>();
    
    AnalyzerConfig cfg;
    cfg.history_size = 10000;
    cfg.adverse_threshold = 0.5;
    cfg.strong_alpha_threshold = 0.7;
    
    analyzer->setConfig(cfg);
    return analyzer;
}

//==============================================================================
// Arbitrage Components
//==============================================================================

std::unique_ptr<SelfTradePrevention> FutuComponentFactory::createSelfTradePrevention(
    const CoordinatorConfig& config,
    UnifiedOrderTracker* tracker)
{
    auto stp = std::make_unique<SelfTradePrevention>();
    const auto& mp = config.modules;
    
    StpConfig stp_cfg;
    stp_cfg.enabled = mp.stp_enabled;
    stp_cfg.strategy = StpConfig::Strategy::CANCEL_MM;
    stp_cfg.min_price_gap = mp.stp_min_price_gap;
    stp_cfg.allow_same_price = mp.stp_allow_same_price;
    stp_cfg.price_adjust_ticks = mp.stp_price_adjust_ticks;
    
    stp->setConfig(stp_cfg);
    
    if (tracker) {
        stp->setUnifiedTracker(tracker);
    }
    
    return stp;
}

std::unique_ptr<AsyncArbitrageExecutor> FutuComponentFactory::createAsyncArbitrageExecutor(
    const CoordinatorConfig& config)
{
    auto executor = std::make_unique<AsyncArbitrageExecutor>();
    
    AsyncArbConfig arb_cfg;
    arb_cfg.enabled = true;
    arb_cfg.signal_interval_us = 5000;     // 5ms signal check interval
    arb_cfg.max_wait_us = 10000;           // 10ms max wait
    arb_cfg.ticks_per_signal = 5;          // Check signal every 5 ticks
    arb_cfg.tick_queue_size = 1024;
    arb_cfg.order_queue_size = 256;
    
    executor->setConfig(arb_cfg);
    return executor;
}

} // namespace futu
