/*!
 * \file ButterflyScanner.cpp
 * \brief Butterfly spread scanner implementation
 */

#include "ButterflyScanner.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace wt_option {

ButterflyScanner::ButterflyScanner(const ScannerConfig& config)
    : IScanModule("ButterflyScanner")
    , m_lastScanTime(0)
    , m_isPanicked(false)
    , m_currentOpenPositions(0)
{
    if (auto* bfCfg = dynamic_cast<const ButterflyScannerConfig*>(&config)) {
        m_config = *bfCfg;
    }
}

void ButterflyScanner::onStart() {
    log("ButterflyScanner started");
    m_isPanicked = false;
    m_currentOpenPositions = 0;
}

void ButterflyScanner::onStop() {
    log("ButterflyScanner stopped");
}

void ButterflyScanner::onPanic() {
    m_isPanicked = true;
    log("Panic signal received - disabling new positions");
}

void ButterflyScanner::onTick(const OptionGrid* grid) {
    if (!grid || !isEnabled() || m_isPanicked) return;
    
    uint64_t now = getTimestamp();
    
    // Throttle: scan at most once per 100ms
    if (now - m_lastScanTime < 100000) return;
    m_lastScanTime = now;
    
    // Check position limits
    if (m_currentOpenPositions >= m_config.maxOpenPositions) return;
    
    // Scan each expiry
    for (const auto& pair : grid->getExpiries()) {
        scanExpiry(pair.second.get(), grid);
    }
}

void ButterflyScanner::onOptionUpdate(OptionData* option) {
    // Could implement incremental scanning here
}

void ButterflyScanner::scanExpiry(const ExpiryData* expiry, const OptionGrid* grid) {
    if (!expiry) return;
    
    // Check days to expiry
    int32_t daysToExpiry = expiry->getTradingDays();
    if (daysToExpiry < m_config.minDaysToExpiry || 
        daysToExpiry > m_config.maxDaysToExpiry) {
        return;
    }
    
    // Collect and sort strikes
    std::vector<StrikeDataPtr> strikes;
    for (const auto& pair : expiry->getStrikes()) {
        strikes.push_back(pair.second);
    }
    
    if (strikes.size() < 3) return;  // Need at least 3 strikes for butterfly
    
    std::sort(strikes.begin(), strikes.end(), 
        [](const StrikeDataPtr& a, const StrikeDataPtr& b) {
            return a->getStrike() < b->getStrike();
        });
    
    // Scan calls
    if (m_config.scanCalls) {
        scanButterflies(strikes, OptionRight::Call, grid);
    }
    
    // Scan puts
    if (m_config.scanPuts) {
        scanButterflies(strikes, OptionRight::Put, grid);
    }
}

void ButterflyScanner::scanButterflies(const std::vector<StrikeDataPtr>& strikes,
                                        OptionRight right, const OptionGrid* grid) {
    // Check all possible butterfly combinations
    // For each triplet of consecutive strikes
    for (size_t i = 0; i + 2 < strikes.size(); ++i) {
        auto& lowStrike = strikes[i];
        auto& highStrike = strikes[i + 2];
        
        // Find middle strike (should be between low and high)
        auto& midStrike = strikes[i + 1];
        
        // Check if strikes are evenly spaced
        double spread1 = midStrike->getStrike() - lowStrike->getStrike();
        double spread2 = highStrike->getStrike() - midStrike->getStrike();
        
        if (std::abs(spread1 - spread2) > 0.01) continue;  // Not evenly spaced
        if (spread1 > m_config.maxSpreadWidth) continue;   // Spread too wide
        
        // Get options
        auto lowOpt = lowStrike->get(right);
        auto midOpt = midStrike->get(right);
        auto highOpt = highStrike->get(right);
        
        if (!lowOpt || !midOpt || !highOpt) continue;
        
        // Calculate butterfly values
        // Long butterfly: buy low, sell 2x mid, buy high
        double buyValue = calculateButterflyValue(lowOpt, midOpt, highOpt, false);  // Pay ask
        double sellValue = calculateButterflyValue(lowOpt, midOpt, highOpt, true);  // Receive bid
        
        // Theoretical value of butterfly is the intrinsic value at expiry
        // For ATM butterfly, max value is spread width
        double theoValue = spread1;  // Maximum profit at middle strike
        
        // Check for opportunities
        // Buy butterfly if market value < theoretical value
        if (buyValue > 0 && buyValue < theoValue - m_config.minThreshold) {
            double profit = theoValue - buyValue;
            if (profit >= m_config.minProfitTicks * m_config.tickSize) {
                ButterflyOpportunity opp;
                opp.lowStrike = lowOpt;
                opp.midStrike = midOpt;
                opp.highStrike = highOpt;
                opp.right = right;
                opp.theorValue = theoValue;
                opp.marketValue = buyValue;
                opp.profit = profit;
                opp.isBuy = true;
                
                evaluateOpportunity(opp);
            }
        }
        
        // Sell butterfly if market value > theoretical value
        if (sellValue > theoValue + m_config.minThreshold) {
            double profit = sellValue - theoValue;
            if (profit >= m_config.minProfitTicks * m_config.tickSize) {
                ButterflyOpportunity opp;
                opp.lowStrike = lowOpt;
                opp.midStrike = midOpt;
                opp.highStrike = highOpt;
                opp.right = right;
                opp.theorValue = theoValue;
                opp.marketValue = sellValue;
                opp.profit = profit;
                opp.isBuy = false;
                
                evaluateOpportunity(opp);
            }
        }
    }
}

double ButterflyScanner::calculateButterflyValue(const OptionDataPtr& low,
                                                  const OptionDataPtr& mid,
                                                  const OptionDataPtr& high,
                                                  bool useBid) const {
    double lowPrice, midPrice, highPrice;
    
    if (useBid) {
        // Selling butterfly: sell low, buy 2x mid, sell high
        lowPrice = low->getBid();
        midPrice = mid->getAsk();
        highPrice = high->getBid();
    } else {
        // Buying butterfly: buy low, sell 2x mid, buy high
        lowPrice = low->getAsk();
        midPrice = mid->getBid();
        highPrice = high->getAsk();
    }
    
    if (lowPrice <= 0 || midPrice <= 0 || highPrice <= 0) {
        return -1;  // Invalid prices
    }
    
    // Butterfly value = low - 2*mid + high
    return lowPrice - 2 * midPrice + highPrice;
}

double ButterflyScanner::calculateTheoreticalValue(double lowStrike, double midStrike,
                                                    double highStrike) const {
    // For a properly constructed butterfly, max value is the wing width
    return midStrike - lowStrike;
}

void ButterflyScanner::evaluateOpportunity(const ButterflyOpportunity& opp) {
    // Generate hit event
    ScannerHitEvent event;
    event.option = opp.lowStrike.get();
    event.signal = opp.profit;
    event.timestamp = getTimestamp();
    
    std::string rightStr = (opp.right == OptionRight::Call) ? "Call" : "Put";
    std::string actionStr = opp.isBuy ? "Buy" : "Sell";
    
    event.reason = actionStr + " " + rightStr + " Butterfly: profit=" + 
                   std::to_string(opp.profit) + 
                   " (theo=" + std::to_string(opp.theorValue) +
                   ", mkt=" + std::to_string(opp.marketValue) + ")";
    
    notifyHit(event);
}

// Register scanner with factory
REGISTER_SCANNER("ButterflyScanner", ButterflyScanner);

} // namespace wt_option
