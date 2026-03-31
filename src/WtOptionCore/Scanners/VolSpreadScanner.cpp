/*!
 * \file VolSpreadScanner.cpp
 * \brief Volatility spread scanner implementation
 */

#include "VolSpreadScanner.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace wt_option {

VolSpreadScanner::VolSpreadScanner(const ScannerConfig& config)
    : IScanModule("VolSpreadScanner")
    , m_lastScanTime(0)
    , m_isPanicked(false)
    , m_currentOpenPositions(0)
{
    if (auto* vsCfg = dynamic_cast<const VolSpreadScannerConfig*>(&config)) {
        m_config = *vsCfg;
    }
}

void VolSpreadScanner::onStart() {
    log("VolSpreadScanner started");
    m_isPanicked = false;
    m_currentOpenPositions = 0;
}

void VolSpreadScanner::onStop() {
    log("VolSpreadScanner stopped");
}

void VolSpreadScanner::onPanic() {
    m_isPanicked = true;
    log("Panic signal received");
}

void VolSpreadScanner::onTick(const OptionGrid* grid) {
    if (!grid || !isEnabled() || m_isPanicked) return;
    
    uint64_t now = getTimestamp();
    
    // Throttle: scan at most once per second
    if (now - m_lastScanTime < 1000000) return;
    m_lastScanTime = now;
    
    // Check position limits
    if (m_currentOpenPositions >= m_config.maxOpenPositions) return;
    
    scanVolSpreads(grid);
}

void VolSpreadScanner::scanVolSpreads(const OptionGrid* grid) {
    // Collect expiries with ATM vol
    std::vector<std::pair<ExpiryData*, double>> expiryVols;
    
    for (const auto& pair : grid->getExpiries()) {
        ExpiryData* expiry = pair.second.get();
        int32_t days = expiry->getTradingDays();
        
        if (days < m_config.minDaysToExpiry || days > m_config.maxDaysToExpiry) {
            continue;
        }
        
        double atmVol = expiry->getATMVol();
        if (atmVol > 0) {
            expiryVols.push_back({expiry, atmVol});
        }
    }
    
    if (expiryVols.size() < 2) return;
    
    // Sort by expiry date
    std::sort(expiryVols.begin(), expiryVols.end(),
        [](const auto& a, const auto& b) {
            return a.first->getExpiryDate() < b.first->getExpiryDate();
        });
    
    // Compare adjacent expiries
    for (size_t i = 0; i + 1 < expiryVols.size(); ++i) {
        auto& nearPair = expiryVols[i];
        auto& farPair = expiryVols[i + 1];
        
        ExpiryData* nearExp = nearPair.first;
        ExpiryData* farExp = farPair.first;
        double nearVol = nearPair.second;
        double farVol = farPair.second;
        
        // Check term spread
        int32_t termSpread = farExp->getTradingDays() - nearExp->getTradingDays();
        if (termSpread < m_config.minTermSpread) {
            continue;
        }
        
        // Calculate vol spread
        double volSpread = nearVol - farVol;
        double absSpread = std::abs(volSpread);
        
        if (absSpread >= m_config.minVolSpread && 
            absSpread <= m_config.maxVolSpread) {
            
            VolSpreadOpportunity opp;
            opp.nearExpiry = nearExp;
            opp.farExpiry = farExp;
            opp.nearVol = nearVol;
            opp.farVol = farVol;
            opp.spread = volSpread;
            opp.sellNearBuyFar = (volSpread > 0);  // Sell high vol, buy low vol
            
            evaluateSpread(opp);
        }
    }
}

void VolSpreadScanner::evaluateSpread(const VolSpreadOpportunity& opp) {
    ScannerHitEvent event;
    event.option = nullptr;  // This is an expiry-level signal
    event.signal = opp.spread * 100;  // Convert to percentage
    event.timestamp = getTimestamp();
    
    std::string direction = opp.sellNearBuyFar ? "Sell Near / Buy Far" : "Buy Near / Sell Far";
    
    event.reason = "Vol Spread: " + direction +
                   " (near=" + std::to_string(opp.nearVol * 100) + "%, " +
                   "far=" + std::to_string(opp.farVol * 100) + "%, " +
                   "spread=" + std::to_string(opp.spread * 100) + "%)";
    
    notifyHit(event);
}

// Register scanner
REGISTER_SCANNER("VolSpreadScanner", VolSpreadScanner);

} // namespace wt_option
