/*!
 * \file check_markets.cpp
 * \brief Implementation of check_markets (migrated from quantbox)
 *
 * Original used MultiMarket::MktLevels (sorted price→size maps).
 * Our MultiMarket is single-level best bid/ask, so the comparison is
 * straightforward price/size equality on each side.
 */
#include "check_markets.h"

namespace wt_option {

UPDATE_TYPE check_side(const PriceSize& desired, const PriceSize& current)
{
    bool d_empty = desired.empty();
    bool c_empty = current.empty();

    if (d_empty && !c_empty)
        return UT_CANCEL;
    if (!d_empty && c_empty)
        return UT_NEW;
    if (d_empty && c_empty)
        return UT_NONE;

    // Both non-empty: compare price AND size.
    if (desired.px() != current.px() || desired.sz() != current.sz())
        return UT_UPDATE;

    return UT_NONE;
}

UPDATE_TYPE check_markets(const MultiMarket& desired, const MultiMarket& current)
{
    // Fast path: both fully empty → nothing to do.
    bool d_empty = desired.empty();
    bool c_empty = current.empty();
    if (d_empty && c_empty)
        return UT_NONE;

    UPDATE_TYPE bstatus = check_side(desired.getBestBid(), current.getBestBid());
    UPDATE_TYPE astatus = check_side(desired.getBestAsk(), current.getBestAsk());

    // Combine the two sides.
    // If either side says CANCEL, the whole order must be cancelled/replaced.
    if (bstatus == UT_CANCEL || astatus == UT_CANCEL)
        return UT_CANCEL;
    // If either side says UPDATE, we need to replace.
    if (bstatus == UT_UPDATE || astatus == UT_UPDATE)
        return UT_UPDATE;
    // If either side says NEW, we need a new order.
    if (bstatus == UT_NEW || astatus == UT_NEW)
        return UT_NEW;

    // Both sides are NONE.
    return UT_NONE;
}

} // namespace wt_option
