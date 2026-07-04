/*!
 * \file check_markets.h
 * \brief Compare two MultiMarkets and return what update is needed
 *
 * Migrated from quantbox optiontrading/check_markets.h + .cc.
 *
 * Original used MultiMarket::MktLevels (a sorted multi-level map of
 * price→size) and iterated level-by-level. Our wt_option::MultiMarket
 * (OptionValues.h) is a single best-bid/best-ask container with
 * getBestBid()/getBestAsk()/empty()/clear()/setBest()/getBest().
 *
 * Simplified logic:
 *  - compare best bid and best ask of desired vs current
 *  - desired empty + current non-empty → UT_CANCEL
 *  - desired non-empty + current empty → UT_NEW
 *  - both non-empty, prices differ      → UT_UPDATE
 *  - both non-empty, prices match        → UT_NONE
 *  - both empty                          → UT_NONE
 */
#pragma once

#include "optioncoretypes.h"
#include "OptionValues.h"  // MultiMarket, PriceSize

namespace wt_option {

/// Compare two MultiMarkets and return the required UPDATE_TYPE.
UPDATE_TYPE check_markets(const MultiMarket& desired, const MultiMarket& current);

/// Compare a single side (bid or ask) of desired vs current.
UPDATE_TYPE check_side(const PriceSize& desired, const PriceSize& current);

} // namespace wt_option
