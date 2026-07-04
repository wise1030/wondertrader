/*!
 * \file FutureOrderInfo.h
 * \brief Snapshot of underlying/future-context state captured at order
 *        issue/fill/cancel.
 *
 * Migrated from quantbox optioncore/FutureOrderInfo.h + .cc (47 + 141 lines).
 *
 * Same migration strategy as OptionOrderInfo:
 *  - trading::OrderInfoSlot / BsonLog / OrderPtr dependencies removed.
 *  - Plain snapshot struct storing weak ptrs to UnderlyingTradingData /
 *    OptionTradingGrid and capturing UnderlyingValues at issue time.
 *  - logFill / logCancel / logIssue → captureFill / captureCancel / captureIssue
 *    populating snapshot fields instead of writing BsonLog records.
 *  - boost::shared_ptr → std::shared_ptr; dir_t → int32_t (1 = buy, -1 = sell).
 */
#pragma once

#include "optioncoretypes.h"
#include "UnderlyingTradingData.h"

#include <cstdint>
#include <memory>

namespace wt_option {

class OptionTradingGrid;
using OptionTradingGridPtr     = std::shared_ptr<OptionTradingGrid>;
using OptionTradingGridWeakPtr = std::weak_ptr<OptionTradingGrid>;

class FutureOrderInfo
{
public:
    // dir: 1 = buy, -1 = sell (replaces longbeach dir_t)
    FutureOrderInfo(const UnderlyingTradingDataWeakPtr& wputd,
                    const OptionTradingGridWeakPtr& wpotg,
                    int32_t dir);

    // Capture hooks (replace logFill / logCancel / logIssue).
    void captureIssue();
    void captureFill(double fill_px, uint32_t fill_qty, bool order_done);
    void captureCancel();

    UnderlyingTradingDataPtr getUnderlyingTradingData() const { return m_wpUTD.lock(); }
    const UnderlyingValues& values_at_issue() const { return m_values_at_issue; }

    bool isLateFill() const { return m_bLateFill; }
    void setLateFill(bool b) { m_bLateFill = b; }

    // Snapshot fields populated by the capture hooks.
    bool      m_bLateFill = false;
    UnderlyingTradingDataWeakPtr m_wpUTD;
    OptionTradingGridWeakPtr m_wpOTG;
    int32_t m_orderType = 0;  // 0 - omm, 1 - shooter
    mutable UnderlyingValues m_values_at_issue;
    mutable double m_atmsig0 = 0;
    mutable double m_mid0 = 0;
    mutable double m_pfdelta0 = 0;

    // Fill snapshot
    mutable double m_fillPx = 0;
    mutable uint32_t m_fillQty = 0;
    mutable bool    m_bDone = false;
};

} // namespace wt_option
