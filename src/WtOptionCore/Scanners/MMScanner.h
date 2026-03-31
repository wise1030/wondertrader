/*!
 * \file MMScanner.h
 * \brief Market Maker Scanner for option trading
 * 
 * Migrated from longbeach/optiontrader/MMScanner.h
 * Provides market making signals based on bid-ask spreads and fair value.
 */

#pragma once

#include "IScanModule.h"
#include <map>

namespace wt_option {

/**
 * @brief Market maker scanner configuration
 */
struct MMScannerConfig : public ScannerConfig {
    double minEdge = 0.0;               // Minimum edge to trade (in ticks)
    double maxSpreadTicks = 10.0;       // Maximum bid-ask spread in ticks
    double tickSize = 0.1;              // Option tick size
    double fairValueBuffer = 0.5;       // Buffer around fair value (ticks)
    int32_t maxOpenPositions = 50;      // Maximum open positions
    int32_t maxPositionPerStrike = 10;  // Max position per strike
    int32_t minDaysToExpiry = 1;        // Minimum days to expiry
    int32_t maxDaysToExpiry = 60;       // Maximum days to expiry
    bool quoteOnBid = true;             // Quote on bid side
    bool quoteOnAsk = true;             // Quote on ask side
    
    /// Get effective max open positions for expiry (mirrors longbeach ExpiryContext)
    int32_t getMaxOpenPositions(uint32_t expiry) const {
        auto* ov = getExpiryOverride(expiry);
        if (ov && ScannerExpiryOverrides::isSet(ov->maxPosOpt)) return ov->maxPosOpt;
        return maxOpenPositions;
    }
    
    /// Get effective max position per strike for expiry
    int32_t getMaxPositionPerStrike(uint32_t expiry) const {
        auto* ov = getExpiryOverride(expiry);
        if (ov && ScannerExpiryOverrides::isSet(ov->maxOrderSize)) return ov->maxOrderSize;
        return maxPositionPerStrike;
    }
};

/**
 * @brief Market maker opportunity
 */
struct MMOpportunity {
    OptionData* option;
    bool isBuy;                         // True = buy at bid, False = sell at ask
    double price;                       // Suggested price
    double fairValue;                   // Theoretical fair value
    double edge;                        // Expected edge (ticks)
};

/**
 * @brief Market maker scanner
 * 
 * Identifies opportunities to provide liquidity by comparing
 * theoretical values with current bid/ask prices.
 */
class MMScanner : public IScanModule {
public:
    MMScanner(const ScannerConfig& config);
    
    void onStart() override;
    void onStop() override;
    void onTick(const OptionGrid* grid) override;
    void onOptionUpdate(OptionData* option) override;
    void onPanic() override;
    
private:
    void scanOption(OptionData* option, const OptionGrid* grid);
    void evaluateQuote(const MMOpportunity& opp);
    
    MMScannerConfig m_config;
    uint64_t m_lastScanTime;
    bool m_isPanicked;
    
    std::map<std::string, int32_t> m_positionPerOption;
    int32_t m_totalPositions;
};

using MMScannerPtr = std::shared_ptr<MMScanner>;

} // namespace wt_option
