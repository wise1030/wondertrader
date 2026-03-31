/*!
 * \file test_optiongrid.cpp
 * \brief Unit tests for OptionGrid and data structures
 */

#include "../OptionGrid.h"
#include "../OptionData.h"
#include "../BlackScholes.h"
#include "../StandardOptionPricer.h"
#include <iostream>
#include <iomanip>

using namespace wt_option;

//=============================================================================
// Test Cases
//=============================================================================

/**
 * Test 1: Create Option Grid
 */
bool testCreateGrid() {
    std::cout << "Test 1: Create Option Grid..." << std::endl;
    
    auto grid = std::make_shared<OptionGrid>("cu");
    
    bool passed = (grid->getUnderlyingCode() == "cu");
    std::cout << "  Underlying: " << grid->getUnderlyingCode() << std::endl;
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 2: Add Options to Grid
 */
bool testAddOptions() {
    std::cout << "Test 2: Add Options to Grid..." << std::endl;
    
    auto grid = std::make_shared<OptionGrid>("cu");
    
    // Add a call option
    OptionInfo callInfo;
    callInfo.code = "cu2502C50000";
    callInfo.underlying = "cu2502";
    callInfo.strike = 50000;
    callInfo.expiry = 20250228;
    callInfo.right = OptionRight::Call;
    callInfo.multiplier = 5;
    
    auto callOpt = grid->addOption(callInfo);
    
    // Add corresponding put
    OptionInfo putInfo = callInfo;
    putInfo.code = "cu2502P50000";
    putInfo.right = OptionRight::Put;
    
    auto putOpt = grid->addOption(putInfo);
    
    std::cout << "  Call: " << callOpt->getCode() << std::endl;
    std::cout << "  Put: " << putOpt->getCode() << std::endl;
    
    // Verify grid structure
    auto expiry = grid->getExpiry(20250228);
    auto strike = expiry->getStrike(50000);
    
    bool passed = (strike->call() == callOpt && strike->put() == putOpt);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 3: Update Market Data
 */
bool testUpdateMarket() {
    std::cout << "Test 3: Update Market Data..." << std::endl;
    
    auto grid = std::make_shared<OptionGrid>("cu");
    grid->setUnderlyingPrice(51000);
    
    OptionInfo info;
    info.code = "cu2502C50000";
    info.underlying = "cu2502";
    info.strike = 50000;
    info.expiry = 20250228;
    info.right = OptionRight::Call;
    
    auto opt = grid->addOption(info);
    
    OptionMarket market;
    market.bid = 1500;
    market.ask = 1600;
    market.last = 1550;
    market.underlyingPrice = 51000;
    opt->updateMarket(market);
    
    std::cout << "  Bid: " << opt->getBid() << std::endl;
    std::cout << "  Ask: " << opt->getAsk() << std::endl;
    std::cout << "  Mid: " << opt->getMid() << std::endl;
    
    bool passed = (opt->getBid() == 1500 && opt->getAsk() == 1600);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 4: Compute Values with Pricer
 */
bool testComputeValues() {
    std::cout << "Test 4: Compute Values with Pricer..." << std::endl;
    
    auto grid = std::make_shared<OptionGrid>("cu");
    grid->setUnderlyingPrice(51000);
    grid->setCurrentDate(20250115);
    
    // Add pricer
    auto pricer = std::make_shared<StandardOptionPricer>();
    grid->setOptionPricer(pricer);
    
    // Add option
    OptionInfo info;
    info.code = "cu2502C50000";
    info.underlying = "cu2502";
    info.strike = 50000;
    info.expiry = 20250228;
    info.right = OptionRight::Call;
    
    auto opt = grid->addOption(info);
    
    // Set expiry data
    auto expiry = grid->getExpiry(20250228);
    expiry->setRiskFreeRate(0.03);
    expiry->setATMVol(0.25);
    
    // Compute values
    grid->computeValues();
    
    std::cout << "  Theo Price: " << opt->getTheoPrice() << std::endl;
    std::cout << "  Delta: " << opt->greeks().delta() << std::endl;
    std::cout << "  Gamma: " << opt->greeks().gamma() << std::endl;
    std::cout << "  Vega: " << opt->greeks().vega() << std::endl;
    
    bool passed = (opt->getTheoPrice() > 0 && opt->greeks().delta() > 0);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 5: Greeks Aggregation
 */
bool testGreeksAggregation() {
    std::cout << "Test 5: Greeks Aggregation..." << std::endl;
    
    auto grid = std::make_shared<OptionGrid>("cu");
    grid->setUnderlyingPrice(50000);
    grid->setCurrentDate(20250115);
    
    auto pricer = std::make_shared<StandardOptionPricer>();
    grid->setOptionPricer(pricer);
    
    // Add call
    OptionInfo callInfo;
    callInfo.code = "cu2502C50000";
    callInfo.underlying = "cu2502";
    callInfo.strike = 50000;
    callInfo.expiry = 20250228;
    callInfo.right = OptionRight::Call;
    
    auto callOpt = grid->addOption(callInfo);
    callOpt->setPosition(10);
    
    // Add put
    OptionInfo putInfo = callInfo;
    putInfo.code = "cu2502P50000";
    putInfo.right = OptionRight::Put;
    
    auto putOpt = grid->addOption(putInfo);
    putOpt->setPosition(-5);
    
    auto expiry = grid->getExpiry(20250228);
    expiry->setRiskFreeRate(0.03);
    expiry->setATMVol(0.25);
    
    grid->computeValues();
    
    auto totalGreeks = grid->getAggregatedGreeks();
    
    std::cout << "  Total Delta: " << totalGreeks.delta() << std::endl;
    std::cout << "  Total Gamma: " << totalGreeks.gamma() << std::endl;
    std::cout << "  Total Vega: " << totalGreeks.vega() << std::endl;
    
    // With long calls and short puts, delta should be positive
    bool passed = (totalGreeks.delta() > 0);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 6: Multiple Expiries
 */
bool testMultipleExpiries() {
    std::cout << "Test 6: Multiple Expiries..." << std::endl;
    
    auto grid = std::make_shared<OptionGrid>("cu");
    
    // Add options for two different expiries
    OptionInfo info1;
    info1.code = "cu2502C50000";
    info1.strike = 50000;
    info1.expiry = 20250228;
    info1.right = OptionRight::Call;
    grid->addOption(info1);
    
    OptionInfo info2;
    info2.code = "cu2503C50000";
    info2.strike = 50000;
    info2.expiry = 20250328;
    info2.right = OptionRight::Call;
    grid->addOption(info2);
    
    auto expiries = grid->getExpiryDates();
    
    std::cout << "  Number of expiries: " << expiries.size() << std::endl;
    for (auto exp : expiries) {
        std::cout << "    Expiry: " << exp << std::endl;
    }
    
    bool passed = (expiries.size() == 2);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 7: Option Greeks Container
 */
bool testOptionGreeks() {
    std::cout << "Test 7: Option Greeks Container..." << std::endl;
    
    OptionGreeks g1;
    g1.delta() = 0.5;
    g1.gamma() = 0.02;
    g1.vega() = 100;
    g1.theta() = -5;
    
    OptionGreeks g2;
    g2.delta() = 0.3;
    g2.gamma() = 0.01;
    g2.vega() = 50;
    g2.theta() = -3;
    
    OptionGreeks sum = g1 + g2;
    
    std::cout << "  g1.delta: " << g1.delta() << std::endl;
    std::cout << "  g2.delta: " << g2.delta() << std::endl;
    std::cout << "  sum.delta: " << sum.delta() << std::endl;
    
    bool passed = (sum.delta() == 0.8 && sum.gamma() == 0.03);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

//=============================================================================
// Main
//=============================================================================

int main() {
    std::cout << "=== Option Grid Unit Tests ===" << std::endl;
    std::cout << std::fixed << std::setprecision(6) << std::endl;
    
    int passed = 0;
    int total = 0;
    
#define RUN_TEST(testFunc) do { total++; if (testFunc()) passed++; } while(0)
    
    RUN_TEST(testCreateGrid);
    RUN_TEST(testAddOptions);
    RUN_TEST(testUpdateMarket);
    RUN_TEST(testComputeValues);
    RUN_TEST(testGreeksAggregation);
    RUN_TEST(testMultipleExpiries);
    RUN_TEST(testOptionGreeks);
    
#undef RUN_TEST
    
    std::cout << std::endl;
    std::cout << "=== Summary ===" << std::endl;
    std::cout << "Passed: " << passed << "/" << total << std::endl;
    
    return (passed == total) ? 0 : 1;
}
