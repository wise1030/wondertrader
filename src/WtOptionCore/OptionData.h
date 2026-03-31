/*!
 * \file OptionData.h
 * \brief Option and Expiry data structures
 * 
 * Migrated from OptionData.h with added synthetic forward logic
 */

#pragma once

#include "OptionTypes.h"
#include "OptionGreeks.h"
#include "UnderlyingTradingData.h"
#include <map>
#include <vector>
#include <memory>
#include <string>
#include <cmath>
#include <atomic>

// Forward declarations for WonderTrader infrastructure
namespace wtp {
    class IBaseDataMgr;
    class WTSSessionInfo;
}

namespace wt_option {

class StrikeData;
class ExpiryData;
class OptionGrid;

using StrikeDataPtr = std::shared_ptr<StrikeData>;
using ExpiryDataPtr = std::shared_ptr<ExpiryData>;

/**
 * @brief Option data structure
 */
class OptionData {
public:
    OptionData(const OptionInfo& info);
    
    // Getters
    const std::string& getCode() const { return m_info.code; }
    const OptionInfo& getInfo() const { return m_info; }
    
    uint32_t getExpiry() const { return m_info.expiry; }
    double getStrike() const { return m_info.strike; }
    OptionRight getRight() const { return m_info.right; }
    
    // Market data
    const OptionMarket& getMarket() const { return m_market; }
    void updateMarket(const OptionMarket& market);
    
    double getBid() const { return m_market.bid; }
    double getAsk() const { return m_market.ask; }
    double getMid() const { return (m_market.bid + m_market.ask) * 0.5; }
    double getLast() const { return m_market.last; }
    
    // Computed values (Double-Buffered)
    // Read from ACTIVE buffer (Fast path)
    const OptionValues& values() const { return m_values[m_activeIndex.load(std::memory_order_acquire)]; } 
    
    // Writer interface for Lock-Free update
    OptionValues& beginUpdateValues() { return m_values[1 - m_activeIndex.load(std::memory_order_relaxed)]; }
    void commitUpdateValues() { 
        m_activeIndex.store(1 - m_activeIndex.load(std::memory_order_relaxed), std::memory_order_release); 
    }
    
    // Helpers reading from ACTIVE buffer
    double getTheoPrice() const { return values().theoreticalPrice; }
    double getImpliedVol() const { return values().impliedVol; }
    const OptionGreeks& greeks() const { return values().greeks; }
    
    // Reference to parent
    void setStrikeData(StrikeDataPtr strike) { m_strike = strike; }
    StrikeDataPtr getStrikeData() const { return m_strike.lock(); }
    
    // Position tracking (for risk aggregation)
    double getPosition() const { return m_position; }
    void setPosition(double pos) { m_position = pos; }

    // Fast array indexing
    uint32_t getInternalId() const { return m_internalId; }
    void setInternalId(uint32_t id) { m_internalId = id; }

private:
    OptionInfo m_info;
    OptionMarket m_market;
    std::array<OptionValues, 2> m_values;  // Double buffering for lock-free reads
    std::atomic<uint32_t> m_activeIndex{0}; // Active buffer index (0 or 1)
    std::weak_ptr<StrikeData> m_strike;
    double m_position;
    uint32_t m_internalId = 0;
};

using OptionDataPtr = std::shared_ptr<OptionData>;

/**
 * @brief Strike data (pair of Call and Put)
 */
class StrikeData {
public:
    static StrikeDataPtr create(ExpiryDataPtr expiry, double strike, 
                                OptionDataPtr call = nullptr, OptionDataPtr put = nullptr);
    
    StrikeData(double strike);
    
    double getStrike() const { return m_strike; }
    
    OptionDataPtr& get(OptionRight right);
    const OptionDataPtr& get(OptionRight right) const;
    
    OptionDataPtr& call() { return m_call; }
    OptionDataPtr& put() { return m_put; }
    
    // Parent
    void setExpiryData(ExpiryDataPtr expiry) { m_expiry = expiry; }
    ExpiryDataPtr getExpiryData() const { return m_expiry.lock(); }

private:
    double m_strike;
    OptionDataPtr m_call;
    OptionDataPtr m_put;
    std::weak_ptr<ExpiryData> m_expiry;
};

class IVolCurve;
using IVolCurvePtr = std::shared_ptr<IVolCurve>;

/**
 * @brief Expiry data container
 */
class ExpiryData {
public:
    ExpiryData(uint32_t expiryDate, const std::string& underlyingCode);
    
    uint32_t getExpiryDate() const { return m_expiryDate; }
    const std::string& getUnderlyingCode() const { return m_underlyingCode; }
    
    // Trading calendar integration
    void setBaseDataMgr(wtp::IBaseDataMgr* bdMgr) { m_bdMgr = bdMgr; }
    void setProductId(const std::string& stdPID) { m_stdPID = stdPID; }
    void setSessionInfo(wtp::WTSSessionInfo* sInfo) { m_sessionInfo = sInfo; }
    
    /**
     * @brief Update time-to-expiry with minute-level precision
     * 
     * Uses trading session sections to compute remaining trading minutes
     * today, plus full trading days in between × minutes/day.
     * 
     * @param currentDate  YYYYMMDD format
     * @param currentTime  HHMM format (e.g., 0935 = 9:35), 0 for day-level only
     */
    void updateTimeToExpiry(uint32_t currentDate, uint32_t currentTime = 0);
    double getTimeToExpiry() const { return m_timeToExpiry; }
    int32_t getTradingDays() const { return m_tradingDays; }
    int32_t getCalendarDays() const { return m_calendarDays; }
    int32_t getRemainingMinsToday() const { return m_remainingMinsToday; }
    uint32_t getTradingMinsPerDay() const { return m_tradingMinsPerDay; }
    
    // Strikes
    void addStrike(StrikeDataPtr strike);
    StrikeDataPtr getStrike(double strike) const;
    const std::map<double, StrikeDataPtr>& getStrikes() const { return m_strikes; }
    
    // Rates and Dividends
    double getRiskFreeRate() const { return m_riskFreeRate; }
    void setRiskFreeRate(double rate) { m_riskFreeRate = rate; }
    
    double getDividendYield() const { return m_dividendYield; }
    void setDividendYield(double yield) { m_dividendYield = yield; }
    
    // ATM parameters
    double getATMVol() const { return m_atmVol; }
    void setATMVol(double vol) { m_atmVol = vol; }
    
    double getATMForward() const { return m_atmForward; }
    void setATMForward(double fwd) { m_atmForward = fwd; }
    
    // Synthetic Pricing
    /**
     * @brief Calculate synthetic forward from put-call parity
     * matches longbeach __getBestSyntheticPrice logic
     */
    double calculateSyntheticForward();
    
    // Volatility Curve
    void setVolCurve(IVolCurvePtr curve) { m_volCurve = curve; }
    IVolCurvePtr getVolCurve() const { return m_volCurve; }
    
    // Underlying Trading
    void setUnderlyingTradingData(UnderlyingTradingDataPtr data) { m_underlyingData = data; }
    UnderlyingTradingDataPtr getUnderlyingTradingData() const { return m_underlyingData; }
    
    // Risk aggregation
    OptionGreeks getAggregatedGreeks() const;

private:
    /// Count trading days between two YYYYMMDD dates using IBaseDataMgr
    int32_t countTradingDays(uint32_t fromDate, uint32_t toDate) const;
    /// Count calendar days between two YYYYMMDD dates
    int32_t countCalendarDays(uint32_t fromDate, uint32_t toDate) const;
    /// Get remaining trading minutes from currentTime to session close
    int32_t calcRemainingMinsToday(uint32_t currentTime) const;

private:
    uint32_t m_expiryDate;
    UnderlyingTradingDataPtr m_underlyingData;
    std::string m_underlyingCode;
    std::string m_stdPID;                    // Product ID for holiday lookup (e.g. "SHFE.cu")
    wtp::IBaseDataMgr* m_bdMgr = nullptr;    // Trading calendar (not owned)
    wtp::WTSSessionInfo* m_sessionInfo = nullptr; // Trading session sections (not owned)
    
    double m_timeToExpiry;
    int32_t m_tradingDays;                   // Trading days to expiry (excluding today)
    int32_t m_calendarDays;                  // Calendar days to expiry
    int32_t m_remainingMinsToday;            // Remaining trading minutes in current session
    uint32_t m_tradingMinsPerDay;            // Total trading minutes per session day
    double m_riskFreeRate;
    double m_dividendYield;
    
    double m_atmVol;
    double m_atmForward;
    
    std::map<double, StrikeDataPtr> m_strikes;
    IVolCurvePtr m_volCurve;
};

} // namespace wt_option
