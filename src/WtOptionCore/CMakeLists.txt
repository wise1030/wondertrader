# WtOptionCore - Option Trading Core Module
# Refactored to integrate with WonderTrader core infrastructure

cmake_minimum_required(VERSION 3.5)
project(WtOptionCore)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files - Core Option Components
set(OPTION_CORE_SOURCES
    # Pricing and Greeks
    OptionGreeks.cpp
    BlackScholes.cpp
    Black76.cpp
    VolCurve.cpp
    
    # Data Management
    OptionData.cpp
    UnderlyingTradingData.cpp
    OptionGrid.cpp
    
    # Order Management
    BaseOrder.cpp
    OptionOrder.cpp
    OrderManager.cpp
    
    # Risk and Pricing
    OptionRisk.cpp
    CompositeOptionPricer.cpp
    GammaScalpOptionPricer.cpp
    StandardOptionPricer.cpp
    CurveFitter.cpp
    QuoteStatistics.cpp
    
    # Strategy
    WtOptionStrategy.cpp
    
    # Engine and Context (WtCore integration)
    WtOptEngine.cpp
    WtOptContext.cpp
    WtOptTicker.cpp
    
    # Scanners
    Scanners/IScanModule.cpp
    Scanners/SpreadScanner.cpp
    Scanners/ButterflyScanner.cpp
    Scanners/SyntheticFutureScanner.cpp
    Scanners/GarchScanner.cpp
    Scanners/MMScanner.cpp
    Scanners/VolSpreadScanner.cpp
    Scanners/StrikeSpreadScanner.cpp
    Scanners/LowBidsScanner.cpp
    Scanners/OpenScanner.cpp
    Scanners/SimplexScanner.cpp
)

# Header files
set(OPTION_CORE_HEADERS
    OptionTypes.h
    OptionGreeks.h
    BlackScholes.h
    Black76.h
    VolCurve.h
    OptionData.h
    UnderlyingTradingData.h
    OptionGrid.h
    BaseOrder.h
    OptionOrder.h
    OrderManager.h
    OptionRisk.h
    CompositeOptionPricer.h
    StandardOptionPricer.h
    IOptionPricer.h
    CurveFitter.h
    QuoteStatistics.h
    WtOptionStrategy.h
    WtOptEngine.h
    WtOptContext.h
    WtOptTicker.h
    OptStrategy.h
    SignalFactory.h
    ForecastSignal.h
    IAlphaSignal.h
    Scanners/IScanModule.h
    Scanners/SpreadScanner.h
    Scanners/ButterflyScanner.h
    Scanners/SyntheticFutureScanner.h
    Scanners/GarchScanner.h
    Scanners/MMScanner.h
    Scanners/VolSpreadScanner.h
    Scanners/StrikeSpreadScanner.h
    Scanners/LowBidsScanner.h
    Scanners/OpenScanner.h
    Scanners/SimplexScanner.h
)

# Create library
add_library(WtOptionCore SHARED ${OPTION_CORE_SOURCES} ${OPTION_CORE_HEADERS})

# Include directories
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

# Link WtCore dependencies
target_link_libraries(WtOptionCore PUBLIC
    WtCore
    WTSTools
    Share
)

# Find and link Boost for threading
find_package(Boost COMPONENTS thread system QUIET)
if(Boost_FOUND)
    target_link_libraries(WtOptionCore PUBLIC ${Boost_LIBRARIES})
    target_include_directories(WtOptionCore PRIVATE ${Boost_INCLUDE_DIRS})
else()
    # Fallback: link system thread library
    find_package(Threads REQUIRED)
    target_link_libraries(WtOptionCore PUBLIC Threads::Threads)
endif()

# Platform-specific settings
if(WIN32)
    target_compile_definitions(WtOptionCore PRIVATE 
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
    )
endif()

# Install rules
install(TARGETS WtOptionCore
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(FILES ${OPTION_CORE_HEADERS}
    DESTINATION include/WtOptionCore
)

# Unit Tests
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    
    # Test BlackScholes (standalone, no WtCore dependency)
    add_executable(test_blackscholes tests/test_blackscholes.cpp
        BlackScholes.cpp OptionGreeks.cpp)
    target_include_directories(test_blackscholes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME BlackScholesTest COMMAND test_blackscholes)
    
    # Test OptionGrid (now requires WTSTools due to WTSLogger in VolCurve)
    add_executable(test_optiongrid tests/test_optiongrid.cpp
        OptionGreeks.cpp BlackScholes.cpp Black76.cpp OptionData.cpp OptionGrid.cpp VolCurve.cpp StandardOptionPricer.cpp)
    target_include_directories(test_optiongrid PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../WTSTools
        ${CMAKE_CURRENT_SOURCE_DIR}/../Share
        ${CMAKE_CURRENT_SOURCE_DIR}/../build_all/compat_inc
        ${CMAKE_CURRENT_SOURCE_DIR}/../Includes
        ${INCS}
    )
    find_package(Boost COMPONENTS filesystem system QUIET)
    target_link_libraries(test_optiongrid PRIVATE WTSTools Share WTSUtils ${Boost_LIBRARIES})
    add_test(NAME OptionGridTest COMMAND test_optiongrid)
endif()
