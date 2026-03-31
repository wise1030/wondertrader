/*!
 * \file OptionTypes.h
 * \brief Core type definitions for option trading module
 * 
 * Migrated from longbeach/quantbox to WonderTrader framework
 */

#pragma once

#include <stdint.h>
#include <string>
#include <memory>
#include <cmath>

namespace wt_option {

/**
 * @brief Option right type (Call or Put)
 */
enum class OptionRight : uint8_t {
    Call = 0,
    Put = 1
};

/**
 * @brief Greek type enumeration
 */
enum class GreekType : uint8_t {
    Delta = 0,
    Gamma,
    Vega,
    VegaTW,     // Time-weighted vega
    Theta,
    Rho,
    Vanna,
    Volga,
    Unknown
};

/**
 * @brief Option contract information
 */
struct OptionInfo {
    std::string code;           // Option contract code
    std::string underlying;     // Underlying contract code
    double strike;              // Strike price
    uint32_t expiry;            // Expiry date (YYYYMMDD)
    OptionRight right;          // Call or Put
    double multiplier;          // Contract multiplier
    double tickSize;            // Minimum price increment
    
    OptionInfo() : strike(0.0), expiry(0), right(OptionRight::Call), multiplier(1.0), tickSize(0.2) {}
};

/**
 * @brief Option market data
 */
struct OptionMarket {
    double bid;                 // Best bid price
    double ask;                 // Best ask price
    double last;                // Last trade price
    double bidSize;             // Bid size
    double askSize;             // Ask size
    double underlyingPrice;     // Underlying price
    double volume;              // Trading volume
    double openInterest;        // Open interest
    uint64_t updateTime;        // Update timestamp (microseconds)
    
    OptionMarket() : bid(0), ask(0), last(0), bidSize(0), askSize(0),
                     underlyingPrice(0), volume(0), openInterest(0), updateTime(0) {}
    
    double mid() const { return (bid + ask) / 2.0; }
};

/**
 * @brief Expiry information
 */
struct ExpiryInfo {
    uint32_t expiryDate;        // Expiry date (YYYYMMDD)
    double timeToExpiry;        // Time to expiry in years
    uint32_t tradingDays;       // Trading days to expiry
    double riskFreeRate;        // Risk-free rate
    double dividendYield;       // Dividend yield
    
    ExpiryInfo() : expiryDate(0), timeToExpiry(0), tradingDays(0),
                   riskFreeRate(0), dividendYield(0) {}
};

// Math constants
constexpr double PI = 3.14159265358979323846;
constexpr double SQRT_2PI = 2.5066282746310002;
constexpr double INV_SQRT_2PI = 0.3989422804014327;

// Tolerance for numerical comparisons
constexpr double EPSILON = 1e-10;
constexpr double VOL_EPSILON = 1e-7;

// Max/min values for edge cases
constexpr double MAX_VOL = 5.0;         // 500% volatility max
constexpr double MIN_VOL = 0.001;       // 0.1% volatility min
constexpr double MAX_TIME = 10.0;       // 10 years max
constexpr double MIN_TIME = 1e-6;       // ~30 seconds min

/**
 * @brief Check if two doubles are approximately equal
 */
inline bool close(double a, double b, double eps = EPSILON) {
    return std::fabs(a - b) < eps;
}

/**
 * @brief Standard normal cumulative distribution function (CDF)
 */
double normalCDF(double x);

/**
 * @brief Standard normal probability density function (PDF)
 */
double normalPDF(double x);

// Smart pointer typedefs
using OptionInfoPtr = std::shared_ptr<OptionInfo>;
using OptionMarketPtr = std::shared_ptr<OptionMarket>;
using ExpiryInfoPtr = std::shared_ptr<ExpiryInfo>;

} // namespace wt_option
