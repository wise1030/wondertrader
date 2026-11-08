/*!
 * \file OptionTradingData.h
 * \brief Per-option trading state + order executor hooks
 *
 * Migrated from quantbox optioncore/OptionTradingData.h + .cc (129 + 201 lines).
 * Business logic preserved: quote mode (AUTO/ON/OFF/CLOSE/FLAT), updateRank,
 * willBeActive delta/size gating, str2qmode,
 * setActive clears multiMarket, enable/disable, updateOrders.
 *
 * Convergence (plan 3.1): the execution-state core (markets/executors/
 * QuoteManager/counters/active flag/PnL tracker) now lives in
 * InstrumentTradingDataBase; this class keeps only option-specific state
 * (the OptionData back-reference, risk-data binding, values proxying).
 */
#pragma once

#include "optioncoretypes.h"
#include "OptionValues.h"
#include "OptionData.h"
#include "PnlTracker.h"
#include "InstrumentTradingDataBase.h"

#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include <optional>

namespace wt_option {

class OptionRiskData;
using OptionRiskDataPtr     = std::shared_ptr<OptionRiskData>;
using OptionRiskDataWeakPtr = std::weak_ptr<OptionRiskData>;

class OptionTradingData : public InstrumentTradingDataBase
{
public:
    OptionTradingData();

    // Identity
    uint32_t getExpiry() const;
    ExpiryDataPtr getExpiryData() const;
    OptionDataPtr getOptionData() const { return m_parent.lock(); }
    OptionRiskDataPtr getOptionRiskData() const { return m_spOptionRiskData.lock(); }
    /// P1-f: bind the risk-side per-option data.  Once bound, getPosition()
    /// reads the risk-data position (maintained from broker snapshots and
    /// fills) instead of the external provider.
    void setOptionRiskData(const OptionRiskDataPtr& rd) { m_spOptionRiskData = rd; }

    /// get position with offsets (provided externally)
    int32_t getPosition() const;
    /// raw position (provided externally)
    int32_t getAbsolutePosition() const;

    OptionValues& values(size_t idx = 0);
    const OptionValues& values(size_t idx = 0) const;

    bool willBeActive(double delta_min, double delta_max, int32_t big_size);

    static std::optional<QuoteMode> str2qmode(const std::string& s);

    // Initialization (replaces init(TradingContext, OptionData))
    void init(const OptionDataPtr& od);

protected:
    // 3.1 divergence hook: after the common updateOrders() core, mirror the
    // realized current market onto OptionData (the grid's diff mirror).
    void onOrdersRealized(int32_t txns, bool cancel_only) override;

protected:
    friend class OptionTradingGrid;
    // NOTE: the quantbox onMarketsPriced no-op hook was removed together with
    // the IOptionDataListener facility — updateOrders() is driven explicitly
    // by the grid's batch refresh, not by pricing callbacks.

    OptionDataWeakPtr m_parent;
    OptionRiskDataWeakPtr m_spOptionRiskData;
};

using OptionTradingDataPtr     = std::shared_ptr<OptionTradingData>;
using OptionTradingDataWeakPtr = std::weak_ptr<OptionTradingData>;

} // namespace wt_option
