/*!
 * \file OptionRiskData.h
 * \brief Position-weighted Greeks for a single option (migrated from quantbox)
 *
 * Original: longbeach::optioncore::OptionRiskData inherited
 *   - trading::IPositionListener (push position updates from IPositionProvider)
 *   - PrioritizedListenerList<OptionRiskDataListener*> (relay listeners)
 * Construction took TradingContextPtr + OptionDataPtr and pulled a position
 * provider + contract size from the TradingContext.
 *
 * Migration:
 *  - namespace: longbeach::optioncore -> wt_option
 *  - Removed trading::IPositionListener and the TradingContext dependency
 *    entirely. Position is now set externally via setPosition()/addFill()
 *    (matching the simplified_v1 pattern and the Trade-Shock protection
 *    fields required by the task).
 *  - Removed PrioritizedListenerList<> relay; replaced with a plain
 *    std::vector<IOptionRiskDataListener*> + addListener/removeListener/notify.
 *  - Removed shared_vector<IPositionOffset*> offset mechanism (it depended on
 *    the longbeach Subscription machinery). An int32_t offset_ field is kept
 *    as a plain external hook for callers that need a delta adjustment.
 *  - contractSize now passed in via setContractSize (default = option's
 *    OptionInfo::multiplier); was previously pulled from InstrumentContext.
 *  - Added lastBuyFillPrice / lastSellFillPrice / lastFillTime + addFill()
 *    for Trade-Shock protection (records last buy/sell fill prices and time).
 *
 * Business logic (update(), getPositionGreeks(), OptionGreeks scaling)
 * is preserved unchanged.
 */
#ifndef WTOPTIONCORE_OPTIONRISKDATA_H_INCLUDED
#define WTOPTIONCORE_OPTIONRISKDATA_H_INCLUDED

#include "optioncoretypes.h"
#include "OptionGreeks.h"
#include "OptionValues.h"
#include "OptionRiskDataListener.h"

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace wt_option {

class OptionData;
class ExpiryData;

class OptionRiskData
{
public:
    explicit OptionRiskData(const OptionDataPtr& od);

    // Identity (delegate to OptionData)
    // NOTE: getInstrument() returns std::string by value (not const ref) because
    // OptionList's multi_index const_mem_fun key requires a by-value return.
    std::string        getInstrument() const;
    uint32_t           getExpiry() const;
    OptionRight        getRight() const;
    double             getStrikePrice() const;
    double             getContractSize() const;

    // Underlying objects
    const OptionDataPtr&  getOptionData();
    const ExpiryDataPtr&  getExpiryData();
    const OptionValues&   getOptionValues();

    // Position & Greeks
    void               update();
    const OptionGreeks& getPositionGreeks() const;
    int32_t            getPosition() const;

    /// Direct position setter (replaces IPositionListener push model).
    void setPosition(int32_t pos) { m_position = pos; }

    /// Optional external offset (replaces shared_vector<IPositionOffset*>).
    void setOffset(int32_t off) { m_offset = off; }

    // ---- Trade-Shock protection: fill tracking (task-required) ----
    /// Record a fill. qty>0 = buy, qty<0 = sell. Updates last fill prices/time.
    /// \param qty     signed fill quantity (positive=buy, negative=sell)
    /// \param price   fill price
    /// \param time    fill timestamp (epoch ms or any monotonic uint64)
    void addFill(int32_t qty, double price, uint64_t time = 0);

    double  getLastBuyFillPrice()  const { return m_lastBuyFillPrice; }
    double  getLastSellFillPrice() const { return m_lastSellFillPrice; }
    uint64_t getLastFillTime()     const { return m_lastFillTime; }

    // ---- Listener relay (replaces PrioritizedListenerList) ----
    void addListener(OptionRiskDataListener* l)    { m_listeners.push_back(l); }
    void removeListener(OptionRiskDataListener* l);
    void notifyPositionChanged(const OptionGreeks& prev);

    // Configurable contract size (defaults to OptionData::getMultiplier)
    void setContractSize(double cs) { m_contractSize = cs; }

private:
    OptionDataPtr  m_spOptionData;
    OptionGreeks   m_positionGreeks;
    double         m_contractSize;
    int32_t        m_position;
    int32_t        m_offset;

    // Trade-Shock protection fields
    double   m_lastBuyFillPrice;
    double   m_lastSellFillPrice;
    uint64_t m_lastFillTime;

    std::vector<OptionRiskDataListener*> m_listeners;
};

using OptionRiskDataPtr     = std::shared_ptr<OptionRiskData>;
using OptionRiskDataWeakPtr = std::weak_ptr<OptionRiskData>;

} // namespace wt_option

#endif // WTOPTIONCORE_OPTIONRISKDATA_H_INCLUDED
