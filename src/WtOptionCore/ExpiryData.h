/*!
 * \file ExpiryData.h
 * \brief Per-expiry data: calendar, forward, vol curve, maturity
 * 
 * Migrated from quantbox optioncore/ExpiryData.h (177 lines).
 * Business logic preserved: maturity calc, forward theo, intraday fraction.
 * Framework deps replaced: ExchangeCalendar → IBaseDataMgr, ClockMonitor → TimeUtils,
 * IPriceProvider → double, MarketDataContext → deleted.
 */
#pragma once

#include "optioncoretypes.h"
#include "OptionGreeks.h"
#include <memory>
#include <string>
#include <set>
#include <cmath>
#include <cstdint>

// Forward declarations for WT types
namespace wtp {
    class IBaseDataMgr;
    class WTSSessionInfo;
}

// EMAFilter is defined in OptionValues.h — must include BEFORE namespace
#include "OptionValues.h"

namespace wt_option {

class IVolCurve;
using IVolCurvePtr = std::shared_ptr<IVolCurve>;

class ExpiryData {
public:
    ExpiryData(uint32_t expiry, const std::string& optionProduct);

    // Expiry
    uint32_t getExpiry() const { return m_expiry; }
    const std::string& getOptionProduct() const { return m_optionProduct; }

    // Trading calendar — uses IBaseDataMgr::isHoliday, or m_holidays if null
    void setBaseDataMgr(wtp::IBaseDataMgr* bdMgr) { m_bdMgr = bdMgr; }
    void setSessionInfo(wtp::WTSSessionInfo* sessInfo) { m_sessionInfo = sessInfo; }
    void setStdPID(const std::string& pid) { m_stdPID = pid; }
    void setHolidays(const std::set<uint32_t>* holidays) { m_holidays = holidays; }

    void updateDaysToExpiration(uint32_t currentDate, uint32_t expirationDate);

    int32_t daysToExpiry() const { return m_daysToExpiration; }
    int32_t bdaysToExpiry() const { return m_bdaysToExpiration; }
    uint32_t getExpirationDate() const { return m_expirationDate; }

    // Rates
    void setRiskFreeRate(double r);
    double getRiskFreeRate() const { return m_riskFreeRate; }
    double getDiscountFactor() const { return m_discountFactor; }
    void setRepoRate(double r) { m_repoRate = r; }
    double getRepoRate() const { return m_repoRate; }

    // Forward
    double getForwardTheo(double spot) const;
    void setForward(double fwd) { m_fwd = fwd; }
    double getForward() const { return m_fwd; }

    // Maturity = (bdays + intradayFraction) / 252
    double getIntradayFraction() const;
    double getSettlementFraction() const;
    double getPremiumFadeFraction() const;
    double getMaturity() const;

    // Vol curve
    void setVolCurve(IVolCurvePtr curve) { m_volCurve = std::move(curve); }
    IVolCurvePtr getVolCurve() const { return m_volCurve; }

    // Hedge instrument
    void setHedgeCode(const std::string& code) { m_hedgeCode = code; }
    const std::string& getHedgeCode() const { return m_hedgeCode; }
    void setOptionVsFutureRatio(int32_t ratio) { m_optionVsFutureRatio = ratio; }
    int32_t getOptionVsFutureRatio() const { return m_optionVsFutureRatio; }

    // Risk parameters
    void setRiskMultiplier(double m) { m_riskMultiplier = m; }
    double getRiskMultiplier() const { return m_riskMultiplier; }
    void setRiskShiftDelta(double s) { m_riskShiftDelta = s; }
    double getRiskShiftDelta() const { return m_riskShiftDelta; }
    void setNormRiskDelta(double p) { m_normRiskDelta = p; }
    double getNormRiskDelta() const { return m_normRiskDelta; }

    // Expiration fractions for Greeks decay
    void setExpireGreeksFrac(double s) { m_expireGreeksFrac = s; }
    double getExpireGreeksFrac() const { return m_expireGreeksFrac; }
    void setExpireDeltaFrac(double s) { m_expireDeltaFrac = s; }
    double getExpireDeltaFrac() const { return m_expireDeltaFrac; }

    // Readiness
    void setForwardReady(bool ready) { m_forwardReady = ready; }
    bool isForwardReady() const { return m_forwardReady; }
    void setFitReady(bool ready) { m_fitReady = ready; }
    bool isFitReady() const { return m_fitReady; }
    bool isValuesReady() const { return m_forwardReady && m_fitReady; }

    // EMA roll vs front
    EMAFilter& emaRollVsFront() { return m_emaRollVsFront; }

    // Option count in this expiry
    int32_t& numOptions() { return m_numOptions; }

private:
    // Trading calendar helpers (use IBaseDataMgr)
    bool isTradingDay(uint32_t date) const;
    uint32_t countTradingDays(uint32_t fromDate, uint32_t toDate) const;
    uint32_t countCalendarDays(uint32_t fromDate, uint32_t toDate) const;

    uint32_t m_expiry;          // YYYYMM
    std::string m_optionProduct;
    std::string m_stdPID;       // for isHoliday (e.g. "SHFE.cu")
    std::string m_hedgeCode;    // underlying/future code for hedging

    wtp::IBaseDataMgr* m_bdMgr = nullptr;
    wtp::WTSSessionInfo* m_sessionInfo = nullptr;
    const std::set<uint32_t>* m_holidays = nullptr;  // holiday calendar (from holidays.json)

    int32_t m_numOptions = 0;
    int32_t m_daysToExpiration = 0;    // calendar days
    int32_t m_bdaysToExpiration = 0;   // business (trading) days
    uint32_t m_expirationDate = 0;     // YYYYMMDD
    uint32_t m_currentDate = 0;

    double m_riskFreeRate = 0;
    double m_discountFactor = 1.0;
    double m_repoRate = 0;
    double m_dividendCumsum = 0;

    IVolCurvePtr m_volCurve;

    double m_expireGreeksFrac = 1.0;
    double m_expireDeltaFrac = 1.0;
    double m_riskMultiplier = 1.0;
    double m_riskShiftDelta = 0;
    double m_normRiskDelta = 0;

    double m_fwd = NAN;
    bool m_forwardReady = false;
    bool m_fitReady = false;

    EMAFilter m_emaRollVsFront;

    int32_t m_optionVsFutureRatio = 1;
};

using ExpiryDataPtr = std::shared_ptr<ExpiryData>;

} // namespace wt_option
