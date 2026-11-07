#pragma once

#include "optioncoretypes.h"
#include "OptionGreeks.h"
#include "PnlTracker.h"
#include "IOptionGrid.h"   // ExpiryTable (P2-4 per-expiry settlement prices)

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <memory>

namespace wt_option {

class OptionData;
class OptionGrid;
class OptionRisk;

struct FillRecord {
    std::string code;
    bool isBuy = false;
    double price = 0;
    uint32_t qty = 0;
    double fee = 0;
    uint64_t time = 0;
};

struct DayResult {
    uint32_t date = 0;
    double totalPnl = 0;
    double dayPnl = 0;
    double tradingPnl = 0;   // from fills
    double settlementPnl = 0; // from option intrinsic / future close
    double closePnl = 0;    // from mark-to-mid of remaining positions
};

class ExpirationSimulator {
public:
    ExpirationSimulator(double multiplier = 1.0);

    void setGrid(OptionGrid* grid) { m_grid = grid; }
    void setRisk(OptionRisk* risk) { m_risk = risk; }
    void setMultiplier(double m) { m_multiplier = m; }

    void onFill(const std::string& code, bool isBuy, double price,
                uint32_t qty, double fee, uint64_t time);

    // Compute settlement PnL for expired contracts + close PnL for open positions
    // Returns the DayResult for the given date.
    // P2-4: `expiries` (may be nullptr) supplies per-expiry underlying prices;
    // when present they take precedence over the global spotPrice so
    // multi-series setups settle each expiry against its own underlying.
    DayResult checkExpiration(uint32_t curDate, double spotPrice,
                              const ExpiryTable* expiries = nullptr);

    // Get all historical day results
    const std::vector<DayResult>& getDayResults() const { return m_dayResults; }

    // Print summary to log
    void printSummary() const;

    // Clear all records (for new session)
    void clear() { m_fills.clear(); m_dayResults.clear(); m_lastTotalPnl = 0; }

private:
    OptionGrid* m_grid = nullptr;
    OptionRisk* m_risk = nullptr;
    double m_multiplier = 1.0;

    std::vector<FillRecord> m_fills;
    std::vector<DayResult> m_dayResults;
    double m_lastTotalPnl = 0;
};

using ExpirationSimulatorPtr = std::shared_ptr<ExpirationSimulator>;

} // namespace wt_option
