#pragma once

#include "OrderTypes.h"

namespace wtp { class IUftStraCtx; }

#include <string>

namespace futu {

//==============================================================================
// B6: IOrderSink - narrow order-execution interface (4 methods).
//
// Purpose: decouple risk/closeout/liquidation consumers from the concrete
// OrderRouter (which has rate-limiting, STP, pair-tracking). Enables unit
// testing via MockOrderSink without the full IUftStraCtx stack.
//
// OrderRouter implements this; FutuQuoter bypasses it (direct stra_buy/sell
// for minimum latency). ArbExecutionBridge keeps OrderRouter* (needs
// registerPairOrder, an arb-specific method outside this interface).
//==============================================================================
class IOrderSink
{
public:
    virtual ~IOrderSink() = default;

    virtual OrderSubmitResult submitBuy(wtp::IUftStraCtx* ctx, const char* code,
                                        double price, double qty, Source src,
                                        int flag = 0) = 0;
    virtual OrderSubmitResult submitSell(wtp::IUftStraCtx* ctx, const char* code,
                                         double price, double qty, Source src,
                                         int flag = 0) = 0;
    virtual void cancelAllBySource(wtp::IUftStraCtx* ctx, Source src) = 0;
    virtual size_t cancelByPair(wtp::IUftStraCtx* ctx, const std::string& pair_id) = 0;
};

} // namespace futu
