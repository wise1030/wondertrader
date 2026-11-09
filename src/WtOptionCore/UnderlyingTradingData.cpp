/*!
 * \file UnderlyingTradingData.cpp
 * \brief Per-underlying trading state (implementation)
 *
 * Migrated from quantbox optioncore/UnderlyingTradingData.cc (131 lines).
 * Business logic preserved; only dependency layer replaced.
 */
#include "UnderlyingTradingData.h"
#include "OptionQuoteManager.h"
#include "ExpiryData.h"

namespace wt_option {

UnderlyingTradingData::UnderlyingTradingData(const std::string& code,
                                             const ExpiryDataPtr& ed,
                                             const OptionTradingGridPtr& otg)
    : m_spExpiryData(ed)
    , m_wpOptionTradingGrid(otg)
{
    m_instr = code;
}

uint32_t UnderlyingTradingData::getExpiry() const
{
    return m_spExpiryData ? m_spExpiryData->getExpiry() : 0;
}

double UnderlyingTradingData::getFees(double price) const
{
    return price * m_feePct;
}

int32_t UnderlyingTradingData::getPosition() const
{
    if (m_positionProvider)
        return m_positionProvider();
    return 0;
}

} // namespace wt_option
