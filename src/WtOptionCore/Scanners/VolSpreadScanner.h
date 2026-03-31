/*!
 * \file VolSpreadScanner.h
 * \brief Volatility spread scanner for option trading
 * 
 * Migrated from longbeach/optiontrader/VolSpreadScanner.h
 * Identifies volatility spread opportunities across expiries.
 */

#pragma once

#include "IScanModule.h"
#include <map>

namespace wt_option {

/**
 * @brief Vol spread scanner configuration
 */
struct VolSpreadScannerConfig : public ScannerConfig {
    double minVolSpread = 0.02;         // Minimum vol spread (2%)
    double maxVolSpread = 0.20;         // Maximum vol spread (20%)
    int32_t maxOpenPositions = 10;      // Maximum open positions
    int32_t minDaysToExpiry = 5;        // Minimum days for near leg
    int32_t maxDaysToExpiry = 90;       // Maximum days for far leg
    double minTermSpread = 10;          // Minimum term spread (days)
    
    /// Get effective max open positions for expiry
    int32_t getMaxOpenPositions(uint32_t expiry) const {
        auto* ov = getExpiryOverride(expiry);
        if (ov && ScannerExpiryOverrides::isSet(ov->maxPosOpt)) return ov->maxPosOpt;
        return maxOpenPositions;
    }
};

/**
 * @brief Vol spread opportunity
 */
struct VolSpreadOpportunity {
    ExpiryData* nearExpiry;
    ExpiryData* farExpiry;
    double nearVol;
    double farVol;
    double spread;                      // nearVol - farVol
    bool sellNearBuyFar;                // Calendar spread direction
};

/**
 * @brief Volatility spread scanner
 * 
 * Scans for calendar spread opportunities where volatility differences
 * between expiries are unusually large.
 */
class VolSpreadScanner : public IScanModule {
public:
    VolSpreadScanner(const ScannerConfig& config);
    
    void onStart() override;
    void onStop() override;
    void onTick(const OptionGrid* grid) override;
    void onPanic() override;
    
private:
    void scanVolSpreads(const OptionGrid* grid);
    void evaluateSpread(const VolSpreadOpportunity& opp);
    
    VolSpreadScannerConfig m_config;
    uint64_t m_lastScanTime;
    bool m_isPanicked;
    int32_t m_currentOpenPositions;
};

using VolSpreadScannerPtr = std::shared_ptr<VolSpreadScanner>;

} // namespace wt_option
