# WtOptionCore - Option Market Making
# Stage 4: Full migration with Scanners

cmake_minimum_required(VERSION 3.5)
project(WtOptionCore)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPTION_CORE_SOURCES
    # Skeleton
    UftOptionStrategy.cpp
    UftOptionStraFact.cpp
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

    # Vol curves (GSL replaced by WLS3)
    LinearVolCurve.cpp
    GvvVolCurve.cpp

    # Risk + Trading data
    OptionRisk.cpp
    OptionTradingData.cpp
    UnderlyingTradingData.cpp
    OptionOrderInfo.cpp
    FutureOrderInfo.cpp

    # PnL
    PnlTracker.cpp

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
)

set(OPTION_CORE_HEADERS
    UftOptionStrategy.h
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
    OptionList.h
    CompositeOptionPricer.h
    ControllableTradingGrid.h
    check_markets.h
    IScanModule.h
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
)

add_library(WtOptionCore SHARED ${OPTION_CORE_SOURCES} ${OPTION_CORE_HEADERS})

target_include_directories(WtOptionCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../Includes
    ${CMAKE_CURRENT_SOURCE_DIR}/../Share
    ${CMAKE_CURRENT_SOURCE_DIR}/../WtCore
    ${CMAKE_CURRENT_SOURCE_DIR}/../WtUftCore
    ${CMAKE_CURRENT_SOURCE_DIR}/../WTSTools
    ${CMAKE_CURRENT_SOURCE_DIR}/../WTSUtils
    ${CMAKE_CURRENT_SOURCE_DIR}/../build_all/compat_inc
    ${INCS}
)

target_link_libraries(WtOptionCore PUBLIC
    WtUftCore
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
