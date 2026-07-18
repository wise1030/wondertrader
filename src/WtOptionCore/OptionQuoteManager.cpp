/*!
 * \file OptionQuoteManager.cpp
 * \brief Per-contract order lifecycle manager implementation
 *
 * Enhanced with incremental diffing, cancel throttle, cautious flipping,
 * scale factor, wait-for-cancels, PositionGuard/PositionOffsetMgr integration.
 */
#include "OptionQuoteManager.h"
#include "PositionOffsetMgr.h"
#include "PositionGuard.h"
#include "../Includes/IHftStraCtx.h"
#include "../WTSTools/WTSLogger.h"

#include <algorithm>
#include <cmath>

namespace wt_option {

OptionQuoteManager::OptionQuoteManager(const std::string& code, const Config& cfg,
                                       wtp::IHftStraCtx* ctx)
    : m_code(code), m_cfg(cfg), m_ctx(ctx)
{
    m_runtimeScale = m_cfg.scale_factor;
}

// ============================================================================
// updateOrders - core: incremental desired -> cancel old -> send new
// ============================================================================

int32_t OptionQuoteManager::updateOrders(const MultiMarket& desired, bool cancel_only)
{
    if (m_getTime) m_lastUpdateCycleTime = m_getTime();

    // Enhancement: Min intra-update period - rate limit updateOrders calls
    if (!cancel_only && m_cfg.min_intra_update_period_ms > 0 && m_getTime) {
        double now = m_getTime();
        double elapsedMs = (now - m_lastUpdateTime) * 1000.0;
        if (m_lastUpdateTime > 0 && elapsedMs < m_cfg.min_intra_update_period_ms) {
            return 0;
        }
        m_lastUpdateTime = now;
    }

    // Enhancement: PositionGuard - check before trading
    if (m_positionGuard && !m_positionGuard->isOK()) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "OQM: {} PositionGuard DISABLED, skipping updateOrders", m_code);
        return 0;
    }

    if (!m_active && !cancel_only) return 0;

    if (cancel_only) {
        cancelSide(true);
        cancelSide(false);
        return m_numCancel;
    }

    // Enhancement: Avoid-trade with per-side comparison (incremental)
    const PriceSize& curBid = m_orderMarketTracker.getBestBid();
    const PriceSize& curAsk = m_orderMarketTracker.getBestAsk();
    const PriceSize& newBid = desired.getBestBid();
    const PriceSize& newAsk = desired.getBestAsk();

    bool bidSame = (curBid.empty() == newBid.empty()) &&
                   (curBid.empty() || (curBid.px() == newBid.px() && curBid.sz() == newBid.sz()));
    bool askSame = (curAsk.empty() == newAsk.empty()) &&
                   (curAsk.empty() || (curAsk.px() == newAsk.px() && curAsk.sz() == newAsk.sz()));

    if (m_cfg.avoid_trade && bidSame && askSame) return 0;

    // STP: prevent self-crossing
    if (!newBid.empty() && !newAsk.empty() && is_crossed(newBid.px(), newAsk.px())) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "OQM STP: {} bid {} >= ask {}, skip", m_code, newBid.px(), newAsk.px());
        return 0;
    }

    // Enhancement: Apply scale factor to quantities
    uint32_t bidQty = newBid.empty() ? 0 :
        static_cast<uint32_t>(std::max(1.0, newBid.sz() * m_runtimeScale));
    uint32_t askQty = newAsk.empty() ? 0 :
        static_cast<uint32_t>(std::max(1.0, newAsk.sz() * m_runtimeScale));

    // Enhancement: Cautious flipping - block orders that would flip position
    bool blockBid = false, blockAsk = false;
    if (m_cfg.cautious_flipping) {
        if (m_position < 0 && bidQty > 0) {
            int32_t after = m_position + static_cast<int32_t>(bidQty);
            if (after > 0) blockBid = true;  // would flip from short to long
        }
        if (m_position > 0 && askQty > 0) {
            int32_t after = m_position - static_cast<int32_t>(askQty);
            if (after < 0) blockAsk = true;  // would flip from long to short
        }
    }

    // Potential position check
    int32_t potentialPos = m_position;
    if (!newBid.empty() && !blockBid) potentialPos += static_cast<int32_t>(bidQty);
    if (!newAsk.empty() && !blockAsk) potentialPos -= static_cast<int32_t>(askQty);

    if (m_cfg.check_potential_position && std::abs(potentialPos) > static_cast<int32_t>(m_cfg.max_position)) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "OQM: {} potential position {} > max {}", m_code, potentialPos, m_cfg.max_position);
        cancelSide(true);
        cancelSide(false);
        return 0;
    }

    // Hard flat after N fills
    if (m_cfg.hard_flat_after_n_fills > 0 && m_numFill >= m_cfg.hard_flat_after_n_fills) {
        if (!m_hardFlatMode) {
            m_hardFlatMode = true;
            WTSLogger::log_by_cat("strategy", LL_WARN,
                "OQM: {} HARD FLAT triggered (fills={})", m_code, m_numFill);
        }
    }

    // Reject after N new orders
    if (m_cfg.reject_max_new_orders >= 0 && m_numNewOrders >= m_cfg.reject_max_new_orders) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "OQM: {} order rejected (newOrders={})", m_code, m_numNewOrders);
        return 0;
    }

    // Enhancement: Cancel throttle - soft warn
    if (isCancelSoftWarn()) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "OQM: {} cancel soft warn (cancels={})", m_code, m_numCancel);
    }

    // Enhancement: Wait-for-cancels - don't send new orders if cancels pending
    bool hasPendingCancels = false;
    for (const auto& o : m_bidOrders) if (o.cancelPending) { hasPendingCancels = true; break; }
    if (!hasPendingCancels)
        for (const auto& o : m_askOrders) if (o.cancelPending) { hasPendingCancels = true; break; }

    // Enhancement: Risk filter chain
    if (m_filterChain && !hasPendingCancels) {
        if (!newBid.empty() && !blockBid) {
            FilterContext fctx;
            fctx.code = m_code;
            fctx.isBuy = true;
            fctx.price = newBid.px();
            fctx.qty = bidQty;
            fctx.currentPosition = m_position;
            fctx.potentialPosition = m_position;
            fctx.numCancels = m_numCancel;
            fctx.numNewOrders = m_numNewOrders;
            fctx.numFills = m_numFill;
            if (m_filterChain->execute(fctx) == FilterResult::REJECTED) {
                blockBid = true;
            }
        }
        if (!newAsk.empty() && !blockAsk) {
            FilterContext fctx;
            fctx.code = m_code;
            fctx.isBuy = false;
            fctx.price = newAsk.px();
            fctx.qty = askQty;
            fctx.currentPosition = m_position;
            fctx.potentialPosition = m_position;
            fctx.numCancels = m_numCancel;
            fctx.numNewOrders = m_numNewOrders;
            fctx.numFills = m_numFill;
            if (m_filterChain->execute(fctx) == FilterResult::REJECTED) {
                blockAsk = true;
            }
        }
    }

    // === Incremental per-side update ===
    // Instead of cancel-all-then-resend, only cancel/send what changed.

    // Bid side
    if (blockBid) {
        cancelSide(true);
    } else if (!bidSame) {
        PriceSize scaledBid(newBid.px(), static_cast<int32_t>(bidQty));
        updateSide(true, scaledBid);
    }

    // Ask side
    if (blockAsk) {
        cancelSide(false);
    } else if (!askSame) {
        PriceSize scaledAsk(newAsk.px(), static_cast<int32_t>(askQty));
        updateSide(false, scaledAsk);
    }

    // TTL - cancel expired orders
    if (m_cfg.time_in_force_ms > 0 && m_getTime) {
        double now = m_getTime();
        double ttl = m_cfg.time_in_force_ms / 1000.0;
        for (auto& o : m_bidOrders) {
            if (o.active && !o.cancelPending && (now - o.issueTime) > ttl) {
                sendCancelById(o.localid);
                o.cancelPending = true;
            }
        }
        for (auto& o : m_askOrders) {
            if (o.active && !o.cancelPending && (now - o.issueTime) > ttl) {
                sendCancelById(o.localid);
                o.cancelPending = true;
            }
        }
    }

    m_lastDesired = desired;
    return 1;
}

// ============================================================================
// updateSide - incremental per-side update (diff desired vs current)
// ============================================================================

void OptionQuoteManager::updateSide(bool isBuy, const PriceSize& desired)
{
    auto& orders = isBuy ? m_bidOrders : m_askOrders;

    // If desired is empty: cancel all on this side
    if (desired.empty()) {
        if (canCancel()) cancelSide(isBuy);
        return;
    }

    // Check if there's already an active order at the desired price
    int32_t missing = getMissingPriceLevelSize(desired.px(), static_cast<uint32_t>(desired.sz()), isBuy);

    if (missing == 0) {
        // Already have the right size at this price -> nothing to do
        return;
    }

    if (missing > 0) {
        // Need to add size - but with single-level quote API, we cancel and resend
        // (exchange quote API replaces the previous quote)

        // Wait-for-cancels: don't send new if cancels still pending
        bool hasPending = false;
        for (const auto& o : orders) {
            if (o.cancelPending) { hasPending = true; break; }
        }

        if (m_cfg.wait_for_cancels && hasPending) {
            // Skip for now, will retry next cycle
            return;
        }

        // Standard path: cancel old + send new quote
        if (canCancel()) {
            // Cancel orders at different prices
            for (auto& o : orders) {
                if (o.active && !o.cancelPending &&
                    std::abs(o.price - desired.px()) > 1e-6) {
                    sendCancelById(o.localid);
                    o.cancelPending = true;
                }
            }

            // If quote API replaces automatically, no need to cancel same-price orders
            if (!m_cfg.enable_quote_api) {
                // Non-quote API: need to cancel same-price orders too for size change
                for (auto& o : orders) {
                    if (o.active && !o.cancelPending) {
                        sendCancelById(o.localid);
                        o.cancelPending = true;
                    }
                }
            }
        }

        // Send new quote/order
        if (isBuy) {
            auto ids = sendQuote(desired.px(), static_cast<uint32_t>(desired.sz()), 0, 0);
            if (ids.first != 0) {
                double now = m_getTime ? m_getTime() : 0;
                m_bidOrders.push_back({ids.first, true, desired.px(),
                    static_cast<uint32_t>(desired.sz()), 0, true, false, now});
                m_numNewOrders++;
            }
        } else {
            auto ids = sendQuote(0, 0, desired.px(), static_cast<uint32_t>(desired.sz()));
            if (ids.second != 0) {
                double now = m_getTime ? m_getTime() : 0;
                m_askOrders.push_back({ids.second, false, desired.px(),
                    static_cast<uint32_t>(desired.sz()), 0, true, false, now});
                m_numNewOrders++;
            }
        }
    } else {
        // missing < 0: too much size at this price -> need to cancel some
        // For single-level quote API: cancel and resend with smaller size
        if (canCancel() && !m_cfg.enable_quote_api) {
            for (auto& o : orders) {
                if (o.active && !o.cancelPending &&
                    std::abs(o.price - desired.px()) < 1e-6) {
                    sendCancelById(o.localid);
                    o.cancelPending = true;
                }
            }
            // Resend with correct size
            if (isBuy) {
                auto ids = sendQuote(desired.px(), static_cast<uint32_t>(desired.sz()), 0, 0);
                if (ids.first != 0) {
                    double now = m_getTime ? m_getTime() : 0;
                    m_bidOrders.push_back({ids.first, true, desired.px(),
                        static_cast<uint32_t>(desired.sz()), 0, true, false, now});
                    m_numNewOrders++;
                }
            } else {
                auto ids = sendQuote(0, 0, desired.px(), static_cast<uint32_t>(desired.sz()));
                if (ids.second != 0) {
                    double now = m_getTime ? m_getTime() : 0;
                    m_askOrders.push_back({ids.second, false, desired.px(),
                        static_cast<uint32_t>(desired.sz()), 0, true, false, now});
                    m_numNewOrders++;
                }
            }
        }
        // With quote API: just resend with new size (auto-replaces)
        if (m_cfg.enable_quote_api) {
            if (isBuy) {
                sendQuote(desired.px(), static_cast<uint32_t>(desired.sz()), 0, 0);
            } else {
                sendQuote(0, 0, desired.px(), static_cast<uint32_t>(desired.sz()));
            }
        }
    }
}

// ============================================================================
// getMissingPriceLevelSize - incremental diff: desired - current at price
// ============================================================================

int32_t OptionQuoteManager::getMissingPriceLevelSize(
    double price, uint32_t desiredSize, bool isBuy) const
{
    const auto& orders = isBuy ? m_bidOrders : m_askOrders;
    uint32_t currentSize = 0;
    for (const auto& o : orders) {
        if (o.active && !o.cancelPending &&
            std::abs(o.price - price) < 1e-6) {
            currentSize += (o.qty - o.filled);
        }
    }
    return static_cast<int32_t>(desiredSize) - static_cast<int32_t>(currentSize);
}

// ============================================================================
// onOrderStatusChange - from strategy on_order callback
// ============================================================================

void OptionQuoteManager::onOrderStatusChange(uint32_t localid, bool isLong,
    double totalQty, double leftQty, double price, bool isCanceled)
{
    auto& orders = isLong ? m_bidOrders : m_askOrders;
    bool found = false;
    bool wasNewOrder = false;

    for (auto& o : orders) {
        if (o.localid == localid) {
            // Detect: this is first status change for a newly sent order (qty was 0)
            if (o.qty == 0 && totalQty > 0) wasNewOrder = true;

            if (isCanceled || leftQty == 0) {
                o.active = false;
                o.cancelPending = false;
                if (isCanceled) m_numCancel++;
            }
            o.qty = static_cast<uint32_t>(totalQty);
            o.filled = static_cast<uint32_t>(totalQty - leftQty);
            found = true;
            break;
        }
    }

    if (found) {
        orders.erase(std::remove_if(orders.begin(), orders.end(),
            [](const OrderState& o) { return !o.active; }),
            orders.end());
        rebuildOrderMarketTracker();

        // QuoteStatistics: all stats driven by callbacks (no hot-path overhead)
        if (m_quoteStats) {
            // New order confirmed (first ack from exchange)
            if (wasNewOrder) {
                m_quoteStats->onOrderSent(m_code);

                // Quote latency: tick -> order confirmed
                if (m_tickTimestampUs > 0) {
                    uint64_t now = m_getTime ? static_cast<uint64_t>(m_getTime() * 1000000) : 0;
                    if (now > m_tickTimestampUs) {
                        m_quoteStats->onQuoteLatency(m_code, now - m_tickTimestampUs);
                    }
                }
            }

            // Cancel confirmed
            if (isCanceled) {
                m_quoteStats->onCancel(m_code);
            }

            // Reject: canceled with no fill at all
            if (isCanceled && leftQty == totalQty) {
                m_quoteStats->onReject(m_code);
            }
        }

        // Enhancement: Reject retry - detect full rejection (canceled with no fill)
        if (isCanceled && leftQty == totalQty && m_rejectRetryCount < m_rejectMaxRetries) {
            m_rejectRetryCount++;
            m_lastRejectTime = m_getTime ? m_getTime() : 0;
            m_retryPending = true;
            WTSLogger::log_by_cat("strategy", LL_WARN,
                "OQM reject retry: {} attempt {}/{} pending",
                m_code, m_rejectRetryCount, m_rejectMaxRetries);
        }

        // Push actual posted market to QuoteStatistics
        pushQuoteStats();
    }
}

// ============================================================================
// onFill - from strategy on_trade callback
// ============================================================================

void OptionQuoteManager::onFill(uint32_t localid, bool isLong,
    double fill_px, uint32_t fill_qty)
{
    bool lateFill = false;
    if (m_getTime && m_lastUpdateCycleTime > 0) {
        double now = m_getTime();
        if ((now - m_lastUpdateCycleTime) > LATE_FILL_THRESHOLD) {
            lateFill = true;
            m_numLateFills++;
            WTSLogger::log_by_cat("strategy", LL_WARN,
                "OQM late fill: {} {}@{} ({}s after update cycle #{})",
                m_code, fill_qty, fill_px,
                now - m_lastUpdateCycleTime, m_numLateFills);
        }
    }

    auto& orders = isLong ? m_bidOrders : m_askOrders;
    for (auto& o : orders) {
        if (o.localid == localid) {
            o.filled += fill_qty;
            break;
        }
    }

    m_position += (isLong ? 1 : -1) * static_cast<int32_t>(fill_qty);
    m_numFill++;

    // QuoteStatistics: only record fill count here.
    // Posted market snapshot (pushQuoteStats) is handled by the subsequent
    // onOrderStatusChange callback (WT calls on_trade then on_order with
    // updated leftQty), so we don't duplicate the work here.
    if (m_quoteStats) m_quoteStats->onFill(m_code);

    // Enhancement: Reset reject retry counter on successful fill
    m_rejectRetryCount = 0;
    m_retryPending = false;
}

// ============================================================================
// Cancel throttle
// ============================================================================

int32_t OptionQuoteManager::getTrueMaxCancels() const {
    if (m_cfg.max_cancels_allowed <= 0) return 0;  // unlimited
    int32_t trueMax = m_cfg.max_cancels_allowed;
    if (m_cfg.cancel_buffer > 0 && m_cfg.cancel_buffer < trueMax) {
        trueMax -= m_cfg.cancel_buffer;
    }
    return trueMax;
}

bool OptionQuoteManager::canCancel() const {
    if (m_cfg.max_cancels_allowed <= 0) return true;  // unlimited
    int32_t trueMax = getTrueMaxCancels();
    return m_numCancel < trueMax;
}

bool OptionQuoteManager::isCancelSoftWarn() const {
    return m_cfg.cancel_soft_max > 0 && m_numCancel >= m_cfg.cancel_soft_max;
}

// ============================================================================
// PositionGuard
// ============================================================================

bool OptionQuoteManager::isGuardOK() const {
    if (!m_positionGuard) return true;
    return m_positionGuard->isOK();
}

// ============================================================================
// Scale factor
// ============================================================================

void OptionQuoteManager::setScaleFactor(double scale) {
    m_runtimeScale = std::max(0.0, std::min(1.0, scale));
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "OQM: {} scale factor set to {:.2f}", m_code, m_runtimeScale);
}

// ============================================================================
// Reject retry
// ============================================================================

double OptionQuoteManager::getRetryDelayRemaining() const {
    if (!m_retryPending || m_lastRejectTime <= 0 || !m_getTime) return 0;
    double elapsed = m_getTime() - m_lastRejectTime;
    double delay = m_rejectRetryDelayMs / 1000.0;
    if (elapsed >= delay) return 0;
    return delay - elapsed;
}

// ============================================================================
// Private helpers
// ============================================================================

std::pair<uint32_t, uint32_t> OptionQuoteManager::sendQuote(
    double bidP, uint32_t bidQ, double askP, uint32_t askQ)
{
    if (!m_ctx) return {0, 0};
    return m_ctx->stra_quote(m_code.c_str(), bidP, bidQ, askP, askQ, "OptionMM");
}

bool OptionQuoteManager::sendCancelById(uint32_t localid) {
    if (!m_ctx) return false;
    return m_ctx->stra_cancel(localid);
}

void OptionQuoteManager::sendCancelAll() {
    if (!m_ctx) return;
    m_ctx->stra_cancel_all(m_code.c_str());
    m_bidOrders.clear();
    m_askOrders.clear();
    m_orderMarketTracker.clear();
}

void OptionQuoteManager::cancelSide(bool isBuy) {
    auto& orders = isBuy ? m_bidOrders : m_askOrders;
    for (auto& o : orders) {
        if (o.active && !o.cancelPending) {
            if (canCancel()) {
                sendCancelById(o.localid);
                o.cancelPending = true;
            }
        }
    }
}

void OptionQuoteManager::cancelByPrice(bool isBuy, double price) {
    auto& orders = isBuy ? m_bidOrders : m_askOrders;
    for (auto& o : orders) {
        if (o.active && !o.cancelPending &&
            std::abs(o.price - price) < 1e-6) {
            if (canCancel()) {
                sendCancelById(o.localid);
                o.cancelPending = true;
            }
        }
    }
}

void OptionQuoteManager::rebuildOrderMarketTracker() {
    m_orderMarketTracker.clear();

    // Best bid = highest price among active buy orders
    double bestBidPx = -1;
    uint32_t bestBidRem = 0;
    for (const auto& o : m_bidOrders) {
        if (o.active && o.filled < o.qty) {
            uint32_t rem = o.qty - o.filled;
            if (o.price > bestBidPx) {
                bestBidPx = o.price;
                bestBidRem = rem;
            }
        }
    }
    if (bestBidPx > 0) {
        m_orderMarketTracker.setBest(0, PriceSize(bestBidPx, static_cast<int32_t>(bestBidRem)));
    }

    // Best ask = lowest price among active sell orders
    double bestAskPx = 1e18;
    uint32_t bestAskRem = 0;
    for (const auto& o : m_askOrders) {
        if (o.active && o.filled < o.qty) {
            uint32_t rem = o.qty - o.filled;
            if (o.price < bestAskPx) {
                bestAskPx = o.price;
                bestAskRem = rem;
            }
        }
    }
    if (bestAskPx < 1e18) {
        m_orderMarketTracker.setBest(1, PriceSize(bestAskPx, static_cast<int32_t>(bestAskRem)));
    }
}

bool OptionQuoteManager::is_crossed(double bidP, double askP) const {
    return bidP >= askP;
}

// ============================================================================
// pushQuoteStats - push actual posted market to QuoteStatistics
// Called from onOrderStatusChange/onFill callbacks (worker thread, not hot path)
// ============================================================================

void OptionQuoteManager::pushQuoteStats() {
    if (!m_quoteStats) return;

    // Extract actual posted market from order tracker
    const PriceSize& bid = m_orderMarketTracker.getBestBid();
    const PriceSize& ask = m_orderMarketTracker.getBestAsk();

    double bidPrice = bid.empty() ? 0 : bid.px();
    double bidQty   = bid.empty() ? 0 : bid.sz();
    double askPrice = ask.empty() ? 0 : ask.px();
    double askQty   = ask.empty() ? 0 : ask.sz();

    // Check if our quote is at market best
    // (simplified: assume best order is at best if it exists)
    bool isAtBest = (!bid.empty() && !ask.empty());

    m_quoteStats->onPostedMarket(m_code, bidPrice, bidQty,
                                  askPrice, askQty,
                                  m_cfg.tick_size,
                                  m_timeHHMM, m_secInMin,
                                  isAtBest);
}

} // namespace wt_option
