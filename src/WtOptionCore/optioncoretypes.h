/*!
 * \file optioncoretypes.h
 * \brief Option core type definitions (adapted from quantbox, no longbeach dependency)
 */
#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include <cmath>

namespace wt_option {

// Strike price type
typedef double strike_t;

// Option right
enum OptionRight { OR_Call = 0, OR_Put = 1 };

// Quote mode
enum QuoteMode { QM_AUTO = 0, QM_OFF = -1, QM_ON = 1, QM_CLOSE = 2, QM_FLAT = 3 };

// Order update type (from ControllableTradingGrid check_markets)
enum UPDATE_TYPE {
    UT_NONE = 0,
    UT_NEW = 1,
    UT_UPDATE = 2,
    UT_CANCEL = 3,
    UT_REPLACE = 2,  // same as UPDATE
};

// Smart pointer shortcuts (replace longbeach LONGBEACH_DECLARE_SHARED_PTR)
template<typename T> using shared_ptr = std::shared_ptr<T>;
template<typename T> using weak_ptr = std::weak_ptr<T>;

// Forward declarations
class OptionGreeks;
class StrikeData;
class ExpiryData;
class OptionData;
class OptionGrid;

using OptionGreeksPtr = std::shared_ptr<OptionGreeks>;
using StrikeDataPtr = std::shared_ptr<StrikeData>;
using ExpiryDataPtr = std::shared_ptr<ExpiryData>;
using OptionDataPtr = std::shared_ptr<OptionData>;
using OptionDataWeakPtr = std::weak_ptr<OptionData>;
using OptionGridPtr = std::shared_ptr<OptionGrid>;
using OptionGridWeakPtr = std::weak_ptr<OptionGrid>;

} // namespace wt_option
