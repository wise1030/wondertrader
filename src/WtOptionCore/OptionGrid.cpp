/*!
 * \file OptionGrid.cpp
 * \brief Option grid implementation
 */

#include "OptionGrid.h"
#include "BlackScholes.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <shared_mutex>

namespace wt_option {

//=============================================================================
// OptionGrid implementation
//=============================================================================

OptionGrid::OptionGrid(const std::string& underlyingCode)
    : m_underlyingCode(underlyingCode)
    , m_underlyingPrice(0)
    , m_currentDate(0)
{
}

void OptionGrid::setUnderlyingPrice(double price) {
    if (price > 0 && price != m_underlyingPrice) {
        {
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            m_underlyingPrice = price;
        }
        if (m_pricer) {
            m_pricer->onUnderlyingPriceChanged(price);
        }
    }
}

void OptionGrid::onTick(const std::string& code, double price, double bid, double ask, int32_t bidQty, int32_t askQty) {
    // 1. Check Underlying
    if (code == m_underlyingCode) {
        setUnderlyingPrice(price);
        return;
    }
    
    // 2. Check Option
    OptionDataPtr option = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_optionsByCode.find(code);
        if (it != m_optionsByCode.end()) {
            option = it->second;
        }
    }

    if (option) {
            OptionMarket market;
            market.bid = bid;
            market.ask = ask;
            market.last = price;
            market.bidSize = bidQty;
            market.askSize = askQty;
            market.underlyingPrice = m_underlyingPrice;
            // Note: time should ideally be passed in, but for now we assume it's updated elsewhere or uses system time?
            // Existing WtOptionStrategy passed 'time'. OptionGrid::onTick excludes time?
            // Let's rely on option->updateMarket to update what it can.
            
            option->updateMarket(market);
        }
}

void OptionGrid::setBaseDataMgr(wtp::IBaseDataMgr* bdMgr) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_bdMgr = bdMgr;
    // Propagate to all existing expiries
    for (auto& pair : m_expiries) {
        pair.second->setBaseDataMgr(bdMgr);
    }
}

void OptionGrid::setProductId(const std::string& stdPID) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_stdPID = stdPID;
    // Propagate to all existing expiries
    for (auto& pair : m_expiries) {
        pair.second->setProductId(stdPID);
    }
}

void OptionGrid::setSessionInfo(wtp::WTSSessionInfo* sInfo) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_sessionInfo = sInfo;
    // Propagate underlying's session to all expiries
    // (option session = underlying session)
    for (auto& pair : m_expiries) {
        pair.second->setSessionInfo(sInfo);
    }
}

void OptionGrid::setCurrentDate(uint32_t date) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_currentDate = date;
    m_currentTime = 0;  // Day-level only
    for (auto& pair : m_expiries) {
        pair.second->updateTimeToExpiry(date, 0);
    }
}

void OptionGrid::setCurrentDateTime(uint32_t date, uint32_t time) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_currentDate = date;
    m_currentTime = time;
    for (auto& pair : m_expiries) {
        pair.second->updateTimeToExpiry(date, time);
    }
}

ExpiryDataPtr OptionGrid::addExpiry(uint32_t expiryDate) {
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_expiries.find(expiryDate);
        if (it != m_expiries.end()) {
            return it->second;
        }
    }
    
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto expiry = std::make_shared<ExpiryData>(expiryDate, m_underlyingCode);
    
    // Propagate trading calendar and session information
    if (m_bdMgr) expiry->setBaseDataMgr(m_bdMgr);
    if (!m_stdPID.empty()) expiry->setProductId(m_stdPID);
    if (m_sessionInfo) expiry->setSessionInfo(m_sessionInfo);
    
    if (m_currentDate > 0) {
        expiry->updateTimeToExpiry(m_currentDate, m_currentTime);
    }
    m_expiries[expiryDate] = expiry;
    lock.unlock(); // Unlock before notify

    notifyExpiryAdded(expiry.get());
    return expiry;
}

ExpiryDataPtr OptionGrid::getExpiry(uint32_t expiryDate) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_expiries.find(expiryDate);
    return (it != m_expiries.end()) ? it->second : nullptr;
}

ExpiryDataPtr OptionGrid::getFrontMonthExpiry() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (m_expiries.empty()) return nullptr;
    return m_expiries.begin()->second;  // Sorted by date, front is earliest
}

std::vector<uint32_t> OptionGrid::getExpiryDates() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::vector<uint32_t> dates;
    dates.reserve(m_expiries.size());
    for (const auto& pair : m_expiries) {
        dates.push_back(pair.first);
    }
    return dates;
}

OptionDataPtr OptionGrid::addOption(const OptionInfo& info) {
    // Check if already exists
    auto existing = getOption(info.code);
    if (existing) return existing;
    
    // Create option
    auto option = std::make_shared<OptionData>(info);
    
    StrikeDataPtr strike;
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        // Verify again under write lock
        auto it = m_optionsByCode.find(info.code);
        if (it != m_optionsByCode.end()) return it->second;

        // Assign array index
        uint32_t internalId = (uint32_t)m_optionsByIndex.size();
        option->setInternalId(internalId);
        
        m_optionsByIndex.push_back(option);
        m_codeToIndex[info.code] = internalId;
        m_optionsByCode[info.code] = option;
        
        // Expiry assignment
        auto expiry_it = m_expiries.find(info.expiry);
        ExpiryDataPtr expiry;
        if (expiry_it != m_expiries.end()) {
             expiry = expiry_it->second;
        } else {
             expiry = std::make_shared<ExpiryData>(info.expiry, m_underlyingCode);
             if (m_bdMgr) expiry->setBaseDataMgr(m_bdMgr);
             if (!m_stdPID.empty()) expiry->setProductId(m_stdPID);
             if (m_sessionInfo) expiry->setSessionInfo(m_sessionInfo);
             if (m_currentDate > 0) expiry->updateTimeToExpiry(m_currentDate, m_currentTime);
             m_expiries[info.expiry] = expiry;
        }

        // Get or create strike
        strike = expiry->getStrike(info.strike);
        if (!strike) {
            // Need to create strike
            OptionDataPtr call, put;
            if (info.right == OptionRight::Call) call = option; else put = option;
            strike = StrikeData::create(expiry, info.strike, call, put);
            expiry->addStrike(strike);
        } else {
            // Update existing strike
            strike->get(info.right) = option;
            option->setStrikeData(strike);
        }
    } // Unlock before notify
    
    notifyOptionAdded(option.get());
    return option;
}

OptionDataPtr OptionGrid::getOption(const std::string& code) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_codeToIndex.find(code);
    if (it != m_codeToIndex.end()) {
        return m_optionsByIndex[it->second];
    }
    return nullptr;
}

OptionDataPtr OptionGrid::getOption(uint32_t internalId) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    if (internalId < m_optionsByIndex.size()) {
        return m_optionsByIndex[internalId];
    }
    return nullptr;
}

uint32_t OptionGrid::getOptionId(const std::string& code) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_codeToIndex.find(code);
    if (it != m_codeToIndex.end()) {
        return it->second;
    }
    return UINT32_MAX;
}

OptionDataPtr OptionGrid::getOption(uint32_t expiry, double strike, OptionRight right) const {
    auto ed = getExpiry(expiry);
    if (!ed) return nullptr;
    
    auto sd = ed->getStrike(strike);
    if (!sd) return nullptr;
    
    return sd->get(right);
}

StrikeDataPtr OptionGrid::getStrike(uint32_t expiry, double strike) const {
    auto ed = getExpiry(expiry);
    return ed ? ed->getStrike(strike) : nullptr;
}

void OptionGrid::setVolatilityCurve(uint32_t expiry, IVolCurvePtr curve) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_volCurves[expiry] = curve;
}

IVolCurvePtr OptionGrid::getVolatilityCurve(uint32_t expiry) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_volCurves.find(expiry);
    return (it != m_volCurves.end()) ? it->second : nullptr;
}

void OptionGrid::computeValues() {
    if (!m_pricer) return;
    
    // Delegate to unified IOptionPricer::computeValues
    m_pricer->computeValues(this);
    notifyGridUpdated();
}

void OptionGrid::computeValues(uint32_t expiry) {
    if (!m_pricer) return;
    
    // For single-expiry compute, use initValuesCompute + computeValue pattern
    m_pricer->initValuesCompute(this);
    
    auto ed = getExpiry(expiry);
    if (ed) {
        for (const auto& strikePair : ed->getStrikes()) {
            const auto& strike = strikePair.second;
            if (strike->call()) m_pricer->computeValue(strike->call().get());
            if (strike->put()) m_pricer->computeValue(strike->put().get());
        }
    }
    
    m_pricer->finalizeCompute(this);
    notifyGridUpdated();
}

void OptionGrid::computeValues(OptionData* option) {
    if (!m_pricer || !option) return;
    m_pricer->computeValue(option);
}

OptionGreeks OptionGrid::getAggregatedGreeks() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    OptionGreeks total;
    for (const auto& pair : m_expiries) {
        total.accum(pair.second->getAggregatedGreeks());
    }
    return total;
}

OptionGreeks OptionGrid::getExpiryGreeks(uint32_t expiry) const {
    auto ed = getExpiry(expiry);
    return ed ? ed->getAggregatedGreeks() : OptionGreeks();
}

void OptionGrid::addListener(IOptionGridListener* listener) {
    if (listener) {
        m_listeners.push_back(listener);
    }
}

void OptionGrid::removeListener(IOptionGridListener* listener) {
    m_listeners.erase(
        std::remove(m_listeners.begin(), m_listeners.end(), listener),
        m_listeners.end()
    );
}

void OptionGrid::forEachOption(std::function<void(OptionData*)> func) {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    for (auto& option : m_optionsByIndex) {
        func(option.get());
    }
}

void OptionGrid::forEachOption(std::function<void(const OptionData*)> func) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    for (const auto& option : m_optionsByIndex) {
        func(option.get());
    }
}

void OptionGrid::forEachExpiry(std::function<void(ExpiryData*)> func) {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    for (auto& pair : m_expiries) {
        func(pair.second.get());
    }
}

void OptionGrid::forEachExpiry(std::function<void(const ExpiryData*)> func) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    for (const auto& pair : m_expiries) {
        func(pair.second.get());
    }
}

void OptionGrid::notifyExpiryAdded(const ExpiryData* expiry) {
    for (auto* listener : m_listeners) {
        listener->onExpiryAdded(expiry);
    }
}

void OptionGrid::notifyStrikeAdded(const StrikeData* strike) {
    for (auto* listener : m_listeners) {
        listener->onStrikeAdded(strike);
    }
}

void OptionGrid::notifyOptionAdded(const OptionData* option) {
    for (auto* listener : m_listeners) {
        listener->onOptionAdded(option);
    }
}

void OptionGrid::notifyGridUpdated() {
    for (auto* listener : m_listeners) {
        listener->onGridUpdated();
    }
}

} // namespace wt_option

