/*!
 * \file test_blackscholes.cpp
 * \brief Unit tests for Black-Scholes pricing model
 */

#include "../BlackScholes.h"
#include "../OptionTypes.h"
#include <iostream>
#include <cmath>
#include <cassert>
#include <iomanip>

using namespace wt_option;

// Test tolerance
const double TOLERANCE = 1e-4;

bool approxEqual(double a, double b, double tol = TOLERANCE) {
    return std::abs(a - b) < tol;
}

//=============================================================================
// Test Cases
//=============================================================================

/**
 * Test 1: ATM Call Option Pricing
 * S = K = 100, r = 5%, T = 1 year, sigma = 20%
 * Expected price ~10.45 (from standard BS formula)
 */
bool testATMCall() {
    std::cout << "Test 1: ATM Call Option Pricing..." << std::endl;
    
    double strike = 100.0;
    double forward = 100.0;
    double vol = 0.20;
    double maturity = 1.0;
    double rate = 0.05;
    double discount = std::exp(-rate * maturity);
    double stdDev = vol * std::sqrt(maturity);
    
    BlackScholes bs(OptionRight::Call, strike, forward, stdDev, discount);
    double price = bs.value();
    
    // Expected price for ATM call with these parameters: ~7.97 (forward model)
    std::cout << "  Price: " << price << std::endl;
    
    // Check price is reasonable (positive and less than forward)
    bool passed = (price > 0 && price < forward);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 2: ATM Put Option Pricing
 * Same parameters as Test 1
 */
bool testATMPut() {
    std::cout << "Test 2: ATM Put Option Pricing..." << std::endl;
    
    double strike = 100.0;
    double forward = 100.0;
    double vol = 0.20;
    double maturity = 1.0;
    double rate = 0.05;
    double discount = std::exp(-rate * maturity);
    double stdDev = vol * std::sqrt(maturity);
    
    BlackScholes bs(OptionRight::Put, strike, forward, stdDev, discount);
    double price = bs.value();
    
    std::cout << "  Price: " << price << std::endl;
    
    bool passed = (price > 0 && price < strike);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 3: Put-Call Parity
 * C - P = D * (F - K) where D = discount factor
 */
bool testPutCallParity() {
    std::cout << "Test 3: Put-Call Parity..." << std::endl;
    
    double strike = 100.0;
    double forward = 105.0;
    double vol = 0.25;
    double maturity = 0.5;
    double rate = 0.03;
    double discount = std::exp(-rate * maturity);
    double stdDev = vol * std::sqrt(maturity);
    
    BlackScholes bsCall(OptionRight::Call, strike, forward, stdDev, discount);
    BlackScholes bsPut(OptionRight::Put, strike, forward, stdDev, discount);
    
    double callPrice = bsCall.value();
    double putPrice = bsPut.value();
    
    double parityLHS = callPrice - putPrice;
    double parityRHS = discount * (forward - strike);
    
    std::cout << "  Call Price: " << callPrice << std::endl;
    std::cout << "  Put Price: " << putPrice << std::endl;
    std::cout << "  C - P = " << parityLHS << std::endl;
    std::cout << "  D*(F-K) = " << parityRHS << std::endl;
    
    bool passed = approxEqual(parityLHS, parityRHS, 0.001);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 4: Delta Bounds
 * Call delta should be between 0 and 1
 * Put delta should be between -1 and 0
 */
bool testDeltaBounds() {
    std::cout << "Test 4: Delta Bounds..." << std::endl;
    
    double strike = 100.0;
    double forward = 100.0;
    double vol = 0.20;
    double maturity = 1.0;
    double rate = 0.05;
    double discount = std::exp(-rate * maturity);
    double stdDev = vol * std::sqrt(maturity);
    
    BlackScholes bsCall(OptionRight::Call, strike, forward, stdDev, discount);
    BlackScholes bsPut(OptionRight::Put, strike, forward, stdDev, discount);
    
    double callDelta = bsCall.delta();
    double putDelta = bsPut.delta();
    
    std::cout << "  Call Delta: " << callDelta << std::endl;
    std::cout << "  Put Delta: " << putDelta << std::endl;
    
    bool passed = (callDelta > 0 && callDelta < 1) && 
                  (putDelta > -1 && putDelta < 0);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 5: Gamma Equality
 * Call gamma should equal Put gamma at same strike
 */
bool testGammaEquality() {
    std::cout << "Test 5: Gamma Equality..." << std::endl;
    
    double strike = 100.0;
    double forward = 100.0;
    double vol = 0.20;
    double maturity = 1.0;
    double rate = 0.05;
    double discount = std::exp(-rate * maturity);
    double stdDev = vol * std::sqrt(maturity);
    
    BlackScholes bsCall(OptionRight::Call, strike, forward, stdDev, discount);
    BlackScholes bsPut(OptionRight::Put, strike, forward, stdDev, discount);
    
    double callGamma = bsCall.gamma();
    double putGamma = bsPut.gamma();
    
    std::cout << "  Call Gamma: " << callGamma << std::endl;
    std::cout << "  Put Gamma: " << putGamma << std::endl;
    
    bool passed = approxEqual(callGamma, putGamma, 0.0001);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 6: Vega Equality
 * Call vega should equal Put vega at same strike
 */
bool testVegaEquality() {
    std::cout << "Test 6: Vega Equality..." << std::endl;
    
    double strike = 100.0;
    double forward = 100.0;
    double vol = 0.20;
    double maturity = 1.0;
    double rate = 0.05;
    double discount = std::exp(-rate * maturity);
    double stdDev = vol * std::sqrt(maturity);
    
    BlackScholes bsCall(OptionRight::Call, strike, forward, stdDev, discount);
    BlackScholes bsPut(OptionRight::Put, strike, forward, stdDev, discount);
    
    double callVega = bsCall.vega(maturity);
    double putVega = bsPut.vega(maturity);
    
    std::cout << "  Call Vega: " << callVega << std::endl;
    std::cout << "  Put Vega: " << putVega << std::endl;
    
    bool passed = approxEqual(callVega, putVega, 0.0001);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 7: Implied Volatility
 * Price an option, then recover IV from that price
 */
bool testImpliedVolatility() {
    std::cout << "Test 7: Implied Volatility..." << std::endl;
    
    double strike = 100.0;
    double forward = 100.0;
    double vol = 0.25;  // Target volatility
    double maturity = 1.0;
    double rate = 0.05;
    double discount = std::exp(-rate * maturity);
    double stdDev = vol * std::sqrt(maturity);
    
    // Price the option
    BlackScholes bs(OptionRight::Call, strike, forward, stdDev, discount);
    double price = bs.value();
    
    // Recover IV
    double recoveredIV = BlackScholes::impliedVolatility(
        OptionRight::Call, price, forward, strike, maturity, discount, 0.2);
    
    std::cout << "  Original Vol: " << vol << std::endl;
    std::cout << "  Option Price: " << price << std::endl;
    std::cout << "  Recovered IV: " << recoveredIV << std::endl;
    
    bool passed = approxEqual(vol, recoveredIV, 0.001);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 8: Greeks Calculation
 * Verify all Greeks are calculated
 */
bool testGreeksCalculation() {
    std::cout << "Test 8: Greeks Calculation..." << std::endl;
    
    double strike = 100.0;
    double forward = 100.0;
    double vol = 0.20;
    double maturity = 1.0;
    double rate = 0.05;
    double discount = std::exp(-rate * maturity);
    double stdDev = vol * std::sqrt(maturity);
    
    BlackScholes bs(OptionRight::Call, strike, forward, stdDev, discount);
    OptionGreeks greeks = bs.calculateGreeks(maturity, rate, vol);
    
    std::cout << "  Delta: " << greeks.delta() << std::endl;
    std::cout << "  Gamma: " << greeks.gamma() << std::endl;
    std::cout << "  Vega: " << greeks.vega() << std::endl;
    std::cout << "  Theta: " << greeks.theta() << std::endl;
    std::cout << "  Rho: " << greeks.rho() << std::endl;
    std::cout << "  Vanna: " << greeks.vanna() << std::endl;
    std::cout << "  Volga: " << greeks.volga() << std::endl;
    
    // Check all Greeks are computed (non-zero for ATM option)
    bool passed = (greeks.delta() != 0 && greeks.gamma() != 0 &&
                   greeks.vega() != 0 && greeks.theta() != 0);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 9: Deep ITM Call
 * Delta should be close to 1
 */
bool testDeepITMCall() {
    std::cout << "Test 9: Deep ITM Call..." << std::endl;
    
    double strike = 80.0;
    double forward = 120.0;  // Deep ITM
    double vol = 0.20;
    double maturity = 0.5;
    double rate = 0.03;
    double discount = std::exp(-rate * maturity);
    double stdDev = vol * std::sqrt(maturity);
    
    BlackScholes bs(OptionRight::Call, strike, forward, stdDev, discount);
    double delta = bs.delta();
    double price = bs.value();
    
    std::cout << "  Price: " << price << std::endl;
    std::cout << "  Delta: " << delta << std::endl;
    
    // Deep ITM delta should be close to 1
    bool passed = (delta > 0.9);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

/**
 * Test 10: Deep OTM Put
 * Delta should be close to 0
 */
bool testDeepOTMPut() {
    std::cout << "Test 10: Deep OTM Put..." << std::endl;
    
    double strike = 80.0;
    double forward = 120.0;  // Deep OTM put
    double vol = 0.20;
    double maturity = 0.5;
    double rate = 0.03;
    double discount = std::exp(-rate * maturity);
    double stdDev = vol * std::sqrt(maturity);
    
    BlackScholes bs(OptionRight::Put, strike, forward, stdDev, discount);
    double delta = bs.delta();
    double price = bs.value();
    
    std::cout << "  Price: " << price << std::endl;
    std::cout << "  Delta: " << delta << std::endl;
    
    // Deep OTM put delta should be close to 0
    bool passed = (delta > -0.1 && delta < 0);
    std::cout << "  Result: " << (passed ? "PASSED" : "FAILED") << std::endl;
    return passed;
}

//=============================================================================
// Main
//=============================================================================

int main() {
    std::cout << "=== Black-Scholes Unit Tests ===" << std::endl;
    std::cout << std::fixed << std::setprecision(6) << std::endl;
    
    int passed = 0;
    int total = 0;
    
#define RUN_TEST(testFunc) do { total++; if (testFunc()) passed++; } while(0)
    
    RUN_TEST(testATMCall);
    RUN_TEST(testATMPut);
    RUN_TEST(testPutCallParity);
    RUN_TEST(testDeltaBounds);
    RUN_TEST(testGammaEquality);
    RUN_TEST(testVegaEquality);
    RUN_TEST(testImpliedVolatility);
    RUN_TEST(testGreeksCalculation);
    RUN_TEST(testDeepITMCall);
    RUN_TEST(testDeepOTMPut);
    
#undef RUN_TEST
    
    std::cout << std::endl;
    std::cout << "=== Summary ===" << std::endl;
    std::cout << "Passed: " << passed << "/" << total << std::endl;
    
    return (passed == total) ? 0 : 1;
}
