/*!
 * \file OptionGrid.h
 * \brief Option grid management for option chains
 * 
 * Migrated from longbeach/quantbox/strategy/optioncore/OptionGrid.h
 */

#pragma once

#include "OptionTypes.h"
#include "OptionData.h"
#include "OptionGreeks.h"
#include "VolCurve.h"
#include "IOptionPricer.h"
#include <map>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>
#include <string>
#include <shared_mutex>

namespace wt_option {

/**
 * @brief Option grid listener interface
 */
class IOptionGridListener {
public:
    virtual ~IOptionGridListener() = default;
    
    virtual void onExpiryAdded(const ExpiryData* expiry) {}
    virtual void onStrikeAdded(const StrikeData* strike) {}
    virtual void onOptionAdded(const OptionData* option) {}
    virtual void onGridUpdated() {}
};

/**
 * @brief Option grid - manages option chain data
 * 
 * Central container for all option data organized by expiry and strike.
 */
class OptionGrid {
public:
    OptionGrid(const std::string& underlyingCode = "");
    virtual ~OptionGrid() = default;
    
    // Underlying info
    const std::string& getUnderlyingCode() const { return m_underlyingCode; }
    void setUnderlyingCode(const std::string& code) { m_underlyingCode = code; }
    
    // Underlying price
    double getUnderlyingPrice() const { return m_underlyingPrice; }
    void setUnderlyingPrice(double price);
    
    // Market Data Update
    void onTick(const std::string& code, double price, double bid, double ask, int32_t bidQty, int32_t askQty);
    
    // Expiry management
    ExpiryDataPtr addExpiry(uint32_t expiryDate);
    ExpiryDataPtr getExpiry(uint32_t expiryDate) const;
    ExpiryDataPtr getFrontMonthExpiry() const;
    std::vector<uint32_t> getExpiryDates() const;
    const std::map<uint32_t, ExpiryDataPtr>& getExpiries() const { return m_expiries; }
    
    // Option management
    OptionDataPtr addOption(const OptionInfo& info);
    OptionDataPtr getOption(const std::string& code) const;
    OptionDataPtr getOption(uint32_t internalId) const;
    OptionDataPtr getOption(uint32_t expiry, double strike, OptionRight right) const;
    uint32_t getOptionId(const std::string& code) const;
    
    // Extracted flat vector for O(1) iteration and lookup
    const std::vector<OptionDataPtr>& getAllOptions() const { return m_optionsByIndex; }
    
    // Strike management
    StrikeDataPtr getStrike(uint32_t expiry, double strike) const;
    
    // Pricer (uses unified IOptionPricer from IOptionPricer.h)
    void setOptionPricer(IOptionPricerPtr pricer) { m_pricer = pricer; }
    IOptionPricerPtr getOptionPricer() const { return m_pricer; }
    
    // Trading calendar integration (from WonderTrader basedata)
    // Note: session uses the UNDERLYING's session (option session = underlying session)
    void setBaseDataMgr(wtp::IBaseDataMgr* bdMgr);
    void setProductId(const std::string& stdPID);
    void setSessionInfo(wtp::WTSSessionInfo* sInfo);
    
    // Volatility curve
    void setVolatilityCurve(uint32_t expiry, IVolCurvePtr curve);
    IVolCurvePtr getVolatilityCurve(uint32_t expiry) const;
    
    // Compute values (delegates to IOptionPricer)
    void computeValues();
    void computeValues(uint32_t expiry);
    void computeValues(OptionData* option);
    
    // Risk aggregation
    OptionGreeks getAggregatedGreeks() const;
    OptionGreeks getExpiryGreeks(uint32_t expiry) const;
    
    // Listeners
    void addListener(IOptionGridListener* listener);
    void removeListener(IOptionGridListener* listener);
    
    // Iteration
    void forEachOption(std::function<void(OptionData*)> func);
    void forEachOption(std::function<void(const OptionData*)> func) const;
    void forEachExpiry(std::function<void(ExpiryData*)> func);
    void forEachExpiry(std::function<void(const ExpiryData*)> func) const;
    
    // Current date/time for calculations
    uint32_t getCurrentDate() const { return m_currentDate; }
    uint32_t getCurrentTime() const { return m_currentTime; }
    void setCurrentDate(uint32_t date);  // Day-level only (backward compat)
    void setCurrentDateTime(uint32_t date, uint32_t time);  // Minute-level precision
    
protected:
    void notifyExpiryAdded(const ExpiryData* expiry);
    void notifyStrikeAdded(const StrikeData* strike);
    void notifyOptionAdded(const OptionData* option);
    void notifyGridUpdated();
    
private:
    std::string m_underlyingCode;
    std::string m_stdPID;                                   // Product ID for holiday lookup
    double m_underlyingPrice;
    uint32_t m_currentDate;
    uint32_t m_currentTime = 0;                             // HHMM format for minute-level
    wtp::IBaseDataMgr* m_bdMgr = nullptr;                   // Trading calendar (not owned)
    wtp::WTSSessionInfo* m_sessionInfo = nullptr;            // Underlying's trading session (not owned)
    
    std::map<uint32_t, ExpiryDataPtr> m_expiries;           // expiry -> ExpiryData
    std::map<std::string, OptionDataPtr> m_optionsByCode;   // code -> OptionData (legacy)
    std::unordered_map<std::string, uint32_t> m_codeToIndex; // Fast string to index lookup
    std::vector<OptionDataPtr> m_optionsByIndex;            // Fast integer lookup
    std::map<uint32_t, IVolCurvePtr> m_volCurves;           // expiry -> VolCurve
    
    IOptionPricerPtr m_pricer;
    std::vector<IOptionGridListener*> m_listeners;
    mutable std::shared_mutex m_mutex; // RCU / fine-grained lock
};

using OptionGridPtr = std::shared_ptr<OptionGrid>;

} // namespace wt_option

