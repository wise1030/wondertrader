/*!
 * \file OptionOrderInfo.h
 * \brief Snapshot of option-context state captured at order issue/fill/cancel.
 *
 * Migrated from quantbox optioncore/OptionOrderInfo.h + .cc (49 + 159 lines).
 *
 * Original inherited from longbeach::trading::OrderInfoSlot and wrote detailed
 * BsonLog records (theo, vol, greeks, risk norms, alphas, atmsig, pfdelta, etc.)
 * on each logFill / logCancel / logIssue. Those methods pulled live values out
 * of OptionTradingData / OptionTradingGrid / OptionRisk at event time.
 *
 * Migration:
 *  - trading::OrderInfoSlot / BsonLog / OrderPtr dependencies removed.
 *  - The class is now a plain POD-ish snapshot struct: it stores the weak
 *    pointers it needs (OptionTradingData / OptionTradingGrid) and captures
 *    the same value snapshot the original captured in logIssue, but instead
 *    of serializing to BsonLog it exposes the captured values directly so the
 *    WT layer (which has its own logging) can read them.
 *  - logFill / logCancel / logIssue are preserved as capture hooks that
 *    populate the snapshot fields; they no longer take a BsonLog::Record.
 *  - boost::shared_ptr → std::shared_ptr; dir_t → int32_t (1 = buy, -1 = sell).
 */
#pragma once

#include "optioncoretypes.h"
#include "OptionValues.h"

#include <cstdint>
#include <memory>

namespace wt_option {

class OptionTradingData;
class OptionTradingGrid;
using OptionTradingDataPtr     = std::shared_ptr<OptionTradingData>;
using OptionTradingDataWeakPtr = std::weak_ptr<OptionTradingData>;
using OptionTradingGridPtr     = std::shared_ptr<OptionTradingGrid>;
using OptionTradingGridWeakPtr = std::weak_ptr<OptionTradingGrid>;

class OptionOrderInfo
{
public:
    // dir: 1 = buy, -1 = sell (replaces longbeach dir_t)
    OptionOrderInfo(const OptionTradingDataWeakPtr& wpotd,
                    const OptionTradingGridWeakPtr& wpotg,
                    int32_t dir);

    // Capture hooks (replace logFill / logCancel / logIssue). These populate
    // the snapshot fields below with live values from the trading data / grid.
    void captureIssue();
    void captureFill(double fill_px, uint32_t fill_qty, bool order_done);
    void captureCancel();

    OptionTradingDataPtr getOptionTradingData() const { return m_wpOTD.lock(); }
    const OptionValues& values_at_issue() const { return m_values_at_issue; }

    bool isLateFill() const { return m_bLateFill; }
    void setLateFill(bool b) { m_bLateFill = b; }

    // Snapshot fields populated by the capture hooks (replaces BsonLog output).
    bool      m_bLateFill = false;
    OptionTradingDataWeakPtr m_wpOTD;
    OptionTradingGridWeakPtr m_wpOTG;

    int32_t m_orderType = 0;  // 0 - omm, 1 - shooter (populated from market ident)
    mutable OptionValues m_values_at_issue;
    mutable double m_atmsig0 = 0;
    mutable double m_mid0 = 0;
    mutable double m_pfdelta0 = 0;

    // Fill snapshot
    mutable double m_fillPx = 0;
    mutable uint32_t m_fillQty = 0;
    mutable bool    m_bDone = false;
};

} // namespace wt_option
