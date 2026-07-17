/*!
 * \file StrikeData.cpp
 * \brief StrikeData implementation (migrated from quantbox)
 *
 * The migrated OptionGrid builds strikes via createWT() + setCall()/setPut()
 * (see OptionGrid::__findOrCreateStrike / __createOption). The legacy
 * create(mdc, src, ed, instr_c, instr_p) factory is retained only for
 * source-compatibility with the original longbeach call sites and is not used
 * by the migrated grid; it builds the StrikeData shell without instantiating
 * OptionData (the migrated grid owns option construction).
 *
 * getImpliedVol()/getPosition() now delegate to the migrated OptionData /
 * OptionTradingData, which are fully available.
 */
#include "StrikeData.h"
#include "OptionData.h"
#include "OptionTradingData.h"

#include <cmath>
#include <stdexcept>

namespace wt_option {

StrikeDataPtr StrikeData::create(const MarketDataContextPtr& /*mdc*/
    , const source_t& /*src*/
    , const ExpiryDataPtr& ed
    , const instrument_t& instr_c
    , const instrument_t& /*instr_p*/ )
{
    // Legacy factory: build the StrikeData shell only. The migrated grid uses
    // createWT() + setCall()/setPut() to attach OptionData objects it owns.
    return StrikeDataPtr(new StrikeData(ed, instr_c.strike));
}

StrikeData::StrikeData(const ExpiryDataPtr& ed, double strike_px)
    : m_options{ OptionDataPtr(), OptionDataPtr() }
    , m_spExpiryData(ed)
    , m_strikePrice(strike_px)
{
}

const expiry_t& StrikeData::getExpiry() const
{
    // The migrated expiry key is a uint32_t on ExpiryData; callers needing the
    // numeric expiry use getExpiryData()->getExpiry(). This legacy string-based
    // accessor is retained for source-compat and returns an empty value.
    static const expiry_t empty;
    return empty;
}

double StrikeData::getImpliedVol() const
{
    // Prefer the call's implied vol (original semantics); fall back to the put.
    if (call()) return call()->getImpliedVol();
    if (put())  return put()->getImpliedVol();
    return std::nan("");
}

int32_t StrikeData::getPosition() const
{
    // call_pos + put_pos via each leg's OptionTradingData (if present).
    int32_t pos = 0;
    if (call()) {
        auto otd = call()->getTradingData();
        if (otd) pos += otd->getPosition();
    }
    if (put()) {
        auto otd = put()->getTradingData();
        if (otd) pos += otd->getPosition();
    }
    return pos;
}

} // namespace wt_option
