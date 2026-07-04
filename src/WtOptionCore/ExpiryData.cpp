/*!
 * \file ExpiryData.cpp
 * \brief Per-expiry data implementation (migrated from quantbox)
 */
// Include standard headers FIRST to avoid WT namespace pollution
#include <cstring>
#include <cmath>
#include <algorithm>

#include "ExpiryData.h"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Share/TimeUtils.hpp"
#include "../WTSTools/WTSLogger.h"

#include <cmath>

namespace wt_option {

ExpiryData::ExpiryData(uint32_t expiry, const std::string& optionProduct)
    : m_expiry(expiry)
    , m_optionProduct(optionProduct)
{
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
            isHol = (m_holidays->find(d) != m_holidays->end());
        }
        if (!isHol) count++;
    }
    return count;
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
    m_daysToExpiration = countCalendarDays(currentDate, expirationDate);
    m_bdaysToExpiration = countTradingDays(currentDate, expirationDate);
    // Recalculate discount factor
    m_discountFactor = exp(m_riskFreeRate * m_daysToExpiration / -365.0);
}

double ExpiryData::getIntradayFraction() const {
    if (!m_sessionInfo) return 0.0;
    // Use WTSSessionInfo trading minutes to compute intraday fraction
    // Fraction = remaining trading minutes / total trading minutes
    uint32_t curTime = TimeUtils::getCurMin(); // current minute of day
    uint32_t totalMins = m_sessionInfo->getTradingMins();
    if (totalMins == 0) return 0.0;

    // Convert curTime to minutes since session start
    // WTSSessionInfo has timeToMinutes which converts time offset to trading minutes
    uint32_t elapsedMins = m_sessionInfo->timeToMinutes(curTime, true);
    if (elapsedMins >= totalMins) return 0.0;
    double frac = static_cast<double>(totalMins - elapsedMins) / totalMins;
    return std::max(1e-6, std::min(1.0, frac));
}

double ExpiryData::getSettlementFraction() const {
    if (m_daysToExpiration != 0) return 1.0;
    // On expiration day, fraction depends on settlement time
    // For simplicity, use intraday fraction
    return getIntradayFraction();
}

double ExpiryData::getPremiumFadeFraction() const {
    if (m_daysToExpiration != 0) return 1.0;
    return getIntradayFraction();
}

double ExpiryData::getMaturity() const {
    return (bdaysToExpiry() + getIntradayFraction()) / 252.0;
}

} // namespace wt_option
