/*!
 * \file UnderlyingTradingData.cpp
 * \brief Per-underlying trading state (implementation)
 *
 * Migrated from quantbox optioncore/UnderlyingTradingData.cc (131 lines).
 * Business logic preserved; only dependency layer replaced.
 */
#include "UnderlyingTradingData.h"

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
    if (m_multiMarket.empty())
        return 0;

    if (cancel_only)
        return 0;

    int32_t nTrans = 0;
    const PriceSize& bid = m_multiMarket.getBestBid();
    const PriceSize& ask = m_multiMarket.getBestAsk();

    if (m_quoteExecutor)
    {
        uint32_t orderId = m_quoteExecutor(
            bid.px(), static_cast<uint32_t>(bid.sz()),
            ask.px(), static_cast<uint32_t>(ask.sz()));
        if (orderId != 0)
            ++nTrans;
    }
    else if (m_orderExecutor)
    {
        if (!bid.empty() && m_orderExecutor(true,  bid.px(), static_cast<uint32_t>(bid.sz())))
            ++nTrans;
        if (!ask.empty() && m_orderExecutor(false, ask.px(), static_cast<uint32_t>(ask.sz())))
            ++nTrans;
    }

    m_lastDesiredMarket = m_multiMarket;
    m_currentMarket = m_multiMarket;
    return nTrans;
}

void UnderlyingTradingData::setActive(bool b)
{
    m_bActive = b;
    if (!b)
        m_multiMarket.clear();
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
