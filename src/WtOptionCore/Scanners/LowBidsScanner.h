/*!
 * \file LowBidsScanner.h
 * \brief Low bids scanner for option trading
 * 
 * Migrated from longbeach/optiontrader/LowBidsScanner.h
 * Scans for mispriced options with abnormally low bids.
 */

#pragma once

#include "IScanModule.h"
#include "../OptionGrid.h"
#include <set>

namespace wt_option {

/**
 * @brief Low bids scanner configuration
 */
struct LowBidsScannerConfig : public ScannerConfig {
    double minBidThreshold = 0.01;       // Min bid to consider
    double maxBidThreshold = 0.5;        // Max bid (above = not "low")
    double theoPremium = 0.02;           // Required premium over theo
    double minDelta = 0.05;              // Min delta to trade
    double maxDelta = 0.5;               // Max delta to trade
    int32_t maxSize = 10;                // Max position size
    int32_t minDaysToExpiry = 3;         // Minimum days to expiry
    
    LowBidsScannerConfig() {
        name = "LowBidsScanner";
    }
    
    /// Get effective max position size for expiry
    int32_t getMaxSize(uint32_t expiry) const {
        auto* ov = getExpiryOverride(expiry);
        if (ov && ScannerExpiryOverrides::isSet(ov->maxPosOpt)) return ov->maxPosOpt;
        return maxSize;
    }
};

/**
 * @brief Low bids scanner
 * 
 * Identifies options with bids significantly below theoretical value.
 * These may represent buying opportunities.
 */
class LowBidsScanner : public IScanModule {
public:
    LowBidsScanner(const LowBidsScannerConfig& config);
    
    void onStart() override;
    void onStop() override;
    void onTick(const OptionGrid* grid) override;
    void onOptionUpdate(OptionData* option) override;
    void onUnderlyingUpdate(double price) override {}
    
protected:
    void evalOption(OptionData* option);
    bool isValidCandidate(OptionData* option);
    
private:
    LowBidsScannerConfig m_config;
    std::set<std::string> m_scannedOptions;  // Avoid rescanning
    uint64_t m_lastScanTime;
};

} // namespace wt_option
