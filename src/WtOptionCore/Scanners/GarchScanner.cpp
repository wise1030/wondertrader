/*!
 * \file GarchScanner.cpp
 * \brief GARCH scanner implementation
 */

#include "GarchScanner.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace wt_option {

//=============================================================================
// GarchModel implementation
//=============================================================================

GarchModel::GarchModel(double omega, double alpha, double beta)
    : m_omega(omega)
    , m_alpha(alpha)
    , m_beta(beta)
    , m_variance(omega / (1 - alpha - beta))  // Initial unconditional variance
    , m_longRunVar(omega / (1 - alpha - beta))
    , m_maxLookback(252)  // 1 year of daily data
{
}

void GarchModel::addReturn(double ret) {
    // GARCH(1,1) update:
    // sigma^2_t = omega + alpha * r^2_(t-1) + beta * sigma^2_(t-1)
    
    double shock = ret * ret;
    m_variance = m_omega + m_alpha * shock + m_beta * m_variance;
    
    // Store return for possible future use
    m_returns.push_back(ret);
    if (m_returns.size() > static_cast<size_t>(m_maxLookback)) {
        m_returns.pop_front();
    }
}

double GarchModel::getVolatility() const {
    // Convert daily variance to annualized volatility
    return std::sqrt(m_variance * 252);
}

double GarchModel::forecast(int32_t days) const {
    // Multi-step GARCH forecast
    // E[sigma^2_(t+h)] = omega * sum(k=0 to h-1) (alpha+beta)^k + (alpha+beta)^h * sigma^2_t
    
    double persistence = m_alpha + m_beta;
    
    if (std::abs(persistence - 1.0) < 0.001) {
        // Near unit root - use current variance
        return std::sqrt(m_variance * 252);
    }
    
    double forecastVar = m_longRunVar + 
        std::pow(persistence, days) * (m_variance - m_longRunVar);
    
    return std::sqrt(forecastVar * 252);
}

void GarchModel::reset() {
    m_variance = m_longRunVar;
    m_returns.clear();
}

//=============================================================================
// GarchScanner implementation
//=============================================================================

GarchScanner::GarchScanner(const ScannerConfig& config)
    : IScanModule("GarchScanner")
    , m_garchModel(0.000001, 0.1, 0.85)
    , m_lastScanTime(0)
    , m_isPanicked(false)
    , m_lastPrice(0)
    , m_lastPriceDate(0)
    , m_currentOpenPositions(0)
{
    if (auto* gCfg = dynamic_cast<const GarchScannerConfig*>(&config)) {
        m_config = *gCfg;
        m_garchModel = GarchModel(m_config.omega, m_config.alpha, m_config.beta);
    }
}

void GarchScanner::onStart() {
    log("GarchScanner started");
    m_isPanicked = false;
    m_currentOpenPositions = 0;
    m_garchModel.reset();
}

void GarchScanner::onStop() {
    log("GarchScanner stopped");
}

void GarchScanner::onPanic() {
    m_isPanicked = true;
    log("Panic signal received");
}

void GarchScanner::onUnderlyingUpdate(double price) {
    updateGarchModel(price);
}

void GarchScanner::updateGarchModel(double price) {
    if (m_lastPrice <= 0) {
        m_lastPrice = price;
        return;
    }
    
    // Calculate log return
    double ret = std::log(price / m_lastPrice);
    m_garchModel.addReturn(ret);
    m_lastPrice = price;
}

void GarchScanner::onTick(const OptionGrid* grid) {
    if (!grid || !isEnabled() || m_isPanicked) return;
    
    uint64_t now = getTimestamp();
    
    // Throttle: scan at most once per second
    if (now - m_lastScanTime < 1000000) return;
    m_lastScanTime = now;
    
    // Check position limits
    if (m_currentOpenPositions >= m_config.maxOpenPositions) return;
    
    scanOptions(grid);
}

void GarchScanner::scanOptions(const OptionGrid* grid) {
    double garchVol = m_garchModel.getVolatility();
    
    if (garchVol <= 0.01) {
        // Not enough data for reliable forecast
        return;
    }
    
    const_cast<OptionGrid*>(grid)->forEachOption([this, garchVol, grid](OptionData* option) {
        evaluateOption(option, garchVol, grid);
    });
}

void GarchScanner::evaluateOption(OptionData* option, double garchVol, 
                                   const OptionGrid* grid) {
    if (!option) return;
    
    // Check days to expiry via strike -> expiry
    int32_t daysToExpiry = 0;
    auto strike = option->getStrikeData();
    if (strike) {
        auto expiryData = strike->getExpiryData();
        if (expiryData) daysToExpiry = expiryData->getTradingDays();
    }
    if (daysToExpiry < m_config.minDaysToExpiry || 
        daysToExpiry > m_config.maxDaysToExpiry) {
        return;
    }
    
    double impliedVol = option->getImpliedVol();
    if (impliedVol <= 0) return;
    
    // Forecast GARCH vol for this expiry
    double forecastVol = m_garchModel.forecast(daysToExpiry);
    
    // Calculate vol difference
    double volDiff = impliedVol - forecastVol;
    double volDiffPct = volDiff / forecastVol;
    
    // Check if difference exceeds threshold
    if (std::abs(volDiffPct) >= m_config.volDiffThreshold) {
        ScannerHitEvent event;
        event.option = option;
        event.signal = volDiffPct;
        event.timestamp = getTimestamp();
        
        std::string action = (volDiff > 0) ? "Sell" : "Buy";
        event.reason = action + " vol: IV=" + std::to_string(impliedVol * 100) + "%, " +
                       "GARCH=" + std::to_string(forecastVol * 100) + "%, " +
                       "diff=" + std::to_string(volDiffPct * 100) + "%";
        
        notifyHit(event);
    }
}

// Register scanner
REGISTER_SCANNER("GarchScanner", GarchScanner);

} // namespace wt_option
