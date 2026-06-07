#include "FutuComponentFactory.h"
#include "SelfTradeCalibrator.h"
#include "../WTSTools/WTSLogger.h"
#include <memory>

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
    const auto& mp = config.modules;
    wtp::WTSVariant* root = config._raw_variant;
    wtp::WTSVariant* modules = root ? root->get("modules") : nullptr;
    wtp::WTSVariant* spread = modules ? modules->get("spreadOptimizer") : nullptr;
    
    GLFTParams glft_cfg;
    if (spread) {
        glft_cfg = GLFTParams::fromVariant(spread, base_spread, tick_size, mp.portfolio_max_delta);
    } else {
        glft_cfg.base_spread = base_spread;
        glft_cfg.tick_size = tick_size;
        glft_cfg.portfolio_max_delta = mp.portfolio_max_delta;
    }
    
    auto optimizer = std::make_unique<SpreadOptimizer>(code);
    optimizer->setParams(glft_cfg);
    
    WTSLogger::debug("SpreadOptimizer[{}]: base_spread={}, tick_size={}, phi={}", 
        code, glft_cfg.base_spread, glft_cfg.tick_size, glft_cfg.phi);
    
    return optimizer;
}

std::unique_ptr<MarketDataContext> FutuComponentFactory::createMarketDataContext(
    const CoordinatorConfig& config)
{
    return std::make_unique<MarketDataContext>();
}

std::unique_ptr<ToxicFlowDetector> FutuComponentFactory::createToxicFlowDetector(
    const CoordinatorConfig& config)
{
    auto detector = std::make_unique<ToxicFlowDetector>();
    
    wtp::WTSVariant* root = config._raw_variant;
    wtp::WTSVariant* modules = root ? root->get("modules") : nullptr;
    wtp::WTSVariant* tox = modules ? modules->get("toxicityDetector") : nullptr;
    
    ToxicityParams params;
    if (tox) {
        params = ToxicityParams::fromVariant(tox);
    }
    
    detector->setParams(params);
    return detector;
}

std::unique_ptr<SelfTradeCalibrator> FutuComponentFactory::createSelfTradeCalibrator(
    const CoordinatorConfig& config)
{
    auto calibrator = std::make_unique<SelfTradeCalibrator>();
    
    wtp::WTSVariant* root = config._raw_variant;
    wtp::WTSVariant* modules = root ? root->get("modules") : nullptr;
    wtp::WTSVariant* cal = modules ? modules->get("selfTradeCalibrator") : nullptr;
    
    SelfTradeCalibratorConfig cfg;
    if (cal) {
        cfg = SelfTradeCalibratorConfig::fromVariant(cal);
    }
    
    calibrator->setConfig(cfg);
    return calibrator;
}

//==============================================================================
// Adaptive & Performance Components
//==============================================================================

std::unique_ptr<PerformanceMonitor> FutuComponentFactory::createPerformanceMonitor(
    const CoordinatorConfig& config)
{
    auto monitor = std::make_unique<PerformanceMonitor>();
    monitor->setLatencyThresholdNs(config.perf_monitor_latency_threshold);
    monitor->setWarnThresholdNs(config.perf_warn_threshold_ns);
    monitor->setCriticalThresholdNs(config.perf_critical_threshold_ns);
    monitor->setLogInterval(config.perf_log_interval);
    return monitor;
}

std::unique_ptr<PerformanceAnalyzer> FutuComponentFactory::createPerformanceAnalyzer(
    const CoordinatorConfig& config)
{
    auto analyzer = std::make_unique<PerformanceAnalyzer>();
    
    AnalyzerConfig cfg;
    
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
    
    wtp::WTSVariant* root = config._raw_variant;
    wtp::WTSVariant* modules = root ? root->get("modules") : nullptr;
    wtp::WTSVariant* stp_v = modules ? modules->get("selfTradePrevention") : nullptr;
    
    StpConfig stp_cfg;
    if (stp_v) {
        stp_cfg = StpConfig::fromVariant(stp_v);
    }
    
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
    
    executor->setConfig(arb_cfg);
    return executor;
}

} // namespace futu
