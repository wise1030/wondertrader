/*!
 * \file ExpiryData.cpp
 * \brief Per-expiry data implementation (migrated from quantbox)
 */
// Include standard headers FIRST to avoid WT namespace pollution
#include <cstring>
#include <cmath>
#include <algorithm>

#include "ExpiryData.h"
#include "IBaseDataMgr.h"
#include "WTSSessionInfo.hpp"
#include "TimeUtils.hpp"
#include "WTSLogger.h"

#include <cmath>

namespace wt_option {

ExpiryData::ExpiryData(uint32_t expiry, const std::string& optionProduct)
    : m_expiry(expiry)
    , m_optionProduct(optionProduct)
{
    m_emaForwardSpread.setWindow(120.0);
}

void ExpiryData::setRiskFreeRate(double r) {
    m_riskFreeRate = r;
    m_discountFactor = exp(m_riskFreeRate * daysToExpiry() / -365.0);
}

double ExpiryData::getForwardTheo(double spot) const {
    return spot * exp((m_riskFreeRate - m_repoRate) * daysToExpiry() / 365.0) - m_dividendCumsum;
}

bool ExpiryData::isTradingDay(uint32_t date) const {
    if (!m_bdMgr) return false;
    return !m_bdMgr->isHoliday(m_stdPID.c_str(), date, false);
}

uint32_t ExpiryData::countTradingDays(uint32_t fromDate, uint32_t toDate) const {
    if (fromDate >= toDate) return 0;
    if (!m_bdMgr && !m_holidays) return 0;
    uint32_t count = 0;
    uint32_t d = fromDate;
    while (d < toDate) {
        uint32_t y = d / 10000, m = (d / 100) % 100, day = d % 100;
        day++;
        uint32_t daysInMonth[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        if (m == 2 && ((y%4==0 && y%100!=0) || y%400==0)) daysInMonth[1] = 29;
        if (day > daysInMonth[m-1]) { day = 1; m++; if (m > 12) { m = 1; y++; } }
        d = y * 10000 + m * 100 + day;

        bool isHol = false;
        if (m_bdMgr) {
            isHol = m_bdMgr->isHoliday(m_stdPID.c_str(), d, false);
        } else if (m_holidays) {
            // mirror IBaseDataMgr::isHoliday: weekends are non-trading even
            // when they are not listed in holidays.json
            uint32_t wd = TimeUtils::getWeekDay(d);
            isHol = (wd == 0 || wd == 6) ||
                    (m_holidays->find(d) != m_holidays->end());
        }
        if (!isHol) count++;
    }
    return count;
}

int32_t ExpiryData::daysBetween(uint32_t fromDate, uint32_t toDate) const {
    if (fromDate <= toDate)
        return static_cast<int32_t>(countCalendarDays(fromDate, toDate));
    return -static_cast<int32_t>(countCalendarDays(toDate, fromDate));
}

uint32_t ExpiryData::countCalendarDays(uint32_t fromDate, uint32_t toDate) const {
    if (fromDate >= toDate) return 0;
    // Approximate: convert YYYYMMDD to ordinal, subtract
    // This is a rough estimate; for production use a proper date library
    uint32_t count = 0;
    uint32_t d = fromDate;
    while (d < toDate) {
        uint32_t y = d / 10000, m = (d / 100) % 100, day = d % 100;
        day++;
        uint32_t daysInMonth[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        if (m == 2 && ((y%4==0 && y%100!=0) || y%400==0)) daysInMonth[1] = 29;
        if (day > daysInMonth[m-1]) { day = 1; m++; if (m > 12) { m = 1; y++; } }
        d = y * 10000 + m * 100 + day;
        count++;
    }
    return count;
}

void ExpiryData::updateDaysToExpiration(uint32_t currentDate, uint32_t expirationDate) {
    m_currentDate = currentDate;
    m_expirationDate = expirationDate;
    m_daysToExpiration = daysBetween(currentDate, expirationDate);
    if (m_daysToExpiration >= 0) {
        m_bdaysToExpiration = static_cast<int32_t>(countTradingDays(currentDate, expirationDate));
    } else {
        // Preserve sign after expiry so purge/deactivate conditions are reachable.
        m_bdaysToExpiration = -static_cast<int32_t>(countTradingDays(expirationDate, currentDate));
    }
    // Prevent bdays=0 leading to maturity=0 (causes division by zero in GvvVolCurve)
    if (m_bdaysToExpiration == 0 && m_daysToExpiration > 0)
        m_bdaysToExpiration = 1;
    // Recalculate discount factor
    m_discountFactor = exp(m_riskFreeRate * m_daysToExpiration / -365.0);
}

double ExpiryData::getIntradayFraction() const {
    if (!m_sessionInfo) return 0.0;
    // Use WTSSessionInfo trading minutes to compute intraday fraction
    // Fraction = remaining trading minutes / total trading minutes
    //
    // Audit 2026-11-13 (v2, T1-4): this used to read TimeUtils::getCurMin(),
    // the WALL CLOCK, with two compounding defects:
    //   1. Format — getCurMin() returns HHMMSS (e.g. 93015) while
    //      timeToMinutes() expects HHMM (0910). With an offset-0 session info
    //      the value passed straight through, so every wall time from 00:09
    //      onward compared ABOVE all section bounds and timeToMinutes
    //      returned INVALID_UINT32 → fraction clamped to ~0. Only wall times
    //      00:00–00:08:59 (numeric < first section start) yielded frac≈1.
    //      Net effect: intraday T-decay was NEVER applied — in backtest AND
    //      in live — except in that 9-minute window.
    //   2. Clock source — in backtest the wall clock is the RUN time, not
    //      the simulated time, so even a correctly formatted read would make
    //      T depend on when the replay was executed.
    // The injected seconds-of-day (per-tick from the strategy timer payload)
    // fixes both: sim-anchored AND converted to the documented HHMM format.
    // Pre-existing limitation preserved as-is: for cross-midnight sessions a
    // post-midnight sim time compares below the night section's raw bounds
    // (e.g. 00:10 < 2100) and auto-aligns to frac=1.0 — same behaviour the
    // old wall-clock path exhibited, left for a future precision pass.
    uint32_t totalMins = m_sessionInfo->getTradingMins();
    if (totalMins == 0) return 0.0;
    const uint32_t sec = m_curSecOfDay;
    const uint32_t curTime = (sec / 3600) * 100 + ((sec % 3600) / 60);  // HHMM

    // Convert curTime to minutes since session start
    // WTSSessionInfo has timeToMinutes which converts time offset to trading minutes
    uint32_t elapsedMins = m_sessionInfo->timeToMinutes(curTime, true);
    if (elapsedMins >= totalMins) return 0.0;
    double frac = static_cast<double>(totalMins - elapsedMins) / totalMins;
    return std::max(1e-6, std::min(1.0, frac));
}

double ExpiryData::getSettlementFraction() const {
    if (m_daysToExpiration > 0) return 1.0;
    if (m_daysToExpiration < 0) return 0.0;   // expired: no further optionality
    // On expiration day, fraction depends on settlement time
    // For simplicity, use intraday fraction
    return getIntradayFraction();
}

double ExpiryData::getPremiumFadeFraction() const {
    if (m_daysToExpiration > 0) return 1.0;
    if (m_daysToExpiration < 0) return 0.0;
    return getIntradayFraction();
}

double ExpiryData::getMaturity() const {
    double m = (bdaysToExpiry() + getIntradayFraction()) / 252.0;
    return std::max(m, 1.0 / 252.0);
}

void ExpiryData::setSyntheticForward(double synFwd, double underlyingPrice, double time) {
    if (synFwd > 0 && underlyingPrice > 0) {
        m_forwardSpread = synFwd - underlyingPrice;
        m_emaForwardSpread.update(time, m_forwardSpread);
    }
}

} // namespace wt_option
