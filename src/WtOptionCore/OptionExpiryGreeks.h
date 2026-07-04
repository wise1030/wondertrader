/*!
 * \file OptionExpiryGreeks.h
 * \brief Per-expiry Greeks aggregation (migrated from quantbox)
 *
 * Original: longbeach::optioncore::OptionExpiryGreeks inherited
 *   - OptionRiskDataListener    (to receive per-option position Greeks changes)
 *   - OptionGreeks              (it IS-A Greeks accumulator)
 * and exposed DECL_EVENT(GreeksChangedEvent/UDeltaChangedEvent) +
 * Subscription-based callbacks.
 *
 * Migration:
 *  - namespace: longbeach::optioncore -> wt_option
 *  - OptionRiskDataListener -> IOptionRiskDataListener
 *  - DECL_EVENT(...Event) + Subscription -> std::function callback vectors
 *    (greeksChangedCallbacks_ / udeltaChangedCallbacks_) matching the pattern
 *    used in IOptionPricer.h
 *  - OptionRisk::HedgeDataPtr -> wt_option::HedgeDataPtr (free struct defined
 *    in OptionRisk.h; both OptionRisk::HedgeData and ::wt_option::HedgeData
 *    refer to the same type via a using-alias, so existing references resolve)
 *  - Subscription m_subs / m_subPrimHedge removed (no longbeach relay)
 *  - boost::shared_ptr -> std::shared_ptr
 *  - expiry_t -> uint32_t
 *  - LONGBEACH_THROW_EXASSERT -> simple assert
 *
 * Business logic preserved:
 *   onPositionGreekChanged  — incremental Greeks update (reduce prev, accum new)
 *   update()                — full recompute from registered options + hedges
 *   portfolio_delta()       — frac_delta() * totalDelta()
 *   frac()/frac_delta()     — delegates to ExpiryData::getExpireGreeksFrac/Delta
 *   getUnderlierDelta()     — sum of hedge getDeltaPosition()
 *   setPrimaryHedge()       — registers primary hedge + position change callback
 */
#ifndef WTOPTIONCORE_OPTIONEXPIRYGREEKS_H_INCLUDED
#define WTOPTIONCORE_OPTIONEXPIRYGREEKS_H_INCLUDED

#include "optioncoretypes.h"
#include "OptionGreeks.h"
#include "OptionRiskData.h"
#include "OptionRiskDataListener.h"

#include <functional>
#include <memory>
#include <set>
#include <vector>
#include <cstdint>

namespace wt_option {

class ExpiryData;
class OptionRisk;

/// Hedge instrument data — plain struct (migrated from OptionRisk::HedgeData).
/// Lives at namespace scope so it can be forward-declared cleanly; OptionRisk.h
/// provides `using HedgeData = ::wt_option::HedgeData;` for source compatibility
/// with the original OptionRisk::HedgeData references.
struct HedgeData
{
    std::string code;
    uint32_t    expiry         = 0;
    int32_t     position       = 0;
    double      deltaPosition  = 0.0;
    double      multiplier     = 1.0;
};
using HedgeDataPtr     = std::shared_ptr<HedgeData>;
using HedgeDataWeakPtr = std::weak_ptr<HedgeData>;

/// Per-expiry Greeks aggregator. Inherits OptionGreeks so it IS-A Greeks table.
/// Underlier delta is tracked separately via getUnderlierDelta().
class OptionExpiryGreeks
    : public OptionRiskDataListener
    , public OptionGreeks
{
public:
    // Callback signatures (replace DECL_EVENT).
    using GreeksChangedSink = std::function<void(const OptionExpiryGreeks&,
                                                 const OptionGreeks&)>;
    using UDeltaChangedSink = std::function<void(const OptionExpiryGreeks&, double)>;

    explicit OptionExpiryGreeks(const ExpiryDataPtr& ed);

    // ---- OptionRiskDataListener ----
    void onPositionGreekChanged(const OptionRiskData& d,
                                const OptionGreeks& prev) override;

    // Full recompute from registered options + hedge deltas.
    void update();

    // Accessors
    const ExpiryDataPtr& getExpiryData() const { return m_spExpiryData; }
    uint32_t getExpiry() const { return m_exp; }

    /// Raw delta (no expire fraction applied).
    double getUnderlierDelta() const;
    double totalDelta() const { return delta() + getUnderlierDelta(); }
    const OptionGreeks& option_greeks() const { return *this; }

    /// With expire fraction applied.
    double portfolio_delta() const;

    double frac() const;
    double frac_delta() const;

    // ---- Hedge wiring ----
    const HedgeDataPtr&           getHedgeData() const        { return m_spHedgeData; }
    const std::set<HedgeDataPtr>& getSecondaryHedgeData() const { return m_vSecondaryHedge; }

    // ---- Callback registration (replaces DECL_EVENT.subscribe) ----
    void addGreeksChangedCallback(GreeksChangedSink cb)
    { m_greeksChangedCallbacks.push_back(std::move(cb)); }
    void addUDeltaChangedCallback(UDeltaChangedSink cb)
    { m_udeltaChangedCallbacks.push_back(std::move(cb)); }

private:
    friend class OptionRisk;

    void setPrimaryHedge(const HedgeDataPtr& hd);
    void registerOptionRiskData(const OptionRiskDataPtr& d);
    void addSecondaryHedge(const HedgeDataPtr& hd) { m_vSecondaryHedge.insert(hd); }

    void __onHedgePositionChange();

    uint32_t         m_exp;
    ExpiryDataPtr    m_spExpiryData;   // for getSettlementFraction() / getExpire*Frac()
    double           m_udelta;         // unmodified by expiry-frac

    std::set<OptionRiskDataPtr> m_expiryOptions;
    HedgeDataPtr                    m_spHedgeData;
    std::set<HedgeDataPtr>          m_vSecondaryHedge;

    std::vector<GreeksChangedSink> m_greeksChangedCallbacks;
    std::vector<UDeltaChangedSink> m_udeltaChangedCallbacks;
};

using OptionExpiryGreeksPtr = std::shared_ptr<OptionExpiryGreeks>;

} // namespace wt_option

#endif // WTOPTIONCORE_OPTIONEXPIRYGREEKS_H_INCLUDED
