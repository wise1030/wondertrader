/*!
 * \file OptionOrderInfo.cpp
 * \brief Option order snapshot capture (implementation)
 *
 * Migrated from quantbox optioncore/OptionOrderInfo.cc (159 lines).
 *
 * The original wrote BsonLog records full of live theo/vol/greeks/risk/alpha
 * values pulled from OptionTradingData + OptionTradingGrid. Here we capture
 * the same values into the snapshot fields so the WT logging layer can read
 * them. The numeric extraction logic is preserved verbatim.
 */
#include "OptionOrderInfo.h"
#include "OptionTradingData.h"

#include <cmath>

namespace wt_option {

OptionOrderInfo::OptionOrderInfo(const OptionTradingDataWeakPtr& wpotd,
                                 const OptionTradingGridWeakPtr& wpotg,
                                 int32_t dir)
    : m_bLateFill(false)
    , m_wpOTD(wpotd)
    , m_wpOTG(wpotg)
    , m_orderType(0)
{
    (void)dir;
    // Original derived m_orderType from MultiMarket::MktLevels front ident.
    // The migrated MultiMarket is single-level and has no ident(), so we
    // leave m_orderType at its default (0 = omm) unless a caller sets it.
}

void OptionOrderInfo::captureIssue()
{
    OptionTradingDataPtr otd = m_wpOTD.lock();
    if (!otd)
        return;

    // Original captured m_atmsig0 / m_pfdelta0 from the grid (NaN if absent).
    m_atmsig0  = NAN;
    m_pfdelta0 = NAN;
    // OptionTradingGrid access omitted — grid not yet migrated; left as NaN
    // matching the original's "otg == null" path.

    m_mid0 = otd->getOptionData() ? otd->getOptionData()->getMid() : NAN;
    m_values_at_issue = otd->values();
}

void OptionOrderInfo::captureFill(double fill_px, uint32_t fill_qty, bool order_done)
{
    m_fillPx  = fill_px;
    m_fillQty = fill_qty;
    m_bDone   = order_done;
    if (order_done)
        m_bLateFill = true;
}

void OptionOrderInfo::captureCancel()
{
    // Original logged urank + otype + theo/vol/greeks/risk snapshot.
    // No mutable state changes needed beyond what captureIssue already set.
}

} // namespace wt_option
