/*!
 * \file OptionRiskData.cpp
 * \brief Position-weighted Greeks for a single option (migrated from quantbox)
 *
 * Business logic preserved:
 *   update()             — risk_mult * position * contractSize scaled Greeks
 *   getPositionGreeks()  — accessor
 *   getPosition()        — position + offsets (offsets now a single int32_t)
 *   onPositionUpdated()  — REMOVED (was IPositionListener push); callers now
 *                          call setPosition()/addFill() then notifyPositionChanged()
 *
 * Migration deltas documented in OptionRiskData.h.
 */
#include "OptionRiskData.h"
#include "OptionRiskDataListener.h"
#include "OptionData.h"
#include "ExpiryData.h"

#include <algorithm>

namespace wt_option {

OptionRiskData::OptionRiskData(const OptionDataPtr& od)
    : m_spOptionData(od)
    , m_positionGreeks()
    , m_contractSize(od ? od->getMultiplier() : 1.0)
    , m_position(0)
    , m_offset(0)
    , m_lastBuyFillPrice(0.0)
    , m_lastSellFillPrice(0.0)
    , m_lastFillTime(0)
{
}

std::string OptionRiskData::getInstrument() const
{
    return m_spOptionData->getCode();
}

uint32_t OptionRiskData::getExpiry() const
{
    return m_spOptionData->getExpiry();
}

OptionRight OptionRiskData::getRight() const
{
    return m_spOptionData->getRight();
}

double OptionRiskData::getStrikePrice() const
{
    return m_spOptionData->getStrike();
}

double OptionRiskData::getContractSize() const
{
    return m_contractSize;
}

void OptionRiskData::update()
{
    ExpiryDataPtr ed = m_spOptionData->getExpiryData();
    double risk_mult = ed ? ed->getRiskMultiplier() : 1.0;
    double mult = getPosition() * getContractSize();
    m_positionGreeks.apply(mult * risk_mult, m_spOptionData->greeks());
}

const OptionGreeks& OptionRiskData::getPositionGreeks() const
{
    return m_positionGreeks;
}

const OptionDataPtr& OptionRiskData::getOptionData()
{
    return m_spOptionData;
}

const ExpiryDataPtr& OptionRiskData::getExpiryData()
{
    // OptionData::getExpiryData() returns by value (shared_ptr); we cache
    // the result in a member for ref-return. Since the call below returns a
    // fresh shared_ptr each time, we instead expose the cached pointer via
    // a thread-unsafe static-ish pattern — simplest: return OptionData's ptr.
    // To keep the ref-return contract, we rely on OptionData holding the
    // ExpiryDataPtr in its m_expiryData weak_ptr; resolving it here once is
    // acceptable for read-only callers in the synchronous Greeks update path.
    static thread_local ExpiryDataPtr s_cached;
    s_cached = m_spOptionData->getExpiryData();
    return s_cached;
}

const OptionValues& OptionRiskData::getOptionValues()
{
    return m_spOptionData->activeValues();
}

int32_t OptionRiskData::getPosition() const
{
    return m_position + m_offset;
}

void OptionRiskData::addFill(int32_t qty, double price, uint64_t time)
{
    if (qty > 0)
    {
        m_lastBuyFillPrice = price;
    }
    else if (qty < 0)
    {
        m_lastSellFillPrice = price;
    }
    if (time > m_lastFillTime)
        m_lastFillTime = time;
    m_position += qty;
}

void OptionRiskData::removeListener(OptionRiskDataListener* l)
{
    auto it = std::find(m_listeners.begin(), m_listeners.end(), l);
    if (it != m_listeners.end())
        m_listeners.erase(it);
}

void OptionRiskData::notifyPositionChanged(const OptionGreeks& prev)
{
    // Iterate by index: listeners may add/remove during dispatch.
    for (size_t i = 0; i < m_listeners.size(); ++i)
    {
        m_listeners[i]->onPositionGreekChanged(*this, prev);
    }
}

} // namespace wt_option
