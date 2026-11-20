#include "RiskFilterChain.h"
#include "WTSLogger.h"

#include <cstdlib>
#include <cmath>
#include <cstdio>

namespace wt_option {

void RiskFilterChain::add(std::unique_ptr<IRiskFilter> f) {
    m_filters.push_back(std::move(f));
}

FilterResult RiskFilterChain::execute(FilterContext& ctx) {
    for (auto& f : m_filters) {
        FilterResult r = f->process(ctx);
        if (r == FilterResult::REJECTED) {
            WTSLogger::log_by_cat("strategy", LL_WARN,
                "RiskFilter {} REJECTED {} {}: {}",
                f->name(), ctx.code, ctx.qty, ctx.rejectReason);
            return FilterResult::REJECTED;
        }
        if (r == FilterResult::MODIFIED && ctx.modifiedQty > 0 && ctx.modifiedQty < ctx.qty) {
            ctx.qty = ctx.modifiedQty;
        }
    }
    return FilterResult::APPROVED;
}

// === MaxOrderSizeFilter ===
FilterResult MaxOrderSizeFilter::process(FilterContext& ctx) {
    if (ctx.qty <= m_maxSize) return FilterResult::APPROVED;
    if (m_bReject) {
        ctx.rejectReason = "order size " + std::to_string(ctx.qty)
            + " > max " + std::to_string(m_maxSize);
        return FilterResult::REJECTED;
    }
    ctx.modifiedQty = m_maxSize;
    return FilterResult::MODIFIED;
}

// === MinSellPriceFilter ===
FilterResult MinSellPriceFilter::process(FilterContext& ctx) {
    if (!ctx.isBuy && ctx.price < m_minPrice) {
        ctx.rejectReason = "sell price " + std::to_string(ctx.price)
            + " < min " + std::to_string(m_minPrice);
        return FilterResult::REJECTED;
    }
    return FilterResult::APPROVED;
}

// === MaxPositionFilter ===
FilterResult MaxPositionFilter::process(FilterContext& ctx) {
    int32_t signedQty = ctx.isBuy ? static_cast<int32_t>(ctx.qty) : -static_cast<int32_t>(ctx.qty);
    int32_t finalPos = ctx.potentialPosition + signedQty;

    // B26 fix: the old same-direction test missed position FLIPS
    // (e.g. long 10 selling 100 → net short 90 bypassed the limit entirely).
    // Risk increases whenever the absolute exposure grows.
    bool positionIncreases = std::abs(finalPos) > std::abs(ctx.currentPosition);

    if (!positionIncreases) return FilterResult::APPROVED;

    if (static_cast<uint32_t>(std::abs(finalPos)) <= m_maxPos) return FilterResult::APPROVED;

    switch (m_mode) {
    case REJECT_ON_OVERFLOW:
        ctx.rejectReason = "position " + std::to_string(finalPos)
            + " > max " + std::to_string(m_maxPos);
        return FilterResult::REJECTED;
    case ALLOW_OVERFLOW:
        return FilterResult::APPROVED;
    case MODIFY_TO_MAX: {
        int32_t allowed = static_cast<int32_t>(m_maxPos) - std::abs(ctx.potentialPosition);
        if (allowed <= 0) {
            ctx.rejectReason = "no room: potential=" + std::to_string(ctx.potentialPosition)
                + " max=" + std::to_string(m_maxPos);
            return FilterResult::REJECTED;
        }
        ctx.modifiedQty = static_cast<uint32_t>(allowed);
        return FilterResult::MODIFIED;
    }
    }
    return FilterResult::APPROVED;
}

// === MaxCancelFilter ===
FilterResult MaxCancelFilter::process(FilterContext& ctx) {
    if (ctx.numCancels >= m_hardLimit) {
        ctx.rejectReason = "cancels " + std::to_string(ctx.numCancels)
            + " >= hard limit " + std::to_string(m_hardLimit);
        return FilterResult::REJECTED;
    }
    if (ctx.numCancels >= m_softLimit) {
        int32_t signedQty = ctx.isBuy ? static_cast<int32_t>(ctx.qty) : -static_cast<int32_t>(ctx.qty);
        int32_t finalPos = ctx.potentialPosition + signedQty;
        if (std::abs(finalPos) < std::abs(ctx.currentPosition)) {
            return FilterResult::APPROVED;
        }
        ctx.rejectReason = "soft cancel limit: cancels=" + std::to_string(ctx.numCancels)
            + " not risk-reducing (cur=" + std::to_string(ctx.currentPosition)
            + " final=" + std::to_string(finalPos) + ")";
        return FilterResult::REJECTED;
    }
    return FilterResult::APPROVED;
}

// === MaxNewOrdersFilter ===
FilterResult MaxNewOrdersFilter::process(FilterContext& ctx) {
    if (m_rejectLimit > 0 && ctx.numNewOrders >= m_rejectLimit) {
        ctx.rejectReason = "new orders " + std::to_string(ctx.numNewOrders)
            + " >= reject limit " + std::to_string(m_rejectLimit);
        return FilterResult::REJECTED;
    }
    // P1 batch-4.4: the hard-flat level must actually STOP the order — the
    // old code only logged and returned APPROVED, so the "hard" tier was a
    // no-op.  REJECTED routes to cancelSide in OQM (risk-reducing), and the
    // once-log makes the escalation traceable.
    if (m_hardFlatLimit > 0 && ctx.numNewOrders >= m_hardFlatLimit) {
        if (!m_hardFlatLogged) {
            m_hardFlatLogged = true;
            WTSLogger::log_by_cat("strategy", LL_ERROR,
                "MaxNewOrders HARD FLAT: {} newOrders={} — rejecting all new orders",
                ctx.code, ctx.numNewOrders);
        }
        ctx.rejectReason = "HARD FLAT newOrders=" + std::to_string(ctx.numNewOrders);
        return FilterResult::REJECTED;
    }
    return FilterResult::APPROVED;
}

// === MaxOpenOffsetFilter (P1 batch-4.4) ===
FilterResult MaxOpenOffsetFilter::process(FilterContext& ctx) {
    if (m_limit <= 0 || !m_expProvider) return FilterResult::APPROVED;

    uint32_t expiry = 0;
    if (!m_expProvider(ctx.code, expiry)) return FilterResult::APPROVED;

    const int64_t cur = ctx.currentPosition;
    const int64_t pot = ctx.potentialPosition;
    int64_t openAdd;
    if (cur == 0 || (cur > 0 && pot > 0) || (cur < 0 && pot < 0))
        openAdd = std::llabs(pot) - std::llabs(cur);   // same direction: incremental
    else
        openAdd = std::llabs(pot);                     // flip: whole new side opens
    if (openAdd <= 0) return FilterResult::APPROVED;   // pure close/reduce

    const int64_t opened = m_openedProvider ? m_openedProvider(expiry) : 0;
    if (opened + openAdd > m_limit) {
        ctx.rejectReason = "exp " + std::to_string(expiry) + " daily open "
            + std::to_string(opened) + "+" + std::to_string(openAdd)
            + " > limit " + std::to_string(m_limit);
        return FilterResult::REJECTED;
    }
    return FilterResult::APPROVED;
}

// === OptionsShortLimitFilter (B3) ===
// Selling increases net-short; buying decreases it. The provider returns the
// current NET SHORT count (positive number) for the call/put side.
FilterResult OptionsShortLimitFilter::process(FilterContext& ctx) {
    if (!m_provider) return FilterResult::APPROVED;

    bool isCall = (ctx.rightFlag == 1);
    if (ctx.rightFlag != 0 && ctx.rightFlag != 1)
        return FilterResult::APPROVED;   // futures / unknown — not applicable

    const int32_t limit = isCall ? m_maxShortCall : m_maxShortPut;
    if (limit <= 0) return FilterResult::APPROVED;   // disabled

    const int32_t cur = m_provider(isCall);
    const int32_t add = ctx.isBuy ? -static_cast<int32_t>(ctx.qty)
                                  : static_cast<int32_t>(ctx.qty);
    const int32_t next = cur + add;
    if (next <= limit) return FilterResult::APPROVED;

    const int32_t allowed = limit - cur;
    char buf[160];
    snprintf(buf, sizeof(buf), "short %s net %d + %d > limit %d",
             isCall ? "call" : "put", cur, add, limit);
    ctx.rejectReason = buf;
    if (allowed <= 0) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "OptionsShortLimit REJECT {}: {}", ctx.code, ctx.rejectReason);
        return FilterResult::REJECTED;
    }
    // Truncate to what still fits below the cap
    ctx.modifiedQty = static_cast<uint32_t>(allowed);
    WTSLogger::log_by_cat("strategy", LL_WARN,
        "OptionsShortLimit MODIFY {} {} -> qty={}", ctx.code, ctx.rejectReason, allowed);
    return FilterResult::MODIFIED;
}

} // namespace wt_option
