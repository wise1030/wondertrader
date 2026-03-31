/*!
 * \file CurveFitter.cpp
 * \brief Periodic volatility curve fitter implementation
 */

#include "CurveFitter.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <chrono>

namespace wt_option {

using namespace std::chrono;

CurveFitter::CurveFitter(OptionGridPtr grid, const CurveFitterConfig& config)
    : m_grid(grid)
    , m_config(config)
    , m_lastFitTime(0)
{
    // Pre-allocate to avoid resize during runtime
    m_cachedPoints.reserve(256); 
}

void CurveFitter::onTimer(uint64_t timestamp) {
    // Check if in fit window
    if (!isInFitWindow(timestamp)) return;
    
    // Check if enough time since last fit
    if (timestamp - m_lastFitTime < m_config.fitPeriod) return;
    
    doFit();
}

bool CurveFitter::doFit() {
    return doFitInternal();
}

bool CurveFitter::doFitInternal() {
    if (!m_grid) return false;
    
    bool anySuccess = false;
    
    // Fit each expiry
    for (uint32_t expiry : m_grid->getExpiryDates()) {
        if (fitExpiry(expiry)) {
            anySuccess = true;
        }
    }
    
    if (anySuccess) {
        m_lastFitTime = duration_cast<microseconds>(
            system_clock::now().time_since_epoch()
        ).count();
    }
    
    return anySuccess;
}

bool CurveFitter::fitExpiry(uint32_t expiry) {
    auto expiryData = m_grid->getExpiry(expiry);
    if (!expiryData) return false;
    
    // Collect fit points
    FitData data;
    data.expiry = expiry;
    collectFitPoints(expiry, data);
    
    if (data.points.size() < 4) {
        if (m_config.traceLevel > 0) {
            std::cout << "CurveFitter: Expiry " << expiry 
                      << " has only " << data.points.size() 
                      << " points, need at least 4" << std::endl;
        }
        m_expiryFitStatus[expiry] = false;
        return false;
    }
    
    // Check threshold
    double threshold = getGoodPointsThreshold(expiryData->getTradingDays());
    if (data.points.size() < threshold) {
        if (m_config.traceLevel > 0) {
            std::cout << "CurveFitter: Expiry " << expiry 
                      << " has " << data.points.size() 
                      << " points, threshold is " << threshold << std::endl;
        }
    }
    
    // Get volatility curve
    auto volCurve = expiryData->getVolCurve();
    if (!volCurve) {
        // Create default GVV curve
        volCurve = std::make_shared<GvvVolCurve>();
        expiryData->setVolCurve(volCurve);
    }
    
    // Set ATM parameters
    volCurve->setATMForward(data.atmForward);
    volCurve->setATMVol(data.atmVol);
    volCurve->setMaturity(expiryData->getTimeToExpiry());
    
    // Perform fit
    bool success = volCurve->fit(data.points);
    
    if (success) {
        data.isValid = true;
        data.fitTime = duration_cast<microseconds>(
            system_clock::now().time_since_epoch()
        ).count();
        
        updateFitData(data);
        m_expiryFitStatus[expiry] = true;
        
        if (m_config.traceLevel > 0) {
            std::cout << "CurveFitter: Expiry " << expiry 
                      << " fit OK with " << data.points.size() 
                      << " points, ATM=" << data.atmVol << std::endl;
        }
        
        if (m_fitCallback) {
            m_fitCallback(expiry, true);
        }
    } else {
        m_expiryFitStatus[expiry] = false;
        
        if (m_config.traceLevel > 0) {
            std::cout << "CurveFitter: Expiry " << expiry 
                      << " fit FAILED" << std::endl;
        }
        
        if (m_fitCallback) {
            m_fitCallback(expiry, false);
        }
    }
    
    return success;
}

void CurveFitter::collectFitPoints(uint32_t expiry, FitData& data) {
    auto expiryData = m_grid->getExpiry(expiry);
    if (!expiryData) return;
    
    // Calculate ATM forward and vol
    data.atmForward = calculateATMForward(expiryData.get());
    data.atmVol = calculateATMVol(expiryData.get());
    
    if (data.atmForward <= 0 || data.atmVol <= 0) return;
    
    // Reuse cached vector to avoid allocation
    m_cachedPoints.clear();
    
    // Collect points from each strike
    for (auto& strikePair : expiryData->getStrikes()) {
        StrikeData* strike = strikePair.second.get();
        double strikePrice = strike->getStrike();
        double strikeDiff = strikePrice - data.atmForward;
        
        // Process call
        auto call = strike->call();
        if (call) {
            double iv = call->getImpliedVol();
            if (validatePoint(strikeDiff, iv, data.atmVol)) {
                double normalizedVol = iv / data.atmVol;
                m_cachedPoints.push_back({strikeDiff, normalizedVol});
            }
        }
        
        // Process put
        auto put = strike->put();
        if (put) {
            double iv = put->getImpliedVol();
            if (validatePoint(strikeDiff, iv, data.atmVol)) {
                double normalizedVol = iv / data.atmVol;
                m_cachedPoints.push_back({strikeDiff, normalizedVol});
            }
        }
    }
    
    // Copy to result (or could change FitData to use shared/cached vector, but this is already better)
    data.points = m_cachedPoints;
    
    // Sort by strike diff
    std::sort(data.points.begin(), data.points.end(),
        [](const DataPoint& a, const DataPoint& b) {
            return std::get<0>(a) < std::get<0>(b);
        });
}

double CurveFitter::calculateATMVol(ExpiryData* expiry) {
    if (!expiry) return 0;
    
    double atmForward = m_grid->getUnderlyingPrice();
    
    // Find two strikes closest to ATM
    StrikeData* lower = nullptr;
    StrikeData* upper = nullptr;
    double lowerDiff = -std::numeric_limits<double>::max();
    double upperDiff = std::numeric_limits<double>::max();
    
    for (auto& pair : expiry->getStrikes()) {
        double diff = pair.first - atmForward;
        if (diff <= 0 && diff > lowerDiff) {
            lowerDiff = diff;
            lower = pair.second.get();
        }
        if (diff > 0 && diff < upperDiff) {
            upperDiff = diff;
            upper = pair.second.get();
        }
    }
    
    // Interpolate ATM vol
    if (lower && upper) {
        double lowerVol = 0, upperVol = 0;
        
        // Use OTM options
        if (lower->put()) lowerVol = lower->put()->getImpliedVol();
        else if (lower->call()) lowerVol = lower->call()->getImpliedVol();
        
        if (upper->call()) upperVol = upper->call()->getImpliedVol();
        else if (upper->put()) upperVol = upper->put()->getImpliedVol();
        
        if (lowerVol > 0 && upperVol > 0) {
            double totalDiff = upperDiff - lowerDiff;
            double weight = -lowerDiff / totalDiff;
            return lowerVol * (1 - weight) + upperVol * weight;
        }
    }
    
    // Fallback: use closest strike
    if (lower && lower->call()) return lower->call()->getImpliedVol();
    if (upper && upper->call()) return upper->call()->getImpliedVol();
    
    return expiry->getATMVol();
}

double CurveFitter::calculateATMForward(ExpiryData* expiry) {
    if (!expiry) return m_grid->getUnderlyingPrice();
    
    // Try to get cached/computed forward from expiry first
    double fwd = expiry->getATMForward();
    if (fwd > 0) return fwd;
    
    // Compute on demand if needed
    fwd = expiry->calculateSyntheticForward();
    if (fwd > 0) return fwd;
    
    // Fallback: use underlying price (cost-of-carry approx)
    return m_grid->getUnderlyingPrice();
}

bool CurveFitter::validatePoint(double strikeDiff, double vol, double atmVol) {
    // Basic validation
    if (vol <= 0 || vol > 5.0) return false;
    if (std::isnan(vol) || std::isinf(vol)) return false;
    
    // Normalized vol should be reasonable
    double normalizedVol = vol / atmVol;
    if (normalizedVol < 0.2 || normalizedVol > 5.0) return false;
    
    return true;
}

void CurveFitter::updateFitData(const FitData& data) {
    m_fitDataPrev[data.expiry] = m_fitData[data.expiry];
    m_fitData[data.expiry] = data;
}

const FitData& CurveFitter::getLastFitData(uint32_t expiry) const {
    static FitData empty;
    auto it = m_fitData.find(expiry);
    return (it != m_fitData.end()) ? it->second : empty;
}

bool CurveFitter::isExpiryFitOK(uint32_t expiry) const {
    auto it = m_expiryFitStatus.find(expiry);
    return (it != m_expiryFitStatus.end()) ? it->second : false;
}

bool CurveFitter::isInFitWindow(uint64_t timestamp) const {
    // Extract time of day from timestamp
    // This is simplified - real implementation would use proper time parsing
    uint64_t timeOfDay = timestamp % (24ULL * 3600 * 1000000);
    
    return timeOfDay >= m_config.fitStartTime && 
           timeOfDay <= m_config.fitEndTime;
}

double CurveFitter::getGoodPointsThreshold(int32_t daysToExpiry) const {
    auto it = m_config.goodPointsThresh.find(daysToExpiry);
    if (it != m_config.goodPointsThresh.end()) {
        return it->second;
    }
    
    // Default: more points needed for longer expiries
    if (daysToExpiry < 7) return 4;
    if (daysToExpiry < 30) return 6;
    return 8;
}

} // namespace wt_option
