#pragma once

#include "arb/AsyncArbitrageExecutor.h"  // FixedString24 + LockFreeQueue
#include <cstdint>

namespace futu {

//==============================================================================
// C11: TdSpi minimal offload - defer non-urgent logging from fill path
// to tick path via SPSC queue. requoteAfterFill and all bookkeeping stay
// synchronous. Depends on A5 (FixedString24 trivially copyable).
//==============================================================================
struct TdSpiLogEvent
{
    int level = 0;             // 0=debug, 1=info
    FixedString24 code;        // contract code
    FixedString24 action;      // buy/sell
    double vol = 0;
    double price = 0;
    double delta = 0;
    FixedString24 effect;      // open/close/reduce
    uint64_t timestamp = 0;
};
static_assert(std::is_trivially_copyable_v<TdSpiLogEvent>,
              "TdSpiLogEvent must be trivially copyable for SPSC queue");

using TdSpiLogQueue = LockFreeQueue<TdSpiLogEvent, 256>;

} // namespace futu
