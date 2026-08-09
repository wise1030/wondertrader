#pragma once

#include <cstdint>
#include "../Includes/ExecuteDefs.h"  // wtp::OrderIDs
#include "../Includes/FasterDefs.h"

namespace futu {

/// Order source classification for priority routing
enum class Source : uint8_t
{
    ARBITRAGE = 0, ///< 套利下单
    HEDGING = 1,   ///< 对冲下单
    CLOSEOUT = 2   ///< 平仓/强平 (highest priority)
};

/// Get numeric priority for a source (higher = more important)
inline int sourcePriority(Source src)
{
    return static_cast<int>(src);
}

/// Order submission result (shared by IOrderSink and OrderRouter)
struct OrderSubmitResult
{
    wtp::OrderIDs localids;          ///< Local order IDs from exchange
    bool rate_limited = false;       ///< True if blocked by rate limit
    bool self_trade_blocked = false; ///< True if blocked by self-trade check
    bool rejected = false;           ///< True if rejected (e.g. invalid price)
};

} // namespace futu
