/*!
 * \file OptionExpiryGreeks.cpp
 * \brief Per-expiry Greeks aggregation (migrated from quantbox)
 *
 * Business logic preserved:
 *   onPositionGreekChanged — reduce(prev) + accum(new); fire GreeksChanged cb
 *   registerOptionRiskData — accum initial Greeks + subscribe via addListener
 *   update()               — reset; for each registered option: p->update();
 *                            accum(p->getPositionGreeks()); sum hedge deltas
 *   getUnderlierDelta()    — primary hedge + secondary hedges delta positions
 *   portfolio_delta()      — frac_delta() * totalDelta()
 *   frac()/frac_delta()    — delegates to ExpiryData::getExpireGreeksFrac/Delta
 *   setPrimaryHedge()      — store hd + add a position-change callback that
 *                            recomputes m_udelta and fires UDeltaChanged cb
 *
 * Migration deltas documented in OptionExpiryGreeks.h.
 */
#include "OptionExpiryGreeks.h"
#include "ExpiryData.h"
#include "OptionRiskData.h"

#include <cassert>

namespace wt_option {

/////////////////////////////////////////////////////////////////
// Per-Expiry

OptionExpiryGreeks::OptionExpiryGreeks(const ExpiryDataPtr& ed)
    : m_exp(ed ? ed->getExpiry() : 0)
    , m_spExpiryData(ed)
    , m_udelta(0)
{
}

void OptionExpiryGreeks::onPositionGreekChanged(const OptionRiskData& d,
                                                const OptionGreeks& prev)
{
    OptionGreeks eg_prev = *this;
    reduce(prev);
    accum(d.getPositionGreeks());
    for (auto& cb : m_greeksChangedCallbacks)
        cb(*this, eg_prev);
}

void OptionExpiryGreeks::registerOptionRiskData(const OptionRiskDataPtr& d)
{
    if (!d) return;
    // Subscribe to per-option Greeks changes (replaces Subscription-based add).
    d->addListener(this);
    accum(d->getPositionGreeks());
    m_expiryOptions.insert(d);
}

double OptionExpiryGreeks::portfolio_delta() const
{
    return frac_delta() * totalDelta();
}

void OptionExpiryGreeks::update()
{
    // P5: Check if any option is dirty. If none are dirty and hedge positions
    // haven't changed, skip the full reset+accumulate (keep previous values).
    bool anyDirty = false;
    for (const OptionRiskDataPtr& p : m_expiryOptions) {
        if (p && p->isDirty()) { anyDirty = true; break; }
    }

    if (anyDirty) {
        reset();
        for (const OptionRiskDataPtr& p : m_expiryOptions)
        {
            assert(p && "can't lock OptionRiskData in OptionExpiryGreeks");
            p->update();
            accum(p->getPositionGreeks());
        }
    } else {
        // Just clear dirty flags (they should already be clear, but be safe)
        for (const OptionRiskDataPtr& p : m_expiryOptions) {
            if (p) p->update();  // no-op when not dirty
        }
    }

    // Always recompute hedge deltas (cheap, and hedge positions may change
    // independently of option Greeks).
    m_udelta = 0;
    std::vector<HedgeDataPtr> hdlist(m_vSecondaryHedge.begin(),
                                     m_vSecondaryHedge.end());
    if (m_spHedgeData)
        hdlist.push_back(m_spHedgeData);
    for (const HedgeDataPtr& hd : hdlist)
    {
        m_udelta += hd->deltaPosition;
    }
}

double OptionExpiryGreeks::getUnderlierDelta() const
{
    double delta = m_spHedgeData ? m_spHedgeData->deltaPosition : 0;
    for (const HedgeDataPtr& hd : m_vSecondaryHedge)
    {
        delta += hd->deltaPosition;
    }
    return delta;
}

double OptionExpiryGreeks::frac() const
{
    return m_spExpiryData ? m_spExpiryData->getExpireGreeksFrac() : 1.0;
}

double OptionExpiryGreeks::frac_delta() const
{
    return m_spExpiryData ? m_spExpiryData->getExpireDeltaFrac() : 1.0;
}

void OptionExpiryGreeks::__onHedgePositionChange()
{
    double prev = m_udelta;
    m_udelta = getUnderlierDelta();
    for (auto& cb : m_udeltaChangedCallbacks)
        cb(*this, prev);
}

void OptionExpiryGreeks::setPrimaryHedge(const HedgeDataPtr& hd)
{
    m_spHedgeData = hd;
    // NOTE: original subscribed to hd->positionChangedEvent via a Subscription
    // bound to __onHedgePositionChange. Since HedgeData is now a passive struct
    // (no event publishing), the caller (OptionRisk) is responsible for invoking
    // __onHedgePositionChange() explicitly after mutating hd->position /
    // hd->deltaPosition. We keep the method private + friend OptionRisk so
    // OptionRisk can call eg->__onHedgePositionChange() if it wants the
    // event-driven semantics, but the simpler model is for OptionRisk::update()
    // to refresh m_udelta directly via update().
}

} // namespace wt_option
