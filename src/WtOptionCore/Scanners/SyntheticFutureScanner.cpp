/*!
 * \file SyntheticFutureScanner.cpp
 * \brief Synthetic future scanner implementation
 */

#include "SyntheticFutureScanner.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace wt_option {

SyntheticFutureScanner::SyntheticFutureScanner(const ScannerConfig& config)
    : IScanModule("SyntheticFutureScanner")
    , m_lastScanTime(0)
    , m_isPanicked(false)
    , m_currentOpenPositions(0)
{
    if (auto* sfCfg = dynamic_cast<const SyntheticFutureScannerConfig*>(&config)) {
        m_config = *sfCfg;
    }
}

void SyntheticFutureScanner::onStart() {
    log("SyntheticFutureScanner started");
    m_isPanicked = false;
    m_currentOpenPositions = 0;
    m_referenceFutPrices.clear();
}

void SyntheticFutureScanner::onStop() {
    log("SyntheticFutureScanner stopped");
}

void SyntheticFutureScanner::onPanic() {
    m_isPanicked = true;
    log("Panic signal received - disabling new positions");
}

void SyntheticFutureScanner::onTick(const OptionGrid* grid) {
    if (!grid || !isEnabled() || m_isPanicked) return;
    
    uint64_t now = getTimestamp();
    
    // Throttle: scan at most once per 100ms
    if (now - m_lastScanTime < 100000) return;
    m_lastScanTime = now;
    
    // Check position limits
    if (m_currentOpenPositions >= m_config.maxOpenPositions) return;
    
    // Get underlying price as reference
    double underlyingPrice = grid->getUnderlyingPrice();
    if (underlyingPrice <= 0) return;
    
    // Scan each expiry
    for (const auto& pair : grid->getExpiries()) {
        scanExpiry(pair.second.get(), grid);
    }
}

void SyntheticFutureScanner::onOptionUpdate(OptionData* option) {
    // Could implement incremental scanning here
}

void SyntheticFutureScanner::scanExpiry(const ExpiryData* expiry, const OptionGrid* grid) {
    if (!expiry) return;
    
    // Check days to expiry
    int32_t daysToExpiry = expiry->getTradingDays();
    if (daysToExpiry < m_config.minDaysToExpiry || 
        daysToExpiry > m_config.maxDaysToExpiry) {
        return;
    }
    
    // Get forward price for this expiry
    double futurePrice = expiry->getATMForward();
    if (futurePrice <= 0) {
        futurePrice = grid->getUnderlyingPrice();
    }
    
    double riskFreeRate = expiry->getRiskFreeRate();
    double timeToExpiry = expiry->getTimeToExpiry();
    
    // Scan each strike
    for (const auto& pair : expiry->getStrikes()) {
        evaluateStrike(pair.second, futurePrice, riskFreeRate, 
                       timeToExpiry, grid);
    }
}

void SyntheticFutureScanner::evaluateStrike(const StrikeDataPtr& strike,
                                             double futurePrice,
                                             double riskFreeRate,
                                             double timeToExpiry,
                                             const OptionGrid* grid) {
    if (!strike) return;
    
    auto call = strike->call();
    auto put = strike->put();
    
    if (!call || !put) return;  // Need both call and put
    
    double strikePrice = strike->getStrike();
    
    // Calculate synthetic future prices
    // Synthetic Long = Buy Call + Sell Put (at strike K)
    // Synthetic price = Strike + Call - Put
    double syntheticBid = calculateSyntheticBid(call, put, strikePrice);
    double syntheticAsk = calculateSyntheticAsk(call, put, strikePrice);
    
    if (syntheticBid <= 0 || syntheticAsk <= 0) return;
    
    // Calculate theoretical forward price from put-call parity
    // F = K + (C - P) * e^(rT)
    double discount = std::exp(-riskFreeRate * timeToExpiry);
    double theoForward = strikePrice + (call->getTheoPrice() - put->getTheoPrice()) / discount;
    
    // Check for arbitrage opportunities
    
    // Buy synthetic (buy call, sell put), sell real future
    // Profit when: Synthetic Ask < Future Bid
    double buyProfitPerUnit = futurePrice - syntheticAsk;
    if (buyProfitPerUnit > m_config.buyThreshold) {
        if (buyProfitPerUnit >= m_config.minProfitTicks * m_config.tickSize) {
            SyntheticFutureOpportunity opp;
            opp.strike = strike;
            opp.syntheticPrice = syntheticAsk;
            opp.futurePrice = futurePrice;
            opp.profit = buyProfitPerUnit;
            opp.buySynthetic = true;
            
            evaluateOpportunity(opp);
        }
    }
    
    // Sell synthetic (sell call, buy put), buy real future
    // Profit when: Synthetic Bid > Future Ask
    double sellProfitPerUnit = syntheticBid - futurePrice;
    if (sellProfitPerUnit > m_config.sellThreshold) {
        if (sellProfitPerUnit >= m_config.minProfitTicks * m_config.tickSize) {
            SyntheticFutureOpportunity opp;
            opp.strike = strike;
            opp.syntheticPrice = syntheticBid;
            opp.futurePrice = futurePrice;
            opp.profit = sellProfitPerUnit;
            opp.buySynthetic = false;
            
            evaluateOpportunity(opp);
        }
    }
}

double SyntheticFutureScanner::calculateSyntheticBid(const OptionDataPtr& call,
                                                      const OptionDataPtr& put,
                                                      double strike) const {
    // Sell synthetic = sell call (get bid) + buy put (pay ask)
    double callBid = call->getBid();
    double putAsk = put->getAsk();
    
    if (callBid <= 0 || putAsk <= 0) return -1;
    
    // Synthetic forward price = Strike + Call - Put
    return strike + callBid - putAsk;
}

double SyntheticFutureScanner::calculateSyntheticAsk(const OptionDataPtr& call,
                                                      const OptionDataPtr& put,
                                                      double strike) const {
    // Buy synthetic = buy call (pay ask) + sell put (get bid)
    double callAsk = call->getAsk();
    double putBid = put->getBid();
    
    if (callAsk <= 0 || putBid <= 0) return -1;
    
    // Synthetic forward price = Strike + Call - Put
    return strike + callAsk - putBid;
}

void SyntheticFutureScanner::evaluateOpportunity(const SyntheticFutureOpportunity& opp) {
    // Generate hit event
    ScannerHitEvent event;
    event.option = opp.strike->call().get();  // Reference the call option
    event.signal = opp.profit;
    event.timestamp = getTimestamp();
    
    std::string actionStr = opp.buySynthetic ? 
        "Buy Synthetic, Sell Future" : "Sell Synthetic, Buy Future";
    
    event.reason = actionStr + ": profit=" + std::to_string(opp.profit) +
                   " (syn=" + std::to_string(opp.syntheticPrice) +
                   ", fut=" + std::to_string(opp.futurePrice) + ")";
    
    notifyHit(event);
}

// Register scanner with factory
REGISTER_SCANNER("SyntheticFutureScanner", SyntheticFutureScanner);

} // namespace wt_option
