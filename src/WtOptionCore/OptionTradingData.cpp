/*!
 * \file OptionTradingData.cpp
 * \brief Per-option trading state + order executor hooks (implementation)
 *
 * Migrated from quantbox optioncore/OptionTradingData.cc (201 lines).
 * Business logic preserved verbatim; only dependency layer replaced.
 */
#include "OptionTradingData.h"
#include "OptionQuoteManager.h"
#include "OptionRiskData.h"
#include "OptionData.h"

#include <cmath>
#include <stdexcept>

namespace wt_option {

OptionTradingData::OptionTradingData()
    : m_bActive(false)
    , m_updateRank(0)
    , m_quoteMode(QM_AUTO)
{
}

void OptionTradingData::init(const OptionDataPtr& od)
{
    m_parent = od;
    m_instr = od->getCode();
}

uint32_t OptionTradingData::getExpiry() const
{
    OptionDataPtr od = m_parent.lock();
    return od ? od->getExpiry() : 0;
}

ExpiryDataPtr OptionTradingData::getExpiryData() const
{
    OptionDataPtr od = getOptionData();
    return od ? od->getExpiryData() : nullptr;
}

void OptionTradingData::onMarketsPriced(const OptionData& od, size_t index)
{
    (void)od;
    (void)index;
    // Original no-op'd the OrderManager update here; with executors there is
    // nothing to do either — updateOrders() is called explicitly by the grid.
}

int32_t OptionTradingData::updateOrders(bool cancel_only)
{
    if (!m_quoteOM) return 0;

    int32_t txns = m_quoteOM->updateOrders(m_multiMarket, cancel_only);
    m_lastDesiredMarket = m_multiMarket;
    if (cancel_only) {
        m_currentMarket.clear();
    } else if (txns > 0) {
        m_currentMarket = m_multiMarket;
    }
    OptionDataPtr od = getOptionData();
    if (od) od->setCurrentMarket(m_currentMarket);
    return txns;
}

void OptionTradingData::setActive(bool b)
{
    m_bActive = b;
    if (!b)
        m_multiMarket.clear();
    if (m_quoteOM)
        m_quoteOM->setActive(b);
}

bool OptionTradingData::isActive() const
{
    return m_bActive;
}

void OptionTradingData::enable()
{
    setActive(true);
    // Original called m_spOrderManager->start(); with executors there is no
    // persistent manager to start — setActive(true) is sufficient.
}

void OptionTradingData::disable()
{
    setActive(false);
    // Original called m_spOrderManager->stop(); with executors we cancel
    // any outstanding desire by clearing the market (done in setActive).
}

int32_t OptionTradingData::getPosition() const
{
    OptionRiskDataPtr rd = getOptionRiskData();
    if (rd)
        return rd->getPosition();
    if (m_positionProvider)
        return m_positionProvider();
    return 0;
}

int32_t OptionTradingData::getAbsolutePosition() const
{
    if (m_positionProvider)
        return m_positionProvider();
    return getPosition();
}

OptionValues& OptionTradingData::values(size_t index)
{
    return getOptionData()->values(index);
}

const OptionValues& OptionTradingData::values(size_t index) const
{
    return getOptionData()->values(index);
}

double OptionTradingData::getDelta() const
{
    return values(0).greeks().delta();
}

bool OptionTradingData::willBeActive(double delta_min, double delta_max, int32_t big_size)
{
    QuoteMode qmode = m_quoteMode;
    if (qmode == QM_ON)
        return true;
    double delta = std::abs(values().greeks().delta());
    int32_t pos = std::abs(getPosition());
    if (qmode == QM_AUTO)
    {
        if (delta > delta_min && delta < delta_max)
            return true;
        if (pos >= big_size)
            return true;
    }
    return false;
}

std::optional<QuoteMode> OptionTradingData::str2qmode(const std::string& s)
{
    if (s == "auto")
        return QM_AUTO;
    if (s == "off")
        return QM_OFF;
    if (s == "on")
        return QM_ON;
    if (s == "close")
        return QM_CLOSE;
    if (s == "flat")
        return QM_FLAT;
    return std::nullopt;
}

} // namespace wt_option
