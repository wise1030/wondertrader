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

// 3.1: the common updateOrders() core lives in InstrumentTradingDataBase;
// this override is the single sanctioned divergence point — mirror the
// realized current market onto OptionData (the grid's diff mirror).
void OptionTradingData::onOrdersRealized(int32_t /*txns*/, bool /*cancel_only*/)
{
    OptionDataPtr od = getOptionData();
    if (od) od->setCurrentMarket(m_currentMarket);
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
