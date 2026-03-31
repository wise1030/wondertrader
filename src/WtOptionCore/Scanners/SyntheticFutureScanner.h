/*!
 * \file SyntheticFutureScanner.h
 * \brief Synthetic future scanner for put-call parity arbitrage
 * 
 * Migrated from longbeach/optiontrader/SyntheticFutureScanner.h
 * Detects arbitrage between synthetic futures and actual futures.
 */

#pragma once

#include "IScanModule.h"
#include <map>
#include <set>

namespace wt_option {

/**
 * @brief Synthetic future scanner configuration
 */
struct SyntheticFutureScannerConfig : public ScannerConfig {
    double minProfitTicks = 1.0;        // Minimum profit in ticks
    double buyThreshold = 0.0;          // Threshold for buying synthetic
    double sellThreshold = 0.0;         // Threshold for selling synthetic
    int32_t maxOpenPositions = 10;      // Maximum open positions
    int32_t maxOrderSize = 100;         // Maximum order size
    int32_t minDaysToExpiry = 1;        // Minimum days to expiry
    int32_t maxDaysToExpiry = 60;       // Maximum days to expiry
    double tickSize = 1.0;              // Future tick size
    
    /// Get effective max open positions for expiry
    int32_t getMaxOpenPositions(uint32_t expiry) const {
        auto* ov = getExpiryOverride(expiry);
        if (ov && ScannerExpiryOverrides::isSet(ov->maxPosOpt)) return ov->maxPosOpt;
        return maxOpenPositions;
    }
    
    /// Get effective max order size for expiry
    int32_t getMaxOrderSize(uint32_t expiry) const {
        auto* ov = getExpiryOverride(expiry);
        if (ov && ScannerExpiryOverrides::isSet(ov->maxOrderSize)) return ov->maxOrderSize;
        return maxOrderSize;
    }
};

/**
 * @brief Synthetic future opportunity
 */
struct SyntheticFutureOpportunity {
    StrikeDataPtr strike;               // Strike with call and put
    double syntheticPrice;              // Synthetic future price
    double futurePrice;                 // Actual future price
    double profit;                      // Profit opportunity
    bool buySynthetic;                  // Buy synthetic, sell real
};

/**
 * @brief Synthetic future scanner
 * 
 * Scans for put-call parity violations between synthetic futures
 * and actual futures.
 * 
 * Put-Call Parity: C - P = F - K * e^(-rT)
 * Synthetic Long Future = Long Call + Short Put (at same strike)
 * Synthetic Short Future = Short Call + Long Put
 */
class SyntheticFutureScanner : public IScanModule {
public:
    SyntheticFutureScanner(const ScannerConfig& config);
    
    void onStart() override;
    void onStop() override;
    void onTick(const OptionGrid* grid) override;
    void onOptionUpdate(OptionData* option) override;
    void onPanic() override;
    
private:
    void scanExpiry(const ExpiryData* expiry, const OptionGrid* grid);
    void evaluateStrike(const StrikeDataPtr& strike, double futurePrice,
                        double riskFreeRate, double timeToExpiry,
                        const OptionGrid* grid);
    
    double calculateSyntheticBid(const OptionDataPtr& call, 
                                  const OptionDataPtr& put,
                                  double strike) const;
    
    double calculateSyntheticAsk(const OptionDataPtr& call,
                                  const OptionDataPtr& put,
                                  double strike) const;
    
    void evaluateOpportunity(const SyntheticFutureOpportunity& opp);
    
    SyntheticFutureScannerConfig m_config;
    uint64_t m_lastScanTime;
    bool m_isPanicked;
    
    // Reference prices per expiry
    std::map<uint32_t, double> m_referenceFutPrices;
    
    // Current state
    int32_t m_currentOpenPositions;
};

using SyntheticFutureScannerPtr = std::shared_ptr<SyntheticFutureScanner>;

} // namespace wt_option
