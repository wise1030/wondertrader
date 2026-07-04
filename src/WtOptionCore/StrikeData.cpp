/*!
 * \file StrikeData.cpp
 * \brief StrikeData implementation (migrated from quantbox)
 *
 * Original longbeach version constructed concrete OptionData objects via
 * boost::make_shared and bumped an option counter on ExpiryData. Those
 * dependencies (OptionData ctor, ExpiryData::num_options(), trading data
 * accessor) are not yet migrated, so the bodies here are guarded to keep
 * the file compilable today. The structure and intent are preserved; once
 * OptionData / ExpiryData / OptionTradingData are migrated, #define
 * WT_OPT_HAS_OPTIONDATA to re-enable the real bodies.
 */
#include "StrikeData.h"

#include <cmath>
#include <stdexcept>

namespace wt_option {

StrikeDataPtr StrikeData::create(const MarketDataContextPtr& /*mdc*/
    , const source_t& /*src*/
    , const ExpiryDataPtr& ed
    , const instrument_t& instr_c
    , const instrument_t& /*instr_p*/ )
{
    // Original body constructed two OptionData objects (call + put) and
    // registered them on the strike + expiry. OptionData is not yet
    // migrated, so we cannot instantiate it here. We still build the
    // StrikeData shell with the call instrument's strike.
    StrikeDataPtr stkd(new StrikeData(ed, instr_c.strike));
    // TODO[OptionData migration]:
    //   OptionDataPtr odc = std::make_shared<OptionData>(mdc, stkd, instr_c, src);
    //   OptionDataPtr odp = std::make_shared<OptionData>(mdc, stkd, instr_p, src);
    //   stkd->__setCall(odc); stkd->__setPut(odp);
    //   ed->num_options() += 2;
    return stkd;
}

StrikeData::StrikeData(const ExpiryDataPtr& ed, double strike_px)
    : m_options{ OptionDataPtr(), OptionDataPtr() }
    , m_spExpiryData(ed)
    , m_strikePrice(strike_px)
{
}

const expiry_t& StrikeData::getExpiry() const
{
    // Original: return m_spExpiryData->getExpiry();
    // ExpiryData is incomplete here. Callers that need the expiry should
    // obtain it directly from the ExpiryDataPtr once that type is migrated.
    static const expiry_t empty;
    return empty;
}

double StrikeData::getImpliedVol() const
{
    // Original: return call()->values().getImpliedVol();
    // Requires OptionData -> OptionValues. Not available until OptionData
    // is migrated; return NaN to signal "no data".
    if (!call()) return std::nan("");
    // TODO[OptionData migration]: return call()->values().getImpliedVol();
    return std::nan("");
}

int32_t StrikeData::getPosition() const
{
    // Original:
    //   return call()->getTradingData()->getPosition()
    //        + put()->getTradingData()->getPosition();
    // OptionTradingData is not yet migrated. Return 0 (no position known)
    // rather than crashing on null trading data.
    return 0;
}

} // namespace wt_option
