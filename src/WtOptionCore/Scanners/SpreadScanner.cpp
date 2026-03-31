/*!
 * \file SpreadScanner.cpp
 * \brief Strike spread scanner implementation
 */

#include "SpreadScanner.h"
#include <algorithm>
#include <cmath>

namespace wt_option {

SpreadScanner::SpreadScanner(const ScannerConfig& config)
    : IScanModule("SpreadScanner")
    , m_lastScanTime(0)
{
    // Copy configuration
    if (auto* spreadCfg = dynamic_cast<const SpreadScannerConfig*>(&config)) {
        m_config = *spreadCfg;
    }
}

void SpreadScanner::onStart() {
    log("SpreadScanner started");
}

void SpreadScanner::onStop() {
    log("SpreadScanner stopped");
}

void SpreadScanner::onTick(const OptionGrid* grid) {
    if (!grid || !isEnabled()) return;
    
    uint64_t now = getTimestamp();
    
    // Throttle: scan at most once per 100ms
    if (now - m_lastScanTime < 100000) return;
    m_lastScanTime = now;
    
    // Scan each expiry
    for (const auto& pair : grid->getExpiries()) {
        scanExpiry(pair.second.get(), grid);
    }
}

void SpreadScanner::onOptionUpdate(OptionData* option) {
    // Could implement incremental scanning here
}

void SpreadScanner::scanExpiry(const ExpiryData* expiry, const OptionGrid* grid) {
    if (!expiry) return;
    
    // Check days to expiry
    int32_t daysToExpiry = expiry->getTradingDays();
    if (daysToExpiry < m_config.minDaysToExpiry || 
        daysToExpiry > m_config.maxDaysToExpiry) {
        return;
    }
    
    // Collect strikes
    std::vector<StrikeDataPtr> strikes;
    for (const auto& pair : expiry->getStrikes()) {
        strikes.push_back(pair.second);
    }
    
    if (strikes.size() < 2) return;
    
    // Sort by strike price
    std::sort(strikes.begin(), strikes.end(), 
        [](const StrikeDataPtr& a, const StrikeDataPtr& b) {
            return a->getStrike() < b->getStrike();
        });
    
    // Scan calls
    if (m_config.scanCalls) {
        scanStrikes(strikes, OptionRight::Call, grid);
    }
    
    // Scan puts
    if (m_config.scanPuts) {
        scanStrikes(strikes, OptionRight::Put, grid);
    }
}

void SpreadScanner::scanStrikes(const std::vector<StrikeDataPtr>& strikes,
                                 OptionRight right, const OptionGrid* grid) {
    // Compare adjacent strikes for spread opportunities
    for (size_t i = 0; i + 1 < strikes.size(); ++i) {
        auto& strike1 = strikes[i];
        auto& strike2 = strikes[i + 1];
        
        auto opt1 = strike1->get(right);
        auto opt2 = strike2->get(right);
        
        if (!opt1 || !opt2) continue;
        
        // Get prices
        double mid1 = opt1->getMid();
        double mid2 = opt2->getMid();
        double theo1 = opt1->getTheoPrice();
        double theo2 = opt2->getTheoPrice();
        
        if (mid1 <= 0 || mid2 <= 0 || theo1 <= 0 || theo2 <= 0) continue;
        
        // Calculate spread values
        double marketSpread = mid1 - mid2;  // For calls: higher strike = lower price
        double theoSpread = theo1 - theo2;
        
        if (right == OptionRight::Put) {
            // For puts: higher strike = higher price
            marketSpread = mid2 - mid1;
            theoSpread = theo2 - theo1;
        }
        
        // Check for mispricing
        double diff = std::abs(theoSpread - marketSpread);
        double pctDiff = (theoSpread != 0) ? diff / std::abs(theoSpread) * 100 : 0;
        
        if (pctDiff >= m_config.minProfitPct && 
            diff >= m_config.minSpread &&
            diff <= m_config.maxSpread) {
            
            // Generate hit event
            ScannerHitEvent event;
            event.option = opt1.get();
            event.signal = pctDiff;
            event.reason = "Spread mispricing: " + std::to_string(pctDiff) + "%";
            event.timestamp = getTimestamp();
            
            notifyHit(event);
        }
    }
}

// Register scanner with factory
REGISTER_SCANNER("SpreadScanner", SpreadScanner);

} // namespace wt_option
