# WtOptionCore - Option Market Making
# Stage 4: Full migration with Scanners

cmake_minimum_required(VERSION 3.5)
project(WtOptionCore)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPTION_CORE_SOURCES
    # Skeleton
    HftOptionStrategy.cpp
    HftOptionStraFact.cpp
    OptionAsyncEventProcessor.cpp
    OptionScanCoordinator.cpp
    OptionHedgeCoordinator.cpp
    OptionConfigLoader.cpp
    OptionExpiryLifecycle.cpp
    OptionCommandHandlers.cpp

    # Math
    OptionGreeks.cpp
    BlackCalc.cpp
    StrikeData.cpp
    BlackImpliedCalculator.cpp

    # Data
    ExpiryData.cpp
    OptionData.cpp
    OptionRiskData.cpp
    OptionExpiryGreeks.cpp
    OptionGrid.cpp
    OptionTradingGrid.cpp

    # Pricing
    OptionPricer2.cpp
    CompositeOptionPricer.cpp
    GammaScalpOptionPricer.cpp

    # Vol curves (GSL replaced by WLS3)
    LinearVolCurve.cpp
    GvvVolCurve.cpp
    PeriodicCurveFitter.cpp
    PowerVolCurve.cpp

    # Strike grids (quantbox optioncore absorption, R3)
    StrikeFinders.cpp

    # Risk + Trading data
    OptionRisk.cpp
    StressAnalyzer.cpp
    OptionTradingData.cpp
    UnderlyingTradingData.cpp

    # PnL
    PnlTracker.cpp

    # Quote statistics
    QuoteStatistics.cpp

    # B7: Attribute publishing for monitoring
    AttributePublisher.cpp

    # B9: Expiration simulator
    ExpirationSimulator.cpp

    # B10: Option value writer
    OptionValueWriter.cpp

    # Execution scheduling
    ControllableTradingGrid.cpp
    OptionQuoteManager.cpp
    check_markets.cpp

    # Scanners
    Scanners/MMScanner.cpp
    Scanners/ButterflyScanner.cpp
    Scanners/VolSpreadScanner.cpp
    Scanners/OpenScanner.cpp
    Scanners/StrikeSpreadScanner.cpp
    Scanners/SyntheticFutureScanner.cpp
    Scanners/GarchScanner.cpp
    Scanners/LowBidsScanner.cpp
    Scanners/SimplexScanner.cpp

    # Scanner base (IScanModule implementation + ScannerFactory)
    Scanners/IScanModule.cpp

    # Combo order execution (multi-leg scanner orders)
    ComboOrders.cpp
    OptionOrder.cpp
    BaseOrder.cpp

    # Signals
    Signals/SignalFactory.cpp
    Signals/AlphaSignals.cpp
    Signals/RiskSignals.cpp

    # Enhancement modules (borrowed from quantbox)
    RiskFilterChain.cpp
    PositionOffsetMgr.cpp
    PositionGuard.cpp
    FillPriceChecker.cpp
    RiskLimitsEx.cpp
)

set(OPTION_CORE_HEADERS
    HftOptionStrategy.h
    OptionScanCoordinator.h
    OptionHedgeCoordinator.h
    OptionExpiryLifecycle.h
    OptionCommandHandlers.h
    OptionPricerMode.h
    OptionAsyncEventProcessor.h
    OptionTimeService.h
    OptionConfigLoader.h
    LatencyHistogram.h
    SingleFlightTaskRunner.h
    AsyncVolFitter.h
    OrderGateway.h
    PositionBook.h
    optioncoretypes.h
    OptionGreeks.h
    BlackCalc.h
    BlackImpliedCalculator.h
    OptionValues.h
    StrikeData.h
    WLS3.h
    ExpiryData.h
    OptionData.h
    OptionRiskData.h
    OptionRiskDataListener.h
    OptionExpiryGreeks.h
    OptionGrid.h
    OptionTradingGrid.h
    OptionQuoteManager.h
    IOptionGrid.h
    IOptionGridListener.h
    IOptionPricer.h
    OptionPricer2.h
    IVolCurve.h
    LinearVolCurve.h
    GvvVolCurve.h
    PeriodicCurveFitter.h
    PowerVolCurve.h
    IStrikeFinder.h
    CffexIOStrikeFinder.h
    DceMOStrikeFinder.h
    OptionRisk.h
    StressAnalyzer.h
    OptionTradingData.h
    InstrumentTradingDataBase.h
    UnderlyingTradingData.h
    ExpiryTradingData.h
    PnlTracker.h
    QuoteStatistics.h
    OptionList.h
    CompositeOptionPricer.h
    GammaScalpOptionPricer.h
    ControllableTradingGrid.h
    check_markets.h
    Scanners/IScanModule.h
    Scanners/EdgeScannerBase.h
    ComboOrders.h
    Scanners/MMScanner.h
    Scanners/ButterflyScanner.h
    Scanners/VolSpreadScanner.h
    Scanners/OpenScanner.h
    Scanners/StrikeSpreadScanner.h
    Scanners/SyntheticFutureScanner.h
    Scanners/GarchScanner.h
    Scanners/LowBidsScanner.h
    Scanners/SimplexScanner.h

    # Signal framework
    Signals/ISignal.h
    Signals/SignalFactory.h
    Signals/AlphaSignals.h
    Signals/RiskSignals.h

    # Enhancement modules
    RiskFilterChain.h
    PositionOffsetMgr.h
    PositionGuard.h
    FillPriceChecker.h
    RiskLimitsEx.h
    IssuedOrderRegistry.h
    CommandBus.h
)

add_library(WtOptionCore SHARED ${OPTION_CORE_SOURCES} ${OPTION_CORE_HEADERS})

# Warnings (audit 2026-11-13 v2 T1-7): full -Wall/-Wextra on the library in
# every build type.  Two deliberate, documented relaxations:
#  - Framework interface headers (Includes/Share/WTSTools/WTSUtils) are
#    SYSTEM includes: 687 of 1132 initial warnings were attributed to them,
#    and the module workflow forbids editing framework files — -isystem
#    silences header-attributed diagnostics without touching them.
#  - -Wno-unused-parameter: 439 of the 445 own-code warnings were unused
#    parameters in virtual/interface implementations whose names exist for
#    documentation (callbacks, vol-curve operator() args). Zero-value churn
#    at that scale; the real classes (unused/dead variables etc.) are fixed
#    to zero instead.
target_compile_options(WtOptionCore PRIVATE -Wall -Wextra -Wno-unused-parameter)

# PERF: Aggressive optimization for HFT low-latency
# -O3: maximum optimization level
# -march=x86-64-v3: AVX2/FMA without binding to the build host CPU
# -flto: link-time optimization for cross-TU inlining
# -ffinite-math-only: assume no NaN/Inf (faster sqrt/exp), but we use NAN as sentinel
#   so we use -fno-math-errno instead which only removes errno overhead from sqrt
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(WtOptionCore PRIVATE
        -O3
        # x86-64-v3 (AVX2/FMA) keeps the fast path but avoids binding the
        # shipped .so to the build host's exact CPU (native can SIGILL on
        # another machine).  Use an explicit target matrix if v3 is too new.
        -march=x86-64-v3
        -flto
        -fno-math-errno
        -fno-trapping-math
    )
    target_link_options(WtOptionCore PRIVATE -flto)
endif()

target_include_directories(WtOptionCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../build_all/compat_inc
    ${INCS}
)
target_include_directories(WtOptionCore SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../Includes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Share
    ${CMAKE_CURRENT_SOURCE_DIR}/../WTSTools
    ${CMAKE_CURRENT_SOURCE_DIR}/../WTSUtils
)

# 插件边界: WtOptionCore 是 HFT 引擎上的策略插件, 头文件只依赖 Includes/*;
# libWtOptionCore.so 的 nm -D -u 中无任何 Wt/wtp 符号, 故不链接 WtCore。
target_link_libraries(WtOptionCore PUBLIC
    WTSTools
    Share
    boost_filesystem
    boost_system
)

# OpenMP support (replaces TBB)
find_package(OpenMP QUIET)
if(OpenMP_CXX_FOUND)
    target_link_libraries(WtOptionCore PUBLIC OpenMP::OpenMP_CXX)
else()
    target_compile_options(WtOptionCore PRIVATE -fopenmp)
    target_link_libraries(WtOptionCore PUBLIC gomp)
endif()

find_package(Boost COMPONENTS thread system QUIET)
if(Boost_FOUND)
    target_link_libraries(WtOptionCore PUBLIC ${Boost_LIBRARIES})
    target_include_directories(WtOptionCore PRIVATE ${Boost_INCLUDE_DIRS})
else()
    find_package(Threads REQUIRED)
    target_link_libraries(WtOptionCore PUBLIC Threads::Threads)
endif()

if(WIN32)
    target_compile_definitions(WtOptionCore PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
endif()

install(TARGETS WtOptionCore ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(FILES ${OPTION_CORE_HEADERS} DESTINATION include/WtOptionCore)

# ============================================================================
# Unit Tests
# ============================================================================
enable_testing()

# Test: CTG ranking + risk
add_executable(test_ctg_ranking tests/test_ctg_ranking.cpp)
target_link_libraries(test_ctg_ranking PRIVATE WtOptionCore WtCore WTSTools WTSUtils)
target_include_directories(test_ctg_ranking PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_ctg_ranking COMMAND test_ctg_ranking)

# Test: Enhancement modules (RiskFilterChain, PositionOffsetMgr, PositionGuard, FillPriceChecker, RiskLimitsEx)
add_executable(test_enhancements tests/test_enhancements.cpp)
target_link_libraries(test_enhancements PRIVATE WtOptionCore WtCore WTSTools WTSUtils)
target_include_directories(test_enhancements PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_enhancements COMMAND test_enhancements)

# Test: Scanner factory/lifecycle/signal acceptance
add_executable(test_scanners tests/test_scanners.cpp)
target_link_libraries(test_scanners PRIVATE WtOptionCore WtCore WTSTools WTSUtils)
target_include_directories(test_scanners PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_scanners COMMAND test_scanners)

# Test: Phase 2 stress analyzer
add_executable(test_stress tests/test_stress.cpp)
target_link_libraries(test_stress PRIVATE WtOptionCore WtCore WTSTools WTSUtils)
target_include_directories(test_stress PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_stress COMMAND test_stress)

# Test: Phase 2 hedge coordinator (in-flight/cooldown/sizing)
add_executable(test_hedge_coordinator tests/test_hedge_coordinator.cpp)
target_link_libraries(test_hedge_coordinator PRIVATE WtOptionCore WtCore WTSTools WTSUtils)
target_include_directories(test_hedge_coordinator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_hedge_coordinator COMMAND test_hedge_coordinator)

# Test: Phase 2 command handlers
add_executable(test_command_handlers tests/test_command_handlers.cpp)
target_link_libraries(test_command_handlers PRIVATE WtOptionCore WtCore WTSTools WTSUtils)
target_include_directories(test_command_handlers PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_command_handlers COMMAND test_command_handlers)

# Test: Phase 0 async event exactly-once / control wakeup regressions
add_executable(test_async_event_processor tests/test_async_event_processor.cpp)
target_link_libraries(test_async_event_processor PRIVATE WtOptionCore WtCore WTSTools WTSUtils)
target_include_directories(test_async_event_processor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_async_event_processor COMMAND test_async_event_processor)

# Synthetic replay-style benchmark harness (not registered as a pass/fail test)
add_executable(test_replay_benchmark tests/test_replay_benchmark.cpp)
target_link_libraries(test_replay_benchmark PRIVATE WtOptionCore)
target_include_directories(test_replay_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
