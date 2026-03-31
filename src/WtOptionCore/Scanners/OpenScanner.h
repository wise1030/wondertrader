/*!
 * \file OpenScanner.h
 * \brief Open interest scanner for option trading
 * 
 * Migrated from longbeach/optiontrader/OpenScanner.h
 * Scans for unusual open interest changes.
 */

#pragma once

#include "IScanModule.h"
#include "../OptionGrid.h"
#include <map>

namespace wt_option {

/**
 * @brief Open scanner configuration
 */
struct OpenScannerConfig : public ScannerConfig {
    double openInterestThreshold = 1000;  // Min OI change to trigger
    double volumeThreshold = 500;         // Min volume to trigger
    double volumeOiRatio = 0.5;           // Min volume/OI ratio
    double minDelta = 0.1;                // Min delta
    double maxDelta = 0.9;                // Max delta
    bool trackIntraday = true;            // Track intraday changes
    
    OpenScannerConfig() {
        name = "OpenScanner";
    }
};

/**
 * @brief Open interest data snapshot
 */
struct OISnapshot {
    uint64_t timestamp;
    int32_t openInterest;
    int32_t volume;
    double price;
};

/**
 * @brief Open interest scanner
 * 
 * Identifies unusual open interest and volume patterns.
 */
class OpenScanner : public IScanModule {
public:
    OpenScanner(const OpenScannerConfig& config);
    
    void onStart() override;
    void onStop() override;
    void onTick(const OptionGrid* grid) override;
    void onOptionUpdate(OptionData* option) override;
    void onUnderlyingUpdate(double price) override {}
    
    void onSessionStart();  // Call at start of session
    
protected:
    void evalOption(OptionData* option);
    void updateSnapshot(OptionData* option);
    
private:
    OpenScannerConfig m_config;
    std::map<std::string, OISnapshot> m_startOfDay;
    std::map<std::string, OISnapshot> m_current;
};

} // namespace wt_option
