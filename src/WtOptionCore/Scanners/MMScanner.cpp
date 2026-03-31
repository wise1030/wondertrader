/*!
 * \file MMScanner.cpp
 * \brief Market Maker Scanner implementation
 */

#include "MMScanner.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace wt_option {

MMScanner::MMScanner(const ScannerConfig& config)
    : IScanModule("MMScanner")
    , m_lastScanTime(0)
    , m_isPanicked(false)
    , m_totalPositions(0)
{
    if (auto* mmCfg = dynamic_cast<const MMScannerConfig*>(&config)) {
        m_config = *mmCfg;
    }
}

void MMScanner::onStart() {
    log("MMScanner started");
    m_isPanicked = false;
    m_totalPositions = 0;
    m_positionPerOption.clear();
}

void MMScanner::onStop() {
    log("MMScanner stopped");
}

void MMScanner::onPanic() {
    m_isPanicked = true;
    log("Panic signal received - stopping market making");
}

void MMScanner::onTick(const OptionGrid* grid) {
    if (!grid || !isEnabled() || m_isPanicked) return;
    
    uint64_t now = getTimestamp();
    
    // Throttle: scan at most once per 50ms
    if (now - m_lastScanTime < 50000) return;
    m_lastScanTime = now;
    
    // Check position limits
    if (m_totalPositions >= m_config.maxOpenPositions) return;
    
    // Scan all options
    const_cast<OptionGrid*>(grid)->forEachOption([this, grid](OptionData* option) {
        scanOption(option, grid);
    });
}

void MMScanner::onOptionUpdate(OptionData* option) {
    // Could trigger immediate re-evaluation
}

void MMScanner::scanOption(OptionData* option, const OptionGrid* grid) {
    if (!option) return;
    
    // Check days to expiry via strike -> expiry
    int32_t daysToExpiry = 0;
    auto strike = option->getStrikeData();
    if (strike) {
        auto expiry = strike->getExpiryData();
        if (expiry) daysToExpiry = expiry->getTradingDays();
    }
    if (daysToExpiry < m_config.minDaysToExpiry || 
        daysToExpiry > m_config.maxDaysToExpiry) {
        return;
    }
    
    double bid = option->getBid();
    double ask = option->getAsk();
    double theoPrice = option->getTheoPrice();
    
    if (bid <= 0 || ask <= 0 || theoPrice <= 0) return;
    
    // Check bid-ask spread
    double spread = (ask - bid) / m_config.tickSize;
    if (spread > m_config.maxSpreadTicks) return;
    
    // Check position limits for this option
    auto it = m_positionPerOption.find(option->getCode());
    int32_t currentPos = (it != m_positionPerOption.end()) ? it->second : 0;
    
    // Calculate edge for buying at bid
    if (m_config.quoteOnBid && currentPos < m_config.maxPositionPerStrike) {
        double buyEdge = (theoPrice - bid) / m_config.tickSize;
        
        if (buyEdge >= m_config.minEdge + m_config.fairValueBuffer) {
            MMOpportunity opp;
            opp.option = option;
            opp.isBuy = true;
            opp.price = bid;
            opp.fairValue = theoPrice;
            opp.edge = buyEdge;
            
            evaluateQuote(opp);
        }
    }
    
    // Calculate edge for selling at ask
    if (m_config.quoteOnAsk && currentPos > -m_config.maxPositionPerStrike) {
        double sellEdge = (ask - theoPrice) / m_config.tickSize;
        
        if (sellEdge >= m_config.minEdge + m_config.fairValueBuffer) {
            MMOpportunity opp;
            opp.option = option;
            opp.isBuy = false;
            opp.price = ask;
            opp.fairValue = theoPrice;
            opp.edge = sellEdge;
            
            evaluateQuote(opp);
        }
    }
}

void MMScanner::evaluateQuote(const MMOpportunity& opp) {
    ScannerHitEvent event;
    event.option = opp.option;
    event.signal = opp.edge;
    event.timestamp = getTimestamp();
    
    std::string action = opp.isBuy ? "BUY" : "SELL";
    event.reason = "MM " + action + " @ " + std::to_string(opp.price) +
                   " (theo=" + std::to_string(opp.fairValue) +
                   ", edge=" + std::to_string(opp.edge) + " ticks)";
    
    notifyHit(event);
}

// Register scanner
REGISTER_SCANNER("MMScanner", MMScanner);

} // namespace wt_option
