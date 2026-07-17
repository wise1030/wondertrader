/*!
 * \file FutureOrderInfo.cpp
 * \brief Future/underlying order snapshot capture (implementation)
 *
 * Migrated from quantbox optioncore/FutureOrderInfo.cc (141 lines).
 * Same capture strategy as OptionOrderInfo.cpp — numeric extraction logic
 * preserved; BsonLog serialization replaced with snapshot field population.
 */
#include "FutureOrderInfo.h"

#include <cmath>

namespace wt_option {

FutureOrderInfo::FutureOrderInfo(const UnderlyingTradingDataWeakPtr& wputd,
                                 const OptionTradingGridWeakPtr& wpotg,
                                 int32_t dir)
    : m_bLateFill(false)
    , m_wpUTD(wputd)
    , m_wpOTG(wpotg)
    , m_orderType(0)
{
    (void)dir;
    // Original derived m_orderType from MultiMarket::MktLevels front ident.
    // Migrated MultiMarket is single-level with no ident(); default 0 = omm.
}

void FutureOrderInfo::captureIssue()
{
    UnderlyingTradingDataPtr utd = m_wpUTD.lock();
    if (!utd)
        return;

    m_mid0 = utd->getMid();
    m_values_at_issue = utd->values();

    m_atmsig0  = NAN;
    m_pfdelta0 = NAN;
    // Grid-level snapshot fields left as NaN (not captured at issue time)'s
    // "otg == null" path.
}

void FutureOrderInfo::captureFill(double fill_px, uint32_t fill_qty, bool order_done)
{
    m_fillPx  = fill_px;
    m_fillQty = fill_qty;
    m_bDone   = order_done;
    if (order_done)
        m_bLateFill = true;
}

void FutureOrderInfo::captureCancel()
{
    // Original logged urank / otype / fwd / risk snapshot.
    // No mutable state changes needed beyond what captureIssue already set.
}

} // namespace wt_option
