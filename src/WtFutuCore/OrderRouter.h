/*!
 * \file OrderRouter.h
 * \brief Unified Order Router for non-MM order sources (Arbitrage, Hedge, Closeout)
 * 
 * Design principle:
 *   - MM (market making) orders go through FutuQuoter directly → ctx API (zero overhead)
 *   - Non-MM orders (arb/hedge/closeout) go through OrderRouter for:
 *     1. Self-trade prevention against MM active orders
 *     2. Per-source rate limiting
 *     3. Priority-based routing (closeout > hedge > arb)
 *     4. Unified order tracking and audit trail
 * 
 * Latency budget: < 500ns per order on hot path
 *   - RateCounter check: ~5ns (inline arithmetic)
 *   - Self-trade check: ~50-200ns (hash lookup + small vector scan)
 *   - ActiveOrderInfo record: ~100ns (pre-allocated, no heap alloc on steady state)
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "../Includes/FasterDefs.h"
#include "../Includes/ExecuteDefs.h"

NS_WTP_BEGIN
class IUftStraCtx;
NS_WTP_END

namespace futu {

class UnifiedOrderTracker;

/// Order source classification for priority routing
enum class Source : uint8_t
{
    ARBITRAGE     = 0,  ///< 套利下单
    HEDGING       = 1,  ///< 对冲下单
    CLOSEOUT      = 2   ///< 平仓/强平 (highest priority)
};

/// Get numeric priority for a source (higher = more important)
inline int sourcePriority(Source src)
{
    return static_cast<int>(src);
}

/// Rate counter for per-source throttling (cache-friendly, inline)
struct RateCounter
{
    uint32_t count       = 0;   ///< Orders in current window
    uint64_t window_start = 0;  ///< Window start timestamp (ms)
    uint32_t limit       = 0;   ///< Max orders per window (0 = unlimited)
    uint32_t window_ms   = 1000;///< Window duration in ms

    __attribute__((always_inline))
    inline bool check(uint64_t now_ms)
    {
        if (limit == 0) return true;
        if (now_ms - window_start >= window_ms)
        {
            count = 0;
            window_start = now_ms;
        }
        return count < limit;
    }

    __attribute__((always_inline))
    inline void increment() { ++count; }
};

/// Active order tracking info (lightweight, uses string_view for code)
struct ActiveOrderInfo
{
    uint32_t    localid   = 0;    ///< Local order ID
    std::string code;             ///< Standard contract code (owned, for safety)
    bool        is_buy    = true; ///< Buy/sell direction
    double      qty       = 0;    ///< Order quantity
    double      price     = 0;    ///< Order price
    Source      source    = Source::ARBITRAGE; ///< Order source
    uint64_t    submit_ts = 0;    ///< Submit timestamp (ms)
    // Track pending cancel state to avoid counting cancelled orders as active
    // in self-trade checks. Without this, cancelAllBySource sends cancel but the order
    // remains in _active_orders until onOrderDone, so self-trade check may block new orders
    // that conflict with a cancelling (but not yet cancelled) order.
    bool        pending_cancel = false;
};

/// Order submission result
struct OrderSubmitResult
{
    wtp::OrderIDs localids;       ///< Local order IDs from exchange
    bool          rate_limited = false; ///< True if blocked by rate limit
    bool          self_trade_blocked = false; ///< True if blocked by self-trade check
    bool          rejected = false;  ///< True if rejected (e.g. invalid price)
};

/// Unified Order Router (for non-MM sources only)
///
/// Routes orders from arbitrage, hedging, and closeout sources with:
///   - Priority-based routing
///   - Per-source rate limiting
///   - Self-trade prevention against MM active orders
///
/// Thread safety: NOT thread-safe. Must be called from the main thread only
/// (same as FutuQuoter and IUftStraCtx).
class OrderRouter
{
public:
    OrderRouter();
    ~OrderRouter() = default;

    //==========================================================================
    // Configuration
    //==========================================================================

    /// Set the order tracker for MM order access (self-trade prevention)
    void setOrderTracker(UnifiedOrderTracker* tracker) { _mm_tracker = tracker; }

    /// Set rate limit for a specific source
    void setRateLimit(Source src, uint32_t limit, uint32_t window_ms = 1000);

    //==========================================================================
    // Order Submission (non-MM only)
    //==========================================================================

    /// Submit a buy order (enter long) through the router
    /// @param ctx     UFT strategy context
    /// @param code    Standard contract code
    /// @param price   Order price
    /// @param qty     Order quantity
    /// @param src     Order source for priority/rate-limiting
    /// @param flag    Order flag: 0-normal, 1-fak, 2-fok
    /// @return Submit result with localids and blocking info
    OrderSubmitResult submitBuy(wtp::IUftStraCtx* ctx,
                                const char* code,
                                double price,
                                double qty,
                                Source src,
                                int flag = 0);

    /// Submit a sell order (enter short) through the router
    OrderSubmitResult submitSell(wtp::IUftStraCtx* ctx,
                                 const char* code,
                                 double price,
                                 double qty,
                                 Source src,
                                 int flag = 0);

    /// Submit a close-long order (exit long) through the router
    OrderSubmitResult submitExitLong(wtp::IUftStraCtx* ctx,
                                      const char* code,
                                      double price,
                                      double qty,
                                      bool isToday,
                                      Source src,
                                      int flag = 0);

    /// Submit a close-short order (exit short) through the router
    OrderSubmitResult submitExitShort(wtp::IUftStraCtx* ctx,
                                       const char* code,
                                       double price,
                                       double qty,
                                       bool isToday,
                                       Source src,
                                       int flag = 0);

    /// Cancel a specific order by local ID
    void cancelOrder(wtp::IUftStraCtx* ctx, uint32_t localid);

    /// Cancel all orders from a specific source
    void cancelAllBySource(wtp::IUftStraCtx* ctx, Source src);

    //==========================================================================
    // Active order query (per-source)
    //==========================================================================

    /// Number of active (not yet finalized) orders submitted with a given source.
    /// Used by executors like CloseoutExecutor to gate "previous batch settled"
    /// without polluting the MM order tracker.
    /// Counts are maintained via recordActiveOrder / onOrderDone — only orders
    /// that went through this router are counted. Direct ctx->stra_* calls are
    /// invisible here.
    uint32_t getActiveCountBySource(Source src) const
    {
        auto it = _active_orders.find(static_cast<int>(src));
        return it == _active_orders.end() ? 0
            : static_cast<uint32_t>(it->second.size());
    }

    //==========================================================================
    // Self-Trade Prevention
    //==========================================================================

    /// Check if an order would result in a self-trade against MM orders
    /// @param code   Standard contract code
    /// @param is_buy Proposed order direction
    /// @param price  Proposed order price
    /// @return true if self-trade detected (order should be blocked)
    bool checkSelfTrade(const char* code, bool is_buy, double price) const;

    //==========================================================================
    // Order lifecycle
    //==========================================================================

    /// Called when an order is filled or cancelled — removes from active tracking
    void onOrderDone(uint32_t localid);

    //==========================================================================
    // Accessors
    //==========================================================================

    /// Get all active orders for a given source
    const std::vector<ActiveOrderInfo>& getActiveOrders(Source src) const;

    /// Get rate counter for a source (mutable for direct manipulation)
    RateCounter& getRateCounter(Source src);

    /// Get total active order count across all sources
    size_t totalActiveOrders() const;

private:
    /// Internal: check rate limit and record
    __attribute__((always_inline))
    inline bool checkRateLimit(Source src, uint64_t now_ms)
    {
        auto key = static_cast<int>(src);
        auto it = _rate_counters.find(key);
        if (it == _rate_counters.end()) return true;
        if (!it->second.check(now_ms)) return false;
        it->second.increment();
        return true;
    }

    /// Internal: record active order
    void recordActiveOrder(uint32_t localid, const char* code, bool is_buy,
                           double price, double qty, Source src, uint64_t now_ms);

    /// Per-source rate counters (pre-allocated for 3 sources)
    wtp::wt_hashmap<int, RateCounter> _rate_counters;

    /// Per-source active order lists (pre-allocated vectors)
    wtp::wt_hashmap<int, std::vector<ActiveOrderInfo>> _active_orders;

    /// Global localid → source mapping for fast lookup on order done
    wtp::wt_hashmap<uint32_t, Source> _order_source_map;

    /// MM order tracker reference (for self-trade prevention)
    UnifiedOrderTracker* _mm_tracker = nullptr;
};

} // namespace futu
