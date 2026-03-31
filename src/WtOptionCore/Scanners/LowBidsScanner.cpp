/*!
 * \file LowBidsScanner.cpp
 * \brief Low bids scanner implementation
 */

#include "LowBidsScanner.h"
#include <cmath>
#include <iostream>

namespace wt_option {

LowBidsScanner::LowBidsScanner(const LowBidsScannerConfig& config)
    : IScanModule("LowBidsScanner")
    , m_config(config)
    , m_lastScanTime(0)
{
}

void LowBidsScanner::onStart() {
    setEnabled(true);
    m_scannedOptions.clear();
}

void LowBidsScanner::onStop() {
    setEnabled(false);
}

void LowBidsScanner::onTick(const OptionGrid* grid) {
    if (!isEnabled() || !grid) return;
    
    // Scan all options periodically
    const_cast<OptionGrid*>(grid)->forEachOption([this](OptionData* option) {
        evalOption(option);
    });
}

void LowBidsScanner::onOptionUpdate(OptionData* option) {
    if (!isEnabled()) return;
    evalOption(option);
}

void LowBidsScanner::evalOption(OptionData* option) {
    if (!option || !isValidCandidate(option)) return;
    
    double bid = option->getBid();
    double theo = option->getTheoPrice();
    
    // Check if bid is in "low bids" range
    if (bid < m_config.minBidThreshold || bid > m_config.maxBidThreshold) {
        return;
    }
    
    // Check for significant underpricing
    double discount = theo - bid;
    if (discount > m_config.theoPremium * theo) {
        // Found a low bid opportunity
        ScannerHitEvent event;
        event.option = option;
        event.signal = discount / theo;  // Discount as % of theo
        event.reason = getName() + ": Low bid: bid=" + std::to_string(bid) + 
                       " theo=" + std::to_string(theo);
        
        notifyHit(event);
        
        // Mark as scanned to avoid duplicate signals
        m_scannedOptions.insert(option->getCode());
    }
}

bool LowBidsScanner::isValidCandidate(OptionData* option) {
    if (!option) return false;
    
    // Check delta bounds
    double delta = std::abs(option->greeks().delta());
    if (delta < m_config.minDelta || delta > m_config.maxDelta) {
        return false;
    }
    
    // Check days to expiry
    auto strike = option->getStrikeData();
    if (strike) {
        auto expiry = strike->getExpiryData();
        if (expiry) {
            int32_t days = expiry->getTradingDays();
            if (days < m_config.minDaysToExpiry) {
                return false;
            }
        }
    }
    
    // Check if already scanned recently
    if (m_scannedOptions.count(option->getCode()) > 0) {
        return false;
    }
    
    return true;
}

} // namespace wt_option
