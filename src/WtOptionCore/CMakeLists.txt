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
    OptionPricer.cpp
    OptionPricer2.cpp
    CompositeOptionPricer.cpp
    GammaScalpOptionPricer.cpp

    # Vol curves (GSL replaced by WLS3)
    LinearVolCurve.cpp
    GvvVolCurve.cpp
    PeriodicCurveFitter.cpp

    # Risk + Trading data
    OptionRisk.cpp
    OptionTradingData.cpp
    UnderlyingTradingData.cpp
    OptionOrderInfo.cpp
    FutureOrderInfo.cpp

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
    OptionAsyncEventProcessor.h
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
    IOptionDataListener.h
    IOptionPricer.h
    OptionPricer.h
    OptionPricer2.h
    IVolCurve.h
    ConstantVolCurve.h
    LinearVolCurve.h
    GvvVolCurve.h
    PeriodicCurveFitter.h
    OptionRisk.h
    OptionTradingData.h
    UnderlyingTradingData.h
    ExpiryTradingData.h
    OptionOrderInfo.h
    FutureOrderInfo.h
    PnlTracker.h
    QuoteStatistics.h
    OptionList.h
    CompositeOptionPricer.h
    GammaScalpOptionPricer.h
    ControllableTradingGrid.h
    check_markets.h
    Scanners/IScanModule.h
    ComboOrders.h
    ScannerInfo.h
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
)

add_library(WtOptionCore SHARED ${OPTION_CORE_SOURCES} ${OPTION_CORE_HEADERS})

target_include_directories(WtOptionCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../Includes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Share
    ${CMAKE_CURRENT_SOURCE_DIR}/../WtCore
    ${CMAKE_CURRENT_SOURCE_DIR}/../WTSTools
    ${CMAKE_CURRENT_SOURCE_DIR}/../WTSUtils
    ${CMAKE_CURRENT_SOURCE_DIR}/../build_all/compat_inc
    ${INCS}
)

target_link_libraries(WtOptionCore PUBLIC
    WtCore
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
