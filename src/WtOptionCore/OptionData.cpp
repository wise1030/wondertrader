/*!
 * \file OptionData.cpp
 * \brief Option data implementation
 * 
 * Trading calendar integration:
 *   updateTimeToExpiry() uses IBaseDataMgr::isHoliday() to count actual
 *   trading days between currentDate and expiryDate. Falls back to
 *   approximate calendar-day logic when IBaseDataMgr is not available.
 */

#include "OptionData.h"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSSessionInfo.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdint>

namespace wt_option {

//=============================================================================
// OptionData implementation
//=============================================================================

OptionData::OptionData(const OptionInfo& info)
    : m_info(info)
    , m_position(0)
{
}

void OptionData::updateMarket(const OptionMarket& market) {
    m_market = market;
}

//=============================================================================
// StrikeData implementation
//=============================================================================

StrikeDataPtr StrikeData::create(ExpiryDataPtr expiry, double strike, 
                                 OptionDataPtr call, OptionDataPtr put) {
    auto instance = std::make_shared<StrikeData>(strike);
    instance->setExpiryData(expiry);
    instance->m_call = call;
    instance->m_put = put;
    
    if (call) call->setStrikeData(instance);
    if (put) put->setStrikeData(instance);
    
    return instance;
}

StrikeData::StrikeData(double strike)
    : m_strike(strike)
{
}

OptionDataPtr& StrikeData::get(OptionRight right) {
    return (right == OptionRight::Call) ? m_call : m_put;
}

const OptionDataPtr& StrikeData::get(OptionRight right) const {
    return (right == OptionRight::Call) ? m_call : m_put;
}

//=============================================================================
// ExpiryData implementation
//=============================================================================

ExpiryData::ExpiryData(uint32_t expiryDate, const std::string& underlyingCode)
    : m_expiryDate(expiryDate)
    , m_underlyingCode(underlyingCode)
    , m_timeToExpiry(0)
    , m_tradingDays(0)
    , m_calendarDays(0)
    , m_remainingMinsToday(0)
    , m_tradingMinsPerDay(0)
    , m_riskFreeRate(0.03)  // Default 3%
    , m_dividendYield(0)
    , m_atmVol(0.2)         // Default 20%
    , m_atmForward(0)
{
}

/**
 * @brief Update time-to-expiry with minute-level precision
 * 
 * Calculation strategy (best to worst precision):
 * 
 * 1. SessionInfo + BaseDataMgr + currentTime available:
 *    - remainingMinsToday = getTradingMins() - timeToMinutes(currentTime)
 *    - futureTradingDays = countTradingDays(currentDate, expiryDate)
 *    - totalMins = remainingMinsToday + futureTradingDays * minsPerDay
 *    - T = totalMins / (252.0 * minsPerDay)
 * 
 * 2. BaseDataMgr available, no session info or no currentTime:
 *    - T = tradingDays / 252.0  (day-level)
 * 
 * 3. Fallback (no infrastructure):
 *    - T = calendarDays / 365.0
 */
void ExpiryData::updateTimeToExpiry(uint32_t currentDate, uint32_t currentTime) {
    if (currentDate >= m_expiryDate) {
        m_timeToExpiry = 0;
        m_tradingDays = 0;
        m_calendarDays = 0;
        m_remainingMinsToday = 0;
        return;
    }
    
    // Always compute calendar days
    m_calendarDays = countCalendarDays(currentDate, m_expiryDate);
    
    // Cache trading mins per day from session info
    if (m_sessionInfo) {
        m_tradingMinsPerDay = m_sessionInfo->getTradingMins();
    }
    
    if (m_bdMgr && !m_stdPID.empty()) {
        // Count trading days from currentDate+1 to expiryDate (future full days)
        m_tradingDays = countTradingDays(currentDate, m_expiryDate);
        
        // Minute-level precision when session info and currentTime are available
        if (m_sessionInfo && currentTime > 0 && m_tradingMinsPerDay > 0) {
            // Remaining trading minutes in current session
            m_remainingMinsToday = calcRemainingMinsToday(currentTime);
            
            // Total remaining trading minutes:
            //   today's remaining + future full trading days × minsPerDay
            // Note: m_tradingDays includes expiryDate as a full day
            // But on expiry day trading ends at close, so we count it as a full day
            int32_t futureDaysExcludingToday = m_tradingDays;  // days from tomorrow to expiryDate
            int32_t totalMins = m_remainingMinsToday + futureDaysExcludingToday * m_tradingMinsPerDay;
            
            // Year fraction: totalMins / (252 trading days × minsPerDay)
            double annualMins = 252.0 * m_tradingMinsPerDay;
            m_timeToExpiry = std::max(0.0, totalMins / annualMins);
        } else {
            // Day-level: T = tradingDays / 252
            m_remainingMinsToday = 0;
            m_timeToExpiry = std::max(0.0, m_tradingDays / 252.0);
        }
    } else {
        // Fallback: approximate using calendar days
        m_tradingDays = static_cast<int32_t>(m_calendarDays * 252.0 / 365.0);
        m_remainingMinsToday = 0;
        m_timeToExpiry = std::max(0.0, m_calendarDays / 365.0);
    }
}

/**
 * @brief Calculate remaining trading minutes from currentTime to session close
 * 
 * Uses WTSSessionInfo::timeToMinutes() to find how many trading minutes
 * have elapsed, then subtracts from total trading minutes.
 * 
 * @param currentTime  HHMM format (e.g., 0935)
 * @return remaining trading minutes, or full session if outside trading time
 */
int32_t ExpiryData::calcRemainingMinsToday(uint32_t currentTime) const {
    if (!m_sessionInfo || m_tradingMinsPerDay == 0) return 0;
    
    uint32_t elapsedMins = m_sessionInfo->timeToMinutes(currentTime);
    if (elapsedMins == UINT32_MAX) {
        // currentTime is outside trading hours
        // Check if we're before the first section -> full day remaining
        // or after the last section -> 0 remaining
        uint32_t openTime = m_sessionInfo->getOpenTime();
        uint32_t closeTime = m_sessionInfo->getCloseTime();
        
        // Simple heuristic: if currentTime < openTime, full day ahead
        // Note: for night sessions with offset, openTime might be > closeTime
        // In that case, timeToMinutes handles it; we treat "outside" as 0
        if (m_sessionInfo->getOffsetMins() >= 0) {
            // Normal session (e.g., 0900-1500): if before open, full day
            if (currentTime < openTime) {
                return static_cast<int32_t>(m_tradingMinsPerDay);
            }
        }
        return 0;
    }
    
    int32_t remaining = static_cast<int32_t>(m_tradingMinsPerDay) - static_cast<int32_t>(elapsedMins);
    return std::max(0, remaining);
}

/**
 * @brief Count trading days between fromDate and toDate (exclusive of fromDate, inclusive of toDate)
 * 
 * Iterates each calendar day from fromDate+1 to toDate, checking
 * IBaseDataMgr::isHoliday() to determine if it's a trading day.
 * isHoliday() already handles both weekends and exchange-specific holidays.
 */
int32_t ExpiryData::countTradingDays(uint32_t fromDate, uint32_t toDate) const {
    if (!m_bdMgr || fromDate >= toDate) return 0;
    
    int32_t tradingDays = 0;
    
    // Convert fromDate to time_t for iteration
    tm t;
    memset(&t, 0, sizeof(tm));
    t.tm_year = fromDate / 10000 - 1900;
    t.tm_mon = (fromDate % 10000) / 100 - 1;
    t.tm_mday = fromDate % 100;
    t.tm_isdst = -1;
    time_t ts = mktime(&t);
    
    // Iterate each day from fromDate+1 to toDate
    while (true) {
        ts += 86400;  // Advance one day
        
        tm* newT = localtime(&ts);
        uint32_t curDate = (newT->tm_year + 1900) * 10000 
                         + (newT->tm_mon + 1) * 100 
                         + newT->tm_mday;
        
        if (curDate > toDate) break;
        
        // isHoliday checks both weekends (wday==0||6) and holiday list
        if (!m_bdMgr->isHoliday(m_stdPID.c_str(), curDate)) {
            tradingDays++;
        }
    }
    
    return tradingDays;
}

/**
 * @brief Count calendar days between two YYYYMMDD dates
 */
int32_t ExpiryData::countCalendarDays(uint32_t fromDate, uint32_t toDate) const {
    if (fromDate >= toDate) return 0;
    
    tm t1;
    memset(&t1, 0, sizeof(tm));
    t1.tm_year = fromDate / 10000 - 1900;
    t1.tm_mon = (fromDate % 10000) / 100 - 1;
    t1.tm_mday = fromDate % 100;
    t1.tm_isdst = -1;
    
    tm t2;
    memset(&t2, 0, sizeof(tm));
    t2.tm_year = toDate / 10000 - 1900;
    t2.tm_mon = (toDate % 10000) / 100 - 1;
    t2.tm_mday = toDate % 100;
    t2.tm_isdst = -1;
    
    time_t ts1 = mktime(&t1);
    time_t ts2 = mktime(&t2);
    
    double diffSec = difftime(ts2, ts1);
    return static_cast<int32_t>(diffSec / 86400.0 + 0.5);
}

void ExpiryData::addStrike(StrikeDataPtr strike) {
    if (strike) {
        m_strikes[strike->getStrike()] = strike;
    }
}

StrikeDataPtr ExpiryData::getStrike(double strike) const {
    auto it = m_strikes.find(strike);
    return (it != m_strikes.end()) ? it->second : nullptr;
}

OptionGreeks ExpiryData::getAggregatedGreeks() const {
    OptionGreeks total;
    for (const auto& pair : m_strikes) {
        if (pair.second->call()) {
            auto call = pair.second->call();
            total.accum(call->getPosition(), call->greeks());
        }
        if (pair.second->put()) {
            auto put = pair.second->put();
            total.accum(put->getPosition(), put->greeks());
        }
    }
    return total;
}

double ExpiryData::calculateSyntheticForward() {
    double sumWeight = 0;
    double sumFwd = 0;
    double discount = std::exp(-m_riskFreeRate * m_timeToExpiry);
    
    for (const auto& pair : m_strikes) {
        StrikeData* strike = pair.second.get();
        OptionData* call = strike->call().get();
        OptionData* put = strike->put().get();
        
        if (call && put) {
            double cBid = call->getBid();
            double cAsk = call->getAsk();
            double pBid = put->getBid();
            double pAsk = put->getAsk();
            
            // Need valid quotes
            if (cBid > 0 && cAsk > 0 && pBid > 0 && pAsk > 0) {
                // Synthetic forward from Put-Call Parity:
                // C - P = (F - K) * discount
                // F = K + (C - P) / discount
                
                double midC = (cBid + cAsk) * 0.5;
                double midP = (pBid + pAsk) * 0.5;
                
                double synFwd = strike->getStrike() + (midC - midP) / discount;
                
                // Weight by inverse spread (tighter spread = more confidence)
                double spreadC = (cAsk - cBid) / midC;
                double spreadP = (pAsk - pBid) / midP;
                double weight = 1.0 / (spreadC + spreadP + 0.001);
                
                // Only consider strikes near money (reasonable spreads)
                if (spreadC < 0.2 && spreadP < 0.2) {
                    sumFwd += synFwd * weight;
                    sumWeight += weight;
                }
            }
        }
    }
    
    if (sumWeight > 0) {
        return sumFwd / sumWeight;
    }
    return 0.0;
}

} // namespace wt_option
