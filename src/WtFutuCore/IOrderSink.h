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
    /// V10-P2-10: channel 通道过滤 —— 0=全部 / 1=仅 open 族(is_close==false) /
    /// 2=仅 close 族(is_close==true)。未登记 is_close 的 id 仅在 channel==0 时撤销
    /// (通道未知时盲撤可能误伤另一通道在飞单, 跳过留其自身超时兜底)。
    virtual size_t cancelByPair(wtp::IUftStraCtx* ctx, const std::string& pair_id, int channel = 0) = 0;
};

} // namespace futu
