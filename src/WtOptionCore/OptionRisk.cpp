/*!
 * \file OptionRisk.cpp
 * \brief Portfolio risk management implementation
 */

#include "OptionRisk.h"
#include <boost/noncopyable.hpp>
#include "../WtCore/TraderAdapter.h"
#include <algorithm>
#include <cmath>

namespace wt_option {

//=============================================================================
// OptionRiskData implementation
//=============================================================================

void OptionRiskData::update() {
    if (!option || position == 0) {
        positionGreeks.reset();
        marketValue = 0;
        theoValue = 0;
        return;
    }
    
    // Scale Greeks by position
    positionGreeks = option->greeks();
    positionGreeks.apply(static_cast<double>(position), option->greeks());
    
    // Calculate values
    double mid = option->getMid();
    double theo = option->getTheoPrice();
    double mult = option->getInfo().multiplier;
    
    marketValue = mid * position * mult;
    theoValue = theo * position * mult;
    netPnl = pnl - fees;
}

//=============================================================================
// ExpiryGreeks implementation
//=============================================================================

ExpiryGreeks::ExpiryGreeks(uint32_t expiry)
    : m_expiry(expiry)
    , m_underlierDelta(0)
    , m_expiryFraction(1.0)
{
}

double ExpiryGreeks::getPortfolioDelta() const {
    return m_optionGreeks.delta() * m_expiryFraction + m_underlierDelta;
}

void ExpiryGreeks::setOptionGreeks(const OptionGreeks& greeks) {
    m_optionGreeks = greeks;
}

void ExpiryGreeks::reset() {
    m_optionGreeks.reset();
    m_underlierDelta = 0;
}

void ExpiryGreeks::accumulate(const OptionRiskData& data) {
    m_optionGreeks.accum(data.positionGreeks);
}

//=============================================================================
// HedgeData implementation
//=============================================================================

HedgeData::HedgeData(uint32_t expiry, const std::string& hedgeCode)
    : m_hedgeCode(hedgeCode)
    , m_expiry(expiry)
    , m_position(0)
    , m_multiplier(1.0)
{
}

void HedgeData::setPosition(int32_t pos) {
    m_position = pos;
}

double HedgeData::getDeltaPosition() const {
    return m_position * m_multiplier;
}

//=============================================================================
// OptionRisk implementation
//=============================================================================

OptionRisk::OptionRisk(OptionGridPtr grid)
    : m_grid(grid)
    , m_underlierDelta(0)
    , m_autoUpdateGreeks(true)
{
    // Register as listener
    if (m_grid) {
        m_grid->addListener(this);
        
        // Initialize risk data for existing options
        m_grid->forEachOption([this](OptionData* opt) {
            onOptionAdded(opt);
        });
    }
}

void OptionRisk::update() {
    recalculateGreeks();
    notifyRiskUpdated();
}

OptionRiskDataPtr OptionRisk::getRiskData(const std::string& code) {
    auto it = m_riskData.find(code);
    return (it != m_riskData.end()) ? it->second : nullptr;
}

std::vector<OptionRiskDataPtr> OptionRisk::getAllRiskData() const {
    std::vector<OptionRiskDataPtr> res;
    for (const auto& data : m_riskDataByIndex) {
        if (data) res.push_back(data);
    }
    return res;
}

std::vector<OptionRiskDataPtr> OptionRisk::getNonZeroPositions() const {
    std::vector<OptionRiskDataPtr> res;
    for (const auto& data : m_riskDataByIndex) {
        if (data && data->position != 0) res.push_back(data);
    }
    return res;
}

std::vector<OptionRiskDataPtr> OptionRisk::getPositionsByExpiry(uint32_t expiry) const {
    std::vector<OptionRiskDataPtr> res;
    for (const auto& data : m_riskDataByIndex) {
        if (data && data->position != 0 && data->option->getInfo().expiry == expiry) {
            res.push_back(data);
        }
    }
    return res;
}

double OptionRisk::getDelta() const {
    return m_optionGreeks.delta();
}

double OptionRisk::getUnderlierDelta() const {
    return m_underlierDelta;
}

double OptionRisk::getPortfolioDelta() const {
    return m_optionGreeks.delta() + m_underlierDelta;
}

double OptionRisk::getTotalDelta() const {
    return m_positionGreeks.delta();
}

ExpiryGreeksPtr OptionRisk::getExpiryGreeks(uint32_t expiry) {
    auto it = m_expiryGreeks.find(expiry);
    if (it != m_expiryGreeks.end()) {
        return it->second;
    }
    
    auto greeks = std::make_shared<ExpiryGreeks>(expiry);
    m_expiryGreeks[expiry] = greeks;
    return greeks;
}

void OptionRisk::addHedgeInstrument(uint32_t expiry, const std::string& hedgeCode) {
    auto hedge = std::make_shared<HedgeData>(expiry, hedgeCode);
    m_hedges.push_back(hedge);
}

HedgeDataPtr OptionRisk::getHedgeData(const std::string& code) {
    for (const auto& hedge : m_hedges) {
        if (hedge->getCode() == code) {
            return hedge;
        }
    }
    return nullptr;
}

void OptionRisk::updateHedgePosition(const std::string& code, int32_t position) {
    auto hedge = getHedgeData(code);
    if (hedge) {
        hedge->setPosition(position);
        
        // Recalculate underlier delta
        m_underlierDelta = 0;
        for (const auto& h : m_hedges) {
            m_underlierDelta += h->getDeltaPosition();
        }
    }
}

void OptionRisk::setPosition(const std::string& code, int32_t position) {
    auto it = m_riskData.find(code);
    if (it == m_riskData.end()) return;
    
    auto& data = it->second;
    data->position = position;
    data->update();
    
    notifyPositionChanged(*data);
    
    if (m_autoUpdateGreeks) {
        recalculateGreeks();
    }
}

void OptionRisk::syncPositionsFromTrader(wtp::TraderAdapter* trader) {
    if (!trader) return;
    
    
    // Sync options
    for (auto& pair : m_riskData) {
        auto& rd = pair.second;
        if (!rd->option) continue;
        
        // flag=3 means include both long and short positions net (or total?)
        // TraderAdapter::getPosition(code, validOnly, flag)
        // flag: 0-net, 1-long, 2-short, 3-net(valid only?)
        // Let's check TraderAdapter implementation or usage. 
        // Based on TraderAdapter.h: getPosition(code, bValidOnly, flag)
        // flag is passed to internal logic.
        // Usually we want the net position for risk.
        double pos = trader->getPosition(rd->option->getCode().c_str(), false, 3);
        int32_t netPos = static_cast<int32_t>(pos);
        
        if (rd->position != netPos) {
            rd->position = netPos;
            rd->update();
            notifyPositionChanged(*rd);
        }
    }
    
    // Sync hedges
    for (auto& hedge : m_hedges) {
        double pos = trader->getPosition(hedge->getCode().c_str(), false, 3);
        int32_t netPos = static_cast<int32_t>(pos);
        
        if (hedge->getPosition() != netPos) {
            updateHedgePosition(hedge->getCode(), netPos);
        }
    }
    
    if (m_autoUpdateGreeks) {
        recalculateGreeks();
        notifyRiskUpdated();
    }
}

void OptionRisk::addFill(const std::string& code, int32_t qty, double price, double fee) {
    // Check Options
    auto it = m_riskData.find(code);
    if (it != m_riskData.end()) {
        auto& data = it->second;
        data->position += qty;
        data->fees += fee;
        data->update();
        notifyPositionChanged(*data);
    }
    else {
        // Check Hedges
        auto hedge = getHedgeData(code);
        if (hedge) {
            hedge->setPosition(hedge->getPosition() + qty);
            // Verify if we need to track fees for hedges in future
        }
    }
    
    if (m_autoUpdateGreeks) {
        recalculateGreeks(); // This handles notifying listeners
        notifyRiskUpdated();
    }
}

void OptionRisk::addListener(IOptionRiskListener* listener) {
    if (listener) {
        m_listeners.push_back(listener);
    }
}

void OptionRisk::removeListener(IOptionRiskListener* listener) {
    m_listeners.erase(
        std::remove(m_listeners.begin(), m_listeners.end(), listener),
        m_listeners.end()
    );
}

void OptionRisk::onOptionAdded(const OptionData* option) {
    if (!option) return;
    std::string code = option->getCode();
    if (m_riskData.find(code) == m_riskData.end()) {
        auto data = std::make_shared<OptionRiskData>();
        
        // We ensure we get the shared_ptr version from grid
        auto optPtr = m_grid->getOption(code);
        if (optPtr) {
            data->option = optPtr;
            m_riskData[code] = data;
            
            uint32_t id = optPtr->getInternalId();
            if (id >= m_riskDataByIndex.size()) {
                m_riskDataByIndex.resize(id + 1);
            }
            m_riskDataByIndex[id] = data;
        }
    }
}

void OptionRisk::onGridUpdated() {
    if (m_autoUpdateGreeks) {
        recalculateGreeks();
    }
}

void OptionRisk::recalculateGreeks() {
    m_positionGreeks.reset();
    m_optionGreeks.reset();
    
    // Reset expiry Greeks
    for (auto& pair : m_expiryGreeks) {
        pair.second->reset();
    }
    
    // Calculate option greeks
    for (auto& data : m_riskDataByIndex) {
        if (!data) continue;
        data->update();
        
        // Always accumulate for unweighted OptionGreeks
        m_optionGreeks.accum(data->option->greeks());
        
        if (data->position != 0) {
            m_positionGreeks.accum(data->positionGreeks);
            
            auto itGreeks = m_expiryGreeks.find(data->option->getInfo().expiry);
            if (itGreeks != m_expiryGreeks.end()) {
                itGreeks->second->accumulate(*data);
            }
        }
    }
    // Update expiry Greeks with hedge/underlying delta from ExpiryData
    if (m_grid) {
        m_underlierDelta = 0;
        
        // Check each expiry for UnderlyingTradingData
        for(uint32_t expiry : m_grid->getExpiryDates()) {
            auto expiryData = m_grid->getExpiry(expiry);
            if(expiryData) {
                auto utd = expiryData->getUnderlyingTradingData();
                if(utd) {
                    double deltaPos = utd->getPosition(); // Assumption: Multiplier 1 or handled in UTD
                    // Add to ExpiryGreeks
                    auto expiryGreeks = getExpiryGreeks(expiry);
                    expiryGreeks->setUnderlierDelta(expiryGreeks->getUnderlierDelta() + deltaPos);
                    
                    // Add to Total Portfolio Delta
                    m_underlierDelta += deltaPos;
                }
            }
        }
    }
    
    // Also include manual hedges if any (legacy or separate)
    for (const auto& hedge : m_hedges) {
        auto expiryGreeks = getExpiryGreeks(hedge->getExpiry());
        double d = hedge->getDeltaPosition();
        expiryGreeks->setUnderlierDelta(expiryGreeks->getUnderlierDelta() + d);
        m_underlierDelta += d;
    }
}

void OptionRisk::notifyRiskUpdated() {
    for (auto* listener : m_listeners) {
        listener->onRiskUpdated(*this);
    }
}

void OptionRisk::notifyPositionChanged(const OptionRiskData& data) {
    for (auto* listener : m_listeners) {
        listener->onPositionChanged(data);
    }
}

} // namespace wt_option
