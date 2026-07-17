/*!
 * \file ExpiryTradingData.h
 * \brief Per-expiry container holding the primary + secondary underliers and
 *        expiry-level greeks / flow filters.
 *
 * Migrated from quantbox optioncore/ExpiryTradingData.h (64 lines).
 * Business logic preserved: enabled flag, primary/secondary underliers,
 * setForward broadcast, expiry greeks slot, vegaflow/deltaflow EMA filters.
 *
 * Dependency replacements:
 *  - longbeach boost::shared_ptr / LONGBEACH_*_SHARED_PTR → std::shared_ptr.
 *  - BOOST_FOREACH → range-for.
 *  - math::EMAFilter → wt_option::EMAFilter (from OptionValues.h).
 *  - longbeach namespace → wt_option.
 *  - No other longbeach types referenced directly.
 */
#pragma once

#include "optioncoretypes.h"
#include "OptionValues.h"        // EMAFilter
#include "UnderlyingTradingData.h"

#include <vector>
#include <memory>

namespace wt_option {

class OptionExpiryGreeks;
using OptionExpiryGreeksPtr = std::shared_ptr<OptionExpiryGreeks>;

class ExpiryTradingData
{
public:
    ExpiryTradingData()
        : m_bEnabled(false)
    {}

    /// set by the Pricer to indicate if the expiry is enabled for trading
    void setEnabled(bool b) { m_bEnabled = b; }
    bool enabled() const { return m_bEnabled; }

    void addSecondaryUTD(const UnderlyingTradingDataPtr& utd) { m_vSecondaryUTD.push_back(utd); }
    const UnderlyingTradingDataPtr& getPrimaryUnderlier() const { return m_spUnderlyingTradingData; }
    const std::vector<UnderlyingTradingDataPtr>& secondary_underliers() const { return m_vSecondaryUTD; }

    const OptionExpiryGreeksPtr& getExpiryGreeks() { return m_spExpiryGreeks; }
    EMAFilter& getVegaflowFilter() { return m_vegaflow_filter; }
    EMAFilter& getDeltaflowFilter() { return m_deltaflow_filter; }

protected:
    friend class OptionTradingGrid;
    void setExpiryGreeks(const OptionExpiryGreeksPtr& eg) { m_spExpiryGreeks = eg; }
    void setPrimaryUnderlier(const UnderlyingTradingDataPtr& utd) { m_spUnderlyingTradingData = utd; }

protected:
    bool m_bEnabled;
    OptionExpiryGreeksPtr m_spExpiryGreeks;
    UnderlyingTradingDataPtr m_spUnderlyingTradingData;
    std::vector<UnderlyingTradingDataPtr> m_vSecondaryUTD;

    EMAFilter m_vegaflow_filter;
    EMAFilter m_deltaflow_filter;
};

using ExpiryTradingDataPtr = std::shared_ptr<ExpiryTradingData>;

} // namespace wt_option
