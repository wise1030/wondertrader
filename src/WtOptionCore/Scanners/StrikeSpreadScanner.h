/*!
 * \file StrikeSpreadScanner.h
 * \brief Strike-based spread scanner for option trading
 * 
 * Migrated from longbeach/optiontrader/StrikeSpreadScanner.h
 * Scans for spread, guts, and synthetic pair opportunities.
 */

#pragma once

#include "IScanModule.h"
#include "../OptionGrid.h"
#include "../OptionOrder.h"
#include <map>
#include <set>
#include <vector>

namespace wt_option {

/**
 * @brief Spread order information
 */
struct SpreadOrderInfo {
    OptionDataPtr option;      // Target option
    OptionDataPtr counterpart; // Counterpart option for spread
    int32_t size;              // Position size
    double profit;             // Expected profit
    double strikeDiff;         // Strike difference
    double bidPrice;           // Entry bid
    double askPrice;           // Entry ask
    
    bool operator<(const SpreadOrderInfo& other) const {
        return profit > other.profit;  // Sort by profit descending
    }
};

/**
 * @brief Guts (strangle) order information
 */
struct GutsOrderInfo {
    OptionDataPtr callOption;  // Call leg
    OptionDataPtr putOption;   // Put leg
    int32_t size;
    double profit;
    double strikeDiff;
    bool signalOnly;           // Signal only, don't trade
    
    bool operator<(const GutsOrderInfo& other) const {
        return profit > other.profit;
    }
};

/**
 * @brief Synthetic pair order information
 */
struct SynpairOrderInfo {
    std::vector<OptionDataPtr> options;  // 4 options for synpair
    std::vector<double> prices;          // Entry prices
    std::vector<double> synDiffs;        // Synthetic differences
    int32_t totalSize;
    double unitProfit;
    
    bool operator<(const SynpairOrderInfo& other) const {
        return unitProfit > other.unitProfit;
    }
};

/**
 * @brief Strike spread scanner configuration
 */
struct StrikeSpreadScannerConfig : public ScannerConfig {
    // Spread parameters
    double spreadThreshold = 0.02;      // Min profit threshold as % of price
    double gutsThreshold = 0.02;        // Min guts profit threshold
    double synpairThreshold = 0.01;     // Min synpair profit threshold
    
    // Size limits
    int32_t maxSpreadSize = 10;
    int32_t maxGutsSize = 10;
    int32_t maxSynpairSize = 10;
    int32_t maxOpenSize = 50;           // Max concurrent open positions
    
    // Timing
    uint64_t cancelInterval1 = 5000000; // 5 sec cancel interval for leg 1
    uint64_t cancelInterval2 = 3000000; // 3 sec cancel interval for leg 2
    double maxTick = 3.0;               // Max tick slippage
    
    // Feature flags
    bool enableSpread = true;
    bool enableGuts = true;
    bool enableSynpair = true;
    bool gutsSignalOnly = false;        // Only signal, don't trade guts
    
    StrikeSpreadScannerConfig() {
        name = "StrikeSpreadScanner";
    }
};

/**
 * @brief Spread combo order
 */
class SpreadComboOrder : public ComboOrder {
public:
    SpreadComboOrder(const SpreadOrderInfo& info, const StrikeSpreadScannerConfig& config);
    
    SendResult sendOrders() override;
    void onFill(const OptionOrder& order, const FillEvent& fill) override;
    bool checkDone(bool timeout) override;
    
    const SpreadOrderInfo& getInfo() const { return m_info; }
    
private:
    void checkOrder1();
    void checkOrder2();
    
    SpreadOrderInfo m_info;
    StrikeSpreadScannerConfig m_config;
    int32_t m_fill1;
    int32_t m_fill2;
    int32_t m_expectedFill2;
};

/**
 * @brief Guts combo order
 */
class GutsComboOrder : public ComboOrder {
public:
    GutsComboOrder(const GutsOrderInfo& info, const StrikeSpreadScannerConfig& config);
    
    SendResult sendOrders() override;
    void onFill(const OptionOrder& order, const FillEvent& fill) override;
    bool checkDone(bool timeout) override;
    
private:
    GutsOrderInfo m_info;
    StrikeSpreadScannerConfig m_config;
    int32_t m_fill1;
    int32_t m_fill2;
};

/**
 * @brief Synpair combo order (4-leg)
 */
class SynpairComboOrder : public ComboOrder {
public:
    SynpairComboOrder(const SynpairOrderInfo& info, const StrikeSpreadScannerConfig& config);
    
    SendResult sendOrders() override;
    void onFill(const OptionOrder& order, const FillEvent& fill) override;
    bool checkDone(bool timeout) override;
    
    bool sortByViolation();  // Sort legs by price violation
    
private:
    SynpairOrderInfo m_info;
    StrikeSpreadScannerConfig m_config;
    std::vector<int32_t> m_fills;
    std::vector<int32_t> m_expectedFills;
    bool m_timeout;
};

/**
 * @brief Strike spread scanner
 * 
 * Scans option chain for:
 * 1. Vertical spreads (call/put spreads across strikes)
 * 2. Guts (strangle variations)
 * 3. Synthetic pairs (arbitrage across synthetic positions)
 */
class StrikeSpreadScanner : public IScanModule, public IOptionGridListener {
public:
    StrikeSpreadScanner(const StrikeSpreadScannerConfig& config);
    
    // IScanModule
    void onStart() override;
    void onStop() override;
    void onTick(const OptionGrid* grid) override;
    void onOptionUpdate(OptionData* option) override;
    void onUnderlyingUpdate(double price) override;
    
    // IOptionGridListener
    void onOptionAdded(const OptionData* option) override;
    void onGridUpdated() override;
    
    // Main scanning
    void scanGrid();
    
protected:
    void evalOption(OptionData* option);
    bool evalSpread(OptionData* option, StrikeData* strike, 
                    std::vector<SpreadOrderInfo>& orders);
    bool evalGuts(OptionData* option, StrikeData* strike,
                  std::vector<GutsOrderInfo>& orders);
    bool evalSynpair(OptionData* option, StrikeData* strike,
                     std::vector<SynpairOrderInfo>& orders);
    
    double getReferenceFutPx(uint32_t expiry);
    bool isWithinDelta(OptionData* option);
    
private:
    StrikeSpreadScannerConfig m_config;
    
    // Per-expiry data
    std::set<uint32_t> m_expiries;
    std::map<uint32_t, std::vector<StrikeDataPtr>> m_expiryStrikes;
    std::map<uint32_t, double> m_referenceFutPx;
    
    // Active orders
    std::vector<std::shared_ptr<SpreadComboOrder>> m_spreadOrders;
    std::vector<std::shared_ptr<GutsComboOrder>> m_gutsOrders;
    std::vector<std::shared_ptr<SynpairComboOrder>> m_synpairOrders;
    
    int32_t m_spreadOpenSize;
    int32_t m_gutsOpenSize;
    int32_t m_synpairOpenSize;
    
    // Scan tracking
    using ScanList = std::map<uint32_t, std::set<OptionDataPtr>>;
    ScanList m_scanList;
    ScanList m_gutsCheckedList;
    std::map<uint32_t, std::set<StrikeDataPtr>> m_synpairCheckedList;
    
    uint64_t m_lastCheckTime;
    double m_tickF;  // Futures tick size
};

} // namespace wt_option
