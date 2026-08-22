#pragma once

#include "SpreadOptimizer.h"
#include "MarketDataContext.h"
#include "ToxicFlowDetector.h"
#include "SelfTradeCalibrator.h"
#include "SelfTradePrevention.h"
#include "PerformanceMonitor.h"
#include "PerformanceAnalyzer.h"
#include "AsyncArbitrageExecutor.h"
#include "StrategyCoordinator.h"

#include <memory>
#include <string>

namespace futu
{

/**
 * @brief Factory for instantiating WtFutuCore trading components
 *
 * Provides centralized dependency injection and instantiation logic,
 * decoupling UftFutuMmStrategy from the concrete implementation setup.
 *
 * All create methods take CoordinatorConfig which contains ModuleParams
 * with all necessary configuration for each component.
 */
class FutuComponentFactory
{
public:
    //==========================================================================
    // Market Making Components
    //==========================================================================

    /// Create SpreadOptimizer with GLFTParams from CoordinatorConfig
    /// @param config Coordinator configuration
    /// @param code Contract code (for logging)
    /// @param base_spread Base spread in ticks (from strategy config)
    /// @param tick_size Tick size for this contract
    static std::unique_ptr<SpreadOptimizer> createSpreadOptimizer(const CoordinatorConfig& config,
                                                                  const std::string& code = "",
                                                                  double base_spread = 2.0,
                                                                  double tick_size = 1.0);

    /// Create MarketDataContext with contract wiring (V8-P0-4)
    /// @param config Coordinator configuration
    ///        (largeTradeThreshold read from modules.signalAggregator.signals.trade_flow, single source)
    /// @param code Contract std code
    /// @param tick_size Contract tick size (single source for Context/Inferer/SignalContext;
    ///        <=0 时告警并跳过装配, derived metrics 将使用默认值)
    static std::unique_ptr<MarketDataContext> createMarketDataContext(const CoordinatorConfig& config,
                                                                      const std::string& code = "",
                                                                      double tick_size = 0.0);

    /// Create ToxicFlowDetector with ToxicityParams from CoordinatorConfig
    static std::unique_ptr<ToxicFlowDetector> createToxicFlowDetector(const CoordinatorConfig& config);

    /// Create SelfTradeCalibrator with SelfTradeCalibratorConfig from CoordinatorConfig
    static std::unique_ptr<SelfTradeCalibrator> createSelfTradeCalibrator(const CoordinatorConfig& config);

    //==========================================================================
    // Adaptive & Performance Components
    //==========================================================================

    /// Create PerformanceMonitor (no config needed, uses defaults)
    static std::unique_ptr<PerformanceMonitor> createPerformanceMonitor(const CoordinatorConfig& config);

    /// Create PerformanceAnalyzer with AnalyzerConfig from CoordinatorConfig
    static std::unique_ptr<PerformanceAnalyzer> createPerformanceAnalyzer(const CoordinatorConfig& config);

    //==========================================================================
    // Arbitrage Components
    //==========================================================================

    /// Create SelfTradePrevention with StpConfig from CoordinatorConfig
    static std::unique_ptr<SelfTradePrevention> createSelfTradePrevention(const CoordinatorConfig& config,
                                                                          UnifiedOrderTracker* tracker = nullptr);

    /// Create AsyncArbitrageExecutor with AsyncArbConfig from CoordinatorConfig
    static std::unique_ptr<AsyncArbitrageExecutor> createAsyncArbitrageExecutor(const CoordinatorConfig& config);
};

} // namespace futu