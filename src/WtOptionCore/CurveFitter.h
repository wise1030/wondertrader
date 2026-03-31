/*!
 * \file CurveFitter.h
 * \brief Periodic volatility curve fitter
 * 
 * Migrated from longbeach/quantbox/strategy/optioncore/PeriodicCurveFitter.h
 */

#pragma once

#include "OptionTypes.h"
#include "VolCurve.h"
#include "OptionGrid.h"
#include <map>
#include <functional>

namespace wt_option {

/**
 * @brief Fit data for a single expiry
 */
struct FitData {
    uint32_t expiry;
    uint64_t fitTime;
    double atmForward;
    double atmVol;
    DataSet points;         // (strikeDiff, normalizedVol)
    bool isValid;
    
    FitData() : expiry(0), fitTime(0), atmForward(0), atmVol(0), isValid(false) {}
};

/**
 * @brief Curve fitter configuration
 */
struct CurveFitterConfig {
    uint64_t fitStartTime = 93000000;    // 09:30:00 in microseconds
    uint64_t fitEndTime = 150000000;     // 15:00:00 in microseconds
    uint64_t fitPeriod = 60000000;       // 1 minute in microseconds
    double fitThreshold = 0.001;         // Min change to trigger refit
    double decayWindow = 0.5;            // Hours for point decay
    std::map<int32_t, double> goodPointsThresh;  // Days to expiry -> min points
    int32_t traceLevel = 0;
};

/**
 * @brief Fit completed event
 */
using FitCompletedCallback = std::function<void(uint32_t expiry, bool success)>;

/**
 * @brief Periodic volatility curve fitter
 * 
 * Fits volatility curves to market data at regular intervals.
 */
class CurveFitter {
public:
    CurveFitter(OptionGridPtr grid, const CurveFitterConfig& config);
    
    // Main fitting interface
    bool doFit();
    bool fitExpiry(uint32_t expiry);
    
    // Timer callback
    void onTimer(uint64_t timestamp);
    
    // Get fit data
    const FitData& getLastFitData(uint32_t expiry) const;
    uint64_t getLastFitTime() const { return m_lastFitTime; }
    bool isExpiryFitOK(uint32_t expiry) const;
    
    // Configuration
    void setTraceLevel(int32_t level) { m_config.traceLevel = level; }
    void setFitPeriod(uint64_t period) { m_config.fitPeriod = period; }
    
    // Events
    void setFitCompletedCallback(FitCompletedCallback cb) { m_fitCallback = cb; }
    
private:
    bool doFitInternal();
    void collectFitPoints(uint32_t expiry, FitData& data);
    double calculateATMVol(ExpiryData* expiry);
    double calculateATMForward(ExpiryData* expiry);
    bool validatePoint(double strikeDiff, double vol, double atmVol);
    void updateFitData(const FitData& data);
    bool isInFitWindow(uint64_t timestamp) const;
    double getGoodPointsThreshold(int32_t daysToExpiry) const;
    
    OptionGridPtr m_grid;
    CurveFitterConfig m_config;
    
    uint64_t m_lastFitTime;
    std::map<uint32_t, FitData> m_fitData;
    std::map<uint32_t, FitData> m_fitDataPrev;
    std::map<uint32_t, bool> m_expiryFitStatus;
    
    // Optimization: reuse vector to avoid reallocation
    DataSet m_cachedPoints;
    
    FitCompletedCallback m_fitCallback;
};

using CurveFitterPtr = std::shared_ptr<CurveFitter>;

} // namespace wt_option
