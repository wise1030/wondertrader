/*!
 * \file SpreadScanner.h
 * \brief Strike spread scanner for option arbitrage
 * 
 * Sample scanner implementation migrated from longbeach StrikeSpreadScanner
 */

#pragma once

#include "IScanModule.h"

namespace wt_option {

/**
 * @brief Strike spread scanner configuration
 */
struct SpreadScannerConfig : public ScannerConfig {
    double minSpread = 0.0;             // Minimum spread to trigger
    double maxSpread = 100.0;           // Maximum spread to consider
    double minProfitPct = 0.5;          // Minimum profit percentage
    bool scanCalls = true;              // Scan call options
    bool scanPuts = true;               // Scan put options
    int32_t minDaysToExpiry = 1;        // Minimum days to expiry
    int32_t maxDaysToExpiry = 60;       // Maximum days to expiry
    int32_t maxOrderSize = 1;           // Global max order size
    int32_t maxPosOpt = 10;             // Global max option position
    
    /// Get effective max order size for expiry (with fallback)
    int32_t getMaxOrderSize(uint32_t expiry) const {
        auto* ov = getExpiryOverride(expiry);
        if (ov && ScannerExpiryOverrides::isSet(ov->maxOrderSize)) return ov->maxOrderSize;
        return maxOrderSize;
    }
    
    /// Get effective max option position for expiry
    int32_t getMaxPosOpt(uint32_t expiry) const {
        auto* ov = getExpiryOverride(expiry);
        if (ov && ScannerExpiryOverrides::isSet(ov->maxPosOpt)) return ov->maxPosOpt;
        return maxPosOpt;
    }
};

/**
 * @brief Strike spread scanner
 * 
 * Scans for vertical spread opportunities where theoretical 
 * value differs significantly from market prices.
 */
class SpreadScanner : public IScanModule {
public:
    SpreadScanner(const ScannerConfig& config);
    
    void onStart() override;
    void onStop() override;
    void onTick(const OptionGrid* grid) override;
    void onOptionUpdate(OptionData* option) override;
    
private:
    void scanExpiry(const ExpiryData* expiry, const OptionGrid* grid);
    void scanStrikes(const std::vector<StrikeDataPtr>& strikes, 
                     OptionRight right, const OptionGrid* grid);
    
    SpreadScannerConfig m_config;
    uint64_t m_lastScanTime;
};

using SpreadScannerPtr = std::shared_ptr<SpreadScanner>;

} // namespace wt_option
