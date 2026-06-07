/*!
 * \file OrderRouter.cpp
 * \brief Unified Order Router implementation for non-MM sources
 */
#include "OrderRouter.h"
#include "UnifiedOrderTracker.h"
#include "../Includes/WTSMarcos.h"
#include "../Includes/IUftStraCtx.h"
#include "../WTSTools/WTSLogger.h"
#include "../Share/TimeUtils.hpp"

namespace futu {

OrderRouter::OrderRouter()
{
    // Pre-allocate rate counters for all 3 sources
    _rate_counters[static_cast<int>(Source::ARBITRAGE)] = RateCounter{};
    _rate_counters[static_cast<int>(Source::HEDGING)]   = RateCounter{};
    _rate_counters[static_cast<int>(Source::CLOSEOUT)]  = RateCounter{};

    // Pre-allocate active order lists with reasonable capacity
    _active_orders[static_cast<int>(Source::ARBITRAGE)].reserve(16);
    _active_orders[static_cast<int>(Source::HEDGING)].reserve(8);
    _active_orders[static_cast<int>(Source::CLOSEOUT)].reserve(8);
}

void OrderRouter::setRateLimit(Source src, uint32_t limit, uint32_t window_ms)
{
    auto key = static_cast<int>(src);
    auto& rc = _rate_counters[key];
    rc.limit = limit;
    rc.window_ms = window_ms;
}

OrderSubmitResult OrderRouter::submitBuy(wtp::IUftStraCtx* ctx,
                                          const char* code,
                                          double price,
                                          double qty,
                                          Source src,
                                          int flag)
{
    OrderSubmitResult result;

    // FIX P1: 价格0保护 — 防止无效价格下单
    if (price <= 0) {
        result.rejected = true;
        WTSLogger::error("OrderRouter: BUY {} rejected - invalid price={}", code, price);
        return result;
    }

    // 1. Rate limit check
    uint64_t now_ms = TimeUtils::getLocalTimeNow();
    if (!checkRateLimit(src, now_ms))
    {
        result.rate_limited = true;
        WTSLogger::warn("OrderRouter: BUY {} {}@{} rate limited (src={})", code, qty, price, static_cast<int>(src));
        return result;
    }

    // 2. Self-trade prevention
    if (checkSelfTrade(code, true, price))
    {
        result.self_trade_blocked = true;
        WTSLogger::warn("OrderRouter: BUY {} {}@{} blocked by self-trade prevention", code, qty, price);
        return result;
    }

    // 3. Execute via ctx API
    result.localids = ctx->stra_buy(code, price, qty, flag);

    // 4. Track active order
    if (!result.localids.empty())
    {
        for (uint32_t localid : result.localids)
        {
            recordActiveOrder(localid, code, true, price, qty, src, now_ms);
        }
    }

    return result;
}

OrderSubmitResult OrderRouter::submitSell(wtp::IUftStraCtx* ctx,
                                           const char* code,
                                           double price,
                                           double qty,
                                           Source src,
                                           int flag)
{
    OrderSubmitResult result;

    // FIX P1: 价格0保护 — 防止无效价格下单
    if (price <= 0) {
        result.rejected = true;
        WTSLogger::error("OrderRouter: SELL {} rejected - invalid price={}", code, price);
        return result;
    }

    uint64_t now_ms = TimeUtils::getLocalTimeNow();
    if (!checkRateLimit(src, now_ms))
    {
        result.rate_limited = true;
        WTSLogger::warn("OrderRouter: SELL {} {}@{} rate limited (src={})", code, qty, price, static_cast<int>(src));
        return result;
    }

    if (checkSelfTrade(code, false, price))
    {
        result.self_trade_blocked = true;
        WTSLogger::warn("OrderRouter: SELL {} {}@{} blocked by self-trade prevention", code, qty, price);
        return result;
    }

    result.localids = ctx->stra_sell(code, price, qty, flag);

    if (!result.localids.empty())
    {
        for (uint32_t localid : result.localids)
        {
            recordActiveOrder(localid, code, false, price, qty, src, now_ms);
        }
    }

    return result;
}

OrderSubmitResult OrderRouter::submitExitLong(wtp::IUftStraCtx* ctx,
                                               const char* code,
                                               double price,
                                               double qty,
                                               bool isToday,
                                               Source src,
                                               int flag)
{
    OrderSubmitResult result;

    uint64_t now_ms = TimeUtils::getLocalTimeNow();
    if (!checkRateLimit(src, now_ms))
    {
        result.rate_limited = true;
        WTSLogger::warn("OrderRouter: EXIT_LONG {} {}@{} rate limited", code, qty, price);
        return result;
    }

    // Self-trade: closing long = selling, check against MM buy orders
    if (checkSelfTrade(code, false, price))
    {
        result.self_trade_blocked = true;
        WTSLogger::warn("OrderRouter: EXIT_LONG {} {}@{} blocked by self-trade prevention", code, qty, price);
        return result;
    }

    uint32_t localid = ctx->stra_exit_long(code, price, qty, isToday, flag);
    if (localid != 0)
    {
        result.localids.push_back(localid);
        recordActiveOrder(localid, code, false, price, qty, src, now_ms);
    }

    return result;
}

OrderSubmitResult OrderRouter::submitExitShort(wtp::IUftStraCtx* ctx,
                                                const char* code,
                                                double price,
                                                double qty,
                                                bool isToday,
                                                Source src,
                                                int flag)
{
    OrderSubmitResult result;

    uint64_t now_ms = TimeUtils::getLocalTimeNow();
    if (!checkRateLimit(src, now_ms))
    {
        result.rate_limited = true;
        WTSLogger::warn("OrderRouter: EXIT_SHORT {} {}@{} rate limited", code, qty, price);
        return result;
    }

    // Self-trade: closing short = buying, check against MM sell orders
    if (checkSelfTrade(code, true, price))
    {
        result.self_trade_blocked = true;
        WTSLogger::warn("OrderRouter: EXIT_SHORT {} {}@{} blocked by self-trade prevention", code, qty, price);
        return result;
    }

    uint32_t localid = ctx->stra_exit_short(code, price, qty, isToday, flag);
    if (localid != 0)
    {
        result.localids.push_back(localid);
        recordActiveOrder(localid, code, true, price, qty, src, now_ms);
    }

    return result;
}

void OrderRouter::cancelOrder(wtp::IUftStraCtx* ctx, uint32_t localid)
{
    ctx->stra_cancel(localid);
}

void OrderRouter::cancelAllBySource(wtp::IUftStraCtx* ctx, Source src)
{
    auto key = static_cast<int>(src);
    auto it = _active_orders.find(key);
    if (it == _active_orders.end()) return;

    // FIX P2-15: Mark orders as pending_cancel before sending cancel request.
    // This prevents self-trade check from treating them as active orders
    // during the cancel-acknowledge window.
    for (auto& info : it->second)
    {
        if (!info.pending_cancel)
        {
            info.pending_cancel = true;
            ctx->stra_cancel(info.localid);
        }
    }
}

bool OrderRouter::checkSelfTrade(const char* code, bool is_buy, double price) const
{
    if (!_mm_tracker) return false;

    // Delegate to UnifiedOrderTracker's existing self-trade check
    auto result = _mm_tracker->checkSelfTrade(
        std::string(code), is_buy, price, /*is_market_order=*/false);
    if (result.has_risk)
    {
        WTSLogger::warn("[ROUTER] Self-trade blocked: {} {}@{} (conflict with active MM order)",
            code, is_buy ? "BUY" : "SELL", price);
    }
    return result.has_risk;
}

void OrderRouter::onOrderDone(uint32_t localid)
{
    auto src_it = _order_source_map.find(localid);
    if (src_it == _order_source_map.end()) return;

    Source src = src_it->second;
    _order_source_map.erase(src_it);

    auto key = static_cast<int>(src);
    auto orders_it = _active_orders.find(key);
    if (orders_it == _active_orders.end()) return;

    auto& orders = orders_it->second;
    for (auto it = orders.begin(); it != orders.end(); ++it)
    {
        if (it->localid == localid)
        {
            // Swap with last and pop (O(1) removal, order doesn't matter)
            *it = std::move(orders.back());
            orders.pop_back();
            return;
        }
    }
}

const std::vector<ActiveOrderInfo>& OrderRouter::getActiveOrders(Source src) const
{
    static const std::vector<ActiveOrderInfo> empty;
    auto key = static_cast<int>(src);
    auto it = _active_orders.find(key);
    if (it == _active_orders.end()) return empty;
    // FIX P2-15: Filter out pending_cancel orders from the returned list
    std::vector<ActiveOrderInfo> result;
    for (const auto& info : it->second)
    {
        if (!info.pending_cancel)
            result.push_back(info);
    }
    return result;
}

RateCounter& OrderRouter::getRateCounter(Source src)
{
    auto key = static_cast<int>(src);
    return _rate_counters[key];
}

size_t OrderRouter::totalActiveOrders() const
{
    size_t total = 0;
    for (const auto& kv : _active_orders)
    {
        // FIX P2-15: Exclude pending_cancel orders from active count
        for (const auto& info : kv.second)
        {
            if (!info.pending_cancel)
                ++total;
        }
    }
    return total;
}

void OrderRouter::recordActiveOrder(uint32_t localid, const char* code, bool is_buy,
                                     double price, double qty, Source src, uint64_t now_ms)
{
    auto key = static_cast<int>(src);
    auto& orders = _active_orders[key];

    ActiveOrderInfo info;
    info.localid   = localid;
    info.code      = code;
    info.is_buy    = is_buy;
    info.price     = price;
    info.qty       = qty;
    info.source    = src;
    info.submit_ts = now_ms;

    orders.push_back(std::move(info));
    _order_source_map[localid] = src;
}

} // namespace futu
