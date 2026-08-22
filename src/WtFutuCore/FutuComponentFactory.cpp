#include "FutuComponentFactory.h"
#include "SelfTradeCalibrator.h"
#include "../WTSTools/WTSLogger.h"
#include <memory>

namespace futu
{

//==============================================================================
// Market Making Components
//==============================================================================

std::unique_ptr<SpreadOptimizer> FutuComponentFactory::createSpreadOptimizer(const CoordinatorConfig& config,
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
        WTSLogger::warn("FutuComponentFactory: modules.spreadOptimizer missing in coordinator.yaml, "
                        "using default GLFT params (config drift)");
        glft_cfg.base_spread = base_spread;
        glft_cfg.tick_size = tick_size;
        glft_cfg.portfolio_max_delta = mp.portfolio_max_delta;
    }

    auto optimizer = std::make_unique<SpreadOptimizer>(code);
    optimizer->setParams(glft_cfg);

    WTSLogger::debug("SpreadOptimizer[{}]: base_spread={}, tick_size={}, phi={}",
                     code,
                     glft_cfg.base_spread,
                     glft_cfg.tick_size,
                     glft_cfg.phi);

    return optimizer;
}

std::unique_ptr<MarketDataContext> FutuComponentFactory::createMarketDataContext(const CoordinatorConfig& config,
                                                                                const std::string& code,
                                                                                double tick_size)
{
    auto ctx = std::make_unique<MarketDataContext>();

    // V8-P0-4: 大单阈值单一口径 -- 与信号层 TradeFlowSignalSource 读同一 yaml 键
    // (此前 OrderBookStateTracker 默认 tick 0.2 / tracker 阈值 10.0 vs 信号层 50.0 三处分裂)
    double large_trade_threshold = 50.0;
    wtp::WTSVariant* root = config._raw_variant;
    wtp::WTSVariant* modules = root ? root->get("modules") : nullptr;
    wtp::WTSVariant* sig_agg = modules ? modules->get("signalAggregator") : nullptr;
    wtp::WTSVariant* signals = sig_agg ? sig_agg->get("signals") : nullptr;
    wtp::WTSVariant* trade_flow = signals ? signals->get("trade_flow") : nullptr;
    if (trade_flow && trade_flow->has("largeTradeThreshold"))
        large_trade_threshold = trade_flow->getDouble("largeTradeThreshold");
    if (!(large_trade_threshold > 0)) { // 空/类型错/负值回落默认 (V8-R5 语义)
        WTSLogger::warn("FutuComponentFactory: invalid largeTradeThreshold, fallback to 50.0");
        large_trade_threshold = 50.0;
    }

    if (tick_size > 0) {
        // setLargeTradeThreshold 顺带把 tick_size 灌入 TickTransactionInferer
        // (TradeFlowTracker::setConfig 链路), SignalAggregator 经 book.getTickSize()
        // 自动取到同一值 -- Context/Inferer/SignalContext 单一来源
        ctx->setContract(code, tick_size);
        ctx->setLargeTradeThreshold(large_trade_threshold);
        WTSLogger::debug("MarketDataContext[{}]: wired tick_size={}, largeTradeThreshold={}",
                         code,
                         tick_size,
                         large_trade_threshold);
    } else {
        // 首帧 onTick 亦有一次兜底告警 (MarketDataContext::onTick)
        WTSLogger::error("MarketDataContext[{}]: invalid tick_size={}, contract wiring skipped "
                         "(tick/depth/imbalance 数值将用默认值, 结果不可信)",
                         code,
                         tick_size);
    }

    return ctx;
}

std::unique_ptr<ToxicFlowDetector> FutuComponentFactory::createToxicFlowDetector(const CoordinatorConfig& config)
{
    auto detector = std::make_unique<ToxicFlowDetector>();

    wtp::WTSVariant* root = config._raw_variant;
    wtp::WTSVariant* modules = root ? root->get("modules") : nullptr;
    wtp::WTSVariant* tox = modules ? modules->get("toxicityDetector") : nullptr;

    ToxicityParams params;
    if (tox) {
        params = ToxicityParams::fromVariant(tox);
    } else {
        WTSLogger::warn("FutuComponentFactory: modules.toxicityDetector missing in coordinator.yaml, "
                        "using default toxicity params (config drift)");
    }

    detector->setParams(params);
    return detector;
}

std::unique_ptr<SelfTradeCalibrator> FutuComponentFactory::createSelfTradeCalibrator(const CoordinatorConfig& config)
{
    auto calibrator = std::make_unique<SelfTradeCalibrator>();

    wtp::WTSVariant* root = config._raw_variant;
    wtp::WTSVariant* modules = root ? root->get("modules") : nullptr;
    wtp::WTSVariant* cal = modules ? modules->get("selfTradeCalibrator") : nullptr;

    SelfTradeCalibratorConfig cfg;
    if (cal) {
        cfg = SelfTradeCalibratorConfig::fromVariant(cal);
    } else {
        WTSLogger::warn("FutuComponentFactory: modules.selfTradeCalibrator missing in coordinator.yaml, "
                        "using default retreat params (config drift)");
    }

    calibrator->setConfig(cfg);
    return calibrator;
}

//==============================================================================
// Adaptive & Performance Components
//==============================================================================

std::unique_ptr<PerformanceMonitor> FutuComponentFactory::createPerformanceMonitor(const CoordinatorConfig& config)
{
    auto monitor = std::make_unique<PerformanceMonitor>();
    monitor->setLatencyThresholdNs(config.perf_monitor_latency_threshold);
    monitor->setWarnThresholdNs(config.perf_warn_threshold_ns);
    monitor->setCriticalThresholdNs(config.perf_critical_threshold_ns);
    monitor->setLogInterval(config.perf_log_interval);
    return monitor;
}

std::unique_ptr<PerformanceAnalyzer> FutuComponentFactory::createPerformanceAnalyzer(const CoordinatorConfig& config)
{
    auto analyzer = std::make_unique<PerformanceAnalyzer>();

    AnalyzerConfig cfg;

    analyzer->setConfig(cfg);
    return analyzer;
}

//==============================================================================
// Arbitrage Components
//==============================================================================

std::unique_ptr<SelfTradePrevention> FutuComponentFactory::createSelfTradePrevention(const CoordinatorConfig& config,
                                                                                     UnifiedOrderTracker* tracker)
{
    auto stp = std::make_unique<SelfTradePrevention>();

    wtp::WTSVariant* root = config._raw_variant;
    wtp::WTSVariant* modules = root ? root->get("modules") : nullptr;
    wtp::WTSVariant* stp_v = modules ? modules->get("selfTradePrevention") : nullptr;

    StpConfig stp_cfg;
    if (stp_v) {
        stp_cfg = StpConfig::fromVariant(stp_v);
    } else {
        WTSLogger::warn("FutuComponentFactory: modules.selfTradePrevention missing in coordinator.yaml, "
                        "using default STP params (config drift)");
    }

    stp->setConfig(stp_cfg);

    if (tracker) {
        stp->setUnifiedTracker(tracker);
    }

    return stp;
}

std::unique_ptr<AsyncArbitrageExecutor>
FutuComponentFactory::createAsyncArbitrageExecutor(const CoordinatorConfig& config)
{
    auto executor = std::make_unique<AsyncArbitrageExecutor>();

    AsyncArbConfig arb_cfg;
    arb_cfg.enabled = true;

    executor->setConfig(arb_cfg);
    return executor;
}

} // namespace futu
