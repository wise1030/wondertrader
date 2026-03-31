/*!
 * \file ButterflyScanner.h
 * \brief Butterfly spread scanner for option arbitrage
 * 
 * Migrated from longbeach/optiontrader/ButterflyScanner.h
 * Detects butterfly spread opportunities in option chains.
 */

#pragma once

#include "IScanModule.h"
#include <map>
#include <set>

namespace wt_option {

/**
 * @brief Butterfly scanner configuration
 */
struct ButterflyScannerConfig : public ScannerConfig {
    double minProfitTicks = 1.0;        // Minimum profit in ticks
    double maxSpreadWidth = 10.0;       // Maximum strike spread width
    double minThreshold = 0.0;          // Minimum threshold for triggering
    int32_t maxOpenPositions = 10;      // Maximum open positions
    int32_t maxOrderSize = 100;         // Maximum order size per leg
    bool scanCalls = true;              // Scan call butterflies
    bool scanPuts = true;               // Scan put butterflies
    int32_t minDaysToExpiry = 1;        // Minimum days to expiry
    int32_t maxDaysToExpiry = 60;       // Maximum days to expiry
    double tickSize = 0.1;              // Option tick size
    
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
 * @brief Butterfly spread opportunity
 */
struct ButterflyOpportunity {
    OptionDataPtr lowStrike;            // Lower strike option
    OptionDataPtr midStrike;            // Middle strike option (x2)
    OptionDataPtr highStrike;           // Higher strike option
    OptionRight right;                  // Call or Put
    double theorValue;                  // Theoretical value
    double marketValue;                 // Market value
    double profit;                      // Profit opportunity
    bool isBuy;                         // Buy or sell butterfly
};

/**
 * @brief Butterfly spread scanner
 * 
 * Scans for butterfly spread arbitrage opportunities where the
 * cost of the butterfly differs from theoretical value.
 * 
 * A butterfly spread consists of:
 * - Buy 1 low strike
 * - Sell 2 middle strike
 * - Buy 1 high strike
 * 
 * For calls: Buy K1 Call, Sell 2x K2 Calls, Buy K3 Call (K1 < K2 < K3)
 */
class ButterflyScanner : public IScanModule {
public:
    ButterflyScanner(const ScannerConfig& config);
    
    void onStart() override;
    void onStop() override;
    void onTick(const OptionGrid* grid) override;
    void onOptionUpdate(OptionData* option) override;
    void onPanic() override;
    
private:
    void scanExpiry(const ExpiryData* expiry, const OptionGrid* grid);
    void scanButterflies(const std::vector<StrikeDataPtr>& strikes, 
                         OptionRight right, const OptionGrid* grid);
    
    double calculateButterflyValue(const OptionDataPtr& low, 
                                   const OptionDataPtr& mid,
                                   const OptionDataPtr& high,
                                   bool useBid) const;
    
    double calculateTheoreticalValue(double lowStrike, double midStrike,
                                     double highStrike) const;
    
    void evaluateOpportunity(const ButterflyOpportunity& opp);
    
    ButterflyScannerConfig m_config;
    uint64_t m_lastScanTime;
    bool m_isPanicked;
    
    // Track expiries and strikes
    std::map<uint32_t, std::vector<StrikeDataPtr>> m_expiryStrikes;
    std::set<uint32_t> m_activeExpiries;
    
    // Current scan state
    int32_t m_currentOpenPositions;
};

using ButterflyScannerPtr = std::shared_ptr<ButterflyScanner>;

} // namespace wt_option
