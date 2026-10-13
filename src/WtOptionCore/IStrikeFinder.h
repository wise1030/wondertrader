/*!
 * \file IStrikeFinder.h
 * \brief Strike-grid abstraction — absorbed from quantbox optioncore
 *        (longbeach::optioncore::IStrikeFinder).
 *
 * Purpose (R3): exchange strike grids are NOT uniform (CFFEX IO monthly 50 /
 * quarterly 100; DCE MO tiered 25/50/100). Snapping reference prices to the
 * listed grid before nearest-strike lookups prevents selecting "virtual"
 * strikes that no contract actually references.
 */
#pragma once

#include <cstdint>
#include <cmath>
#include <memory>

namespace wt_option {

class IStrikeFinder
{
public:
    enum ExpiryType { WEEKLY, MONTHLY, QUARTERLY };

    virtual ~IStrikeFinder() {}

    /// Snap a reference price to the nearest listed strike on this grid.
    virtual double getStrike(double price) const = 0;

    /// Listed interval at this price level (tiered grids override).
    virtual uint32_t getStrikeInterval(double price) const = 0;
};

using IStrikeFinderPtr = std::shared_ptr<IStrikeFinder>;

/// Round to an integer interval (shared helper for concrete finders).
inline double roundToInterval(double price, uint32_t interval)
{
    if (interval == 0) return price;
    return std::round(price / interval) * interval;
}

} // namespace wt_option
