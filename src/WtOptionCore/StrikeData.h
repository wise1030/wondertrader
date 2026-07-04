/*!
 * \file StrikeData.h
 * \brief Per-strike call/put option pair container (migrated from quantbox)
 *
 * Original: longbeach::optioncore::StrikeData depended on longbeach::
 * InstrumentMDContext, MarketDataContext, OptionData, ExpiryData, and the
 * longbeach core types source_t / instrument_t / expiry_t / right_t.
 *
 * Those types live in not-yet-migrated components (OptionData, ExpiryData,
 * MarketDataContext). To keep this translation unit self-contained and
 * compilable today, the constructor's create() signature is preserved but
 * the parameter types are reduced to lightweight stubs defined here. When
 * the dependent components are migrated, swap the stubs back to the real
 * types (they share names and intent).
 *
 * Naming: longbeach::optioncore -> wt_option.
 */
#pragma once

#include "optioncoretypes.h"

#include <cstdint>
#include <memory>
#include <string>

namespace wt_option {

// ---------------------------------------------------------------------------
// Stub types replacing longbeach core identifiers.
// Replace these with the real migrated types once OptionData / ExpiryData /
// MarketDataContext land. They are intentionally minimal and POD-like so
// that StrikeData compiles standalone today.
// ---------------------------------------------------------------------------
struct source_t
{
    std::string name;
    source_t() = default;
    explicit source_t(const std::string& n) : name(n) {}
};

struct instrument_t
{
    std::string code;
    double      strike = 0.0;
    int         right  = 0;   // 0 = call, 1 = put (matches OR_Call/OR_Put)
    instrument_t() = default;
};

struct expiry_t
{
    std::string date;   // YYYYMMDD or similar
    expiry_t() = default;
    explicit expiry_t(const std::string& d) : date(d) {}
};

enum Right { RT_CALL = 0, RT_PUT = 1 };

// Minimal forward-declared context handle. The real MarketDataContext
// owns market data subscriptions; here it is an opaque placeholder so
// StrikeData's factory signature stays source-compatible.
class MarketDataContext;
using MarketDataContextPtr = std::shared_ptr<MarketDataContext>;

// ExpiryData / OptionData are forward-declared in optioncoretypes.h and
// their shared_ptr aliases (ExpiryDataPtr / OptionDataPtr) are defined there.
// We only need them as incomplete types in the header.

class StrikeData
{
public:
    static StrikeDataPtr create(const MarketDataContextPtr& mdc
        , const source_t& src
        , const ExpiryDataPtr& ed
        , const instrument_t& instr_c
        , const instrument_t& instr_p );

    // WT-compatible factory (no longbeach dependencies)
    static StrikeDataPtr createWT(const ExpiryDataPtr& ed, double strike_px) {
        return StrikeDataPtr(new StrikeData(ed, strike_px));
    }

    const expiry_t& getExpiry() const;
    strike_t getStrikePrice() const { return m_strikePrice; }

    const OptionDataPtr& get( Right r ) const { return m_options[r]; }
    const OptionDataPtr& call() const { return m_options[RT_CALL]; }
    const OptionDataPtr& put()  const { return m_options[RT_PUT]; }

    const ExpiryDataPtr& getExpiryData() const { return m_spExpiryData; }

    double getImpliedVol() const;   // returns call()->values().getImpliedVol()

    /// call_pos + put_pos, this only works if trading data exists
    int32_t getPosition() const;

    // WT-compatible setters (public for OptionGrid)
    void setCall(const OptionDataPtr& odc) { m_options[RT_CALL]=odc; }
    void setPut(const OptionDataPtr& odp)  { m_options[RT_PUT]=odp; }

private:
    // because constructing a strikedata is complicated
    StrikeData(const ExpiryDataPtr& ed, double strike_px);

    void __setCall(const OptionDataPtr& odc) { m_options[RT_CALL]=odc; }
    void __setPut(const OptionDataPtr& odp)  { m_options[RT_PUT]=odp; }

private:
    OptionDataPtr m_options[2];
    ExpiryDataPtr m_spExpiryData;
    strike_t      m_strikePrice;
};

} // namespace wt_option
