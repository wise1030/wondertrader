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
    : m_bActive(false)
    , m_fwd(0)
    , m_updateRank(0)
    , m_quoteMode(QM_AUTO)
    , m_bDisplayShark(false)
    , m_instr(code)
    , m_spExpiryData(ed)
    , m_spOptionTradingGrid(otg)
{
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

int32_t UnderlyingTradingData::updateOrders(bool cancel_only)
{
    if (!m_quoteOM) return 0;

    int32_t txns = m_quoteOM->updateOrders(m_multiMarket, cancel_only);
    m_lastDesiredMarket = m_multiMarket;
    if (cancel_only) {
        m_currentMarket.clear();
    } else if (txns > 0) {
        m_currentMarket = m_multiMarket;
    }
    return txns;
}

void UnderlyingTradingData::setActive(bool b)
{
    m_bActive = b;
    if (!b)
        m_multiMarket.clear();
    if (m_quoteOM)
        m_quoteOM->setActive(b);
}

void UnderlyingTradingData::enable()
{
    setActive(true);
}

void UnderlyingTradingData::disable()
{
    setActive(false);
}

bool UnderlyingTradingData::isActive() const
{
    return m_bActive;
}

PnlTrackerPtr UnderlyingTradingData::getPnlTracker()
{
    if (!m_pnlTracker)
    {
        PnlTrackerPtr pt(new PnlTracker(m_contractSize));
        m_pnlTracker = pt;
    }
    return m_pnlTracker;
}

} // namespace wt_option
