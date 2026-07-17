/*!
 * \file OptionRisk.h
 * \brief Portfolio risk / Greeks aggregator for an option chain (migrated from quantbox)
 *
 * Original: longbeach::optioncore::OptionRisk inherited
 *   - OptionGridListener (private) to receive grid compute-complete callbacks
 * Construction took TradingContextPtr + OptionGridPtr + hedge instrument list
 * and wired:
 *   - HedgeData (nested class, IPositionListener) for underlier position pushes
 *   - ListenerRelay<IFillListener*> / ListenerRelay<OptionRiskDataListener*>
 *   - grid->subscribe(Subscription, this) for grid-event push
 *   - InstrumentContext::getContractSize / getPositionProvider lookups
 *   - SpotTradingData override of m_pfuDelta
 *
 * Migration:
 *  - namespace: longbeach::optioncore -> wt_option
 *  - Inherits IOptionGridListener (public) replacing private OptionGridListener
 *  - HedgeData: now a passive plain struct (defined in OptionExpiryGreeks.h at
 *    namespace scope). Source compat: `using HedgeData = ::wt_option::HedgeData;`
 *    inside the OptionRisk class body. IPositionListener inheritance removed.
 *  - Constructor: OptionRisk(OptionGridPtr grid) only — no TradingContext,
 *    no hedge list. Hedge instruments are registered via registerHedgeInstrument
 *    (kept public for the engine/strategy to call after the underlying code is
 *    wired). Position is set on HedgeData directly via setPosition.
 *  - ListenerRelay / Subscription removed entirely.
 *  - IOptionPricer* -> IOptionPricerPtr.
 *  - getHedgeInstruments() now returns vector<HedgeDataPtr> (was
 *    vector<InstrumentContextPtr>); the original returned InstrumentContexts
 *    which we no longer have.
 *  - SpotTradingData override path: not applicable — the migrated OptionGrid
 *    carries options + futures only (no spot leg). The override is a
 *    documented no-op; the engine may set the underlier delta externally.
 *
 * Business logic preserved:
 *   createOptionRiskData, onAddOption, update, all(), all() const,
 *   getDelta/getOptionDelta/getPortfolioDelta/getUnderlierDelta/totalDelta,
 *   getPositionGreeks, getExpiryGreeks, getNonZeroPositions(2 overloads),
 *   setOptionPricer, option_pfgreeks, portfolio_delta, raw_delta,
 *   onComputeValuesCompleted.
 */
#ifndef WTOPTIONCORE_OPTIONRISK_H_INCLUDED
#define WTOPTIONCORE_OPTIONRISK_H_INCLUDED

#include "optioncoretypes.h"
#include "OptionGreeks.h"
#include "OptionValues.h"
#include "OptionRiskData.h"
#include "OptionExpiryGreeks.h"
#include "OptionList.h"
#include "IOptionGridListener.h"
#include "IOptionPricer.h"

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace wt_option {

class OptionData;
class ExpiryData;
class OptionGrid;

/// Portfolio risk + Greeks aggregator. Listens to OptionGrid compute-complete
/// events, tracks per-option position Greeks (OptionRiskData), per-expiry
/// aggregates (OptionExpiryGreeks), and underlier hedge deltas (HedgeData).
class OptionRisk
    : public IOptionGridListener
{
public:
    /// Multi-index view on OptionRiskData ordered by instrument code.
    typedef typename OptionList<OptionRiskData>::nth_index<0>::type by_instr;

    // HedgeData is the plain struct defined at namespace scope (see
    // OptionExpiryGreeks.h). Source-compat alias for OptionRisk::HedgeData.
    using HedgeData     = ::wt_option::HedgeData;
    using HedgeDataPtr  = ::wt_option::HedgeDataPtr;

public:
    /// Simplified constructor — grid only (no TradingContext, no hedge list).
    /// Hedge instruments must be registered post-construction via
    /// registerHedgeInstrument() once the strategy/engine wires underliers.
    explicit OptionRisk(const OptionGridPtr& grid);

    // ---- Grid listener (compute-complete triggers update()) ----
    void onComputeValuesCompleted(const IOptionGrid* grid) override;

    // ---- Configuration ----
    void setAutoUpdateGreeks(bool b) { m_bAutoUpdateGreeks = b; }
    void setOptionPricer(const IOptionPricerPtr& p) { m_spOptionPricer = p; }

    // ---- Update / query ----
    void update();
    const by_instr& all();
    const by_instr& all() const;

    /// Option-only delta (no underlier delta).
    double getDelta() const;
    double getOptionDelta() const;
    /// Underlier (hedge) delta contribution.
    double getUnderlierDelta() const;
    /// Option delta + underlier delta.
    double getPortfolioDelta() const;
    /// Unmodified delta (no expire fraction).
    double totalDelta() const;

    const OptionGridPtr&      getOptionGrid() const { return m_spOptionGrid; }
    const OptionGreeksPtr&    getPositionGreeks() const;
    const std::vector<HedgeDataPtr>& getHedgeInstruments() const { return m_hedgeDataList; }

    OptionRiskDataPtr           get(const std::string& instr);
    const OptionExpiryGreeksPtr& getExpiryGreeks(uint32_t exp);
    std::vector<OptionRiskDataPtr> getNonZeroPositions();
    std::vector<OptionRiskDataPtr> getNonZeroPositions(uint32_t exp);

    const OptionGreeks& option_pfgreeks() const;
    double portfolio_delta() const { return getPortfolioDelta(); }
    double raw_delta() const       { return totalDelta(); }

    // ---- Hedge instrument registration (public for post-construction wiring) ----
    HedgeDataPtr registerHedgeInstrument(const std::string& code, uint32_t exp);
    /// Update a hedge's position externally (replaces IPositionListener push).
    void setHedgePosition(const std::string& code, int32_t position);

private:
    OptionRiskDataPtr createOptionRiskData(const OptionDataPtr& od);
    void onAddOption(const OptionDataPtr& od);

    void __onExpiryGreeksChanged(const OptionExpiryGreeks& g, const OptionGreeks& prev);
    void __onUndDeltaChanged(const OptionExpiryGreeks& g, double prev);

    OptionGridPtr                 m_spOptionGrid;
    OptionList<OptionRiskData>    m_optionList;
    OptionGreeksPtr               m_spPositionGreeks;
    std::vector<HedgeDataPtr>     m_hedgeDataList;
    using ExpiryTable = std::map<uint32_t, OptionExpiryGreeksPtr>;
    ExpiryTable                    m_expiryTable;
    IOptionPricerPtr               m_spOptionPricer;
    bool                           m_bAutoUpdateGreeks;
    double                         m_pfuDelta;   // underlier delta
};

using OptionRiskPtr = std::shared_ptr<OptionRisk>;

} // namespace wt_option

#endif // WTOPTIONCORE_OPTIONRISK_H_INCLUDED
