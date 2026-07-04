/*!
 * \file OptionQuoteManager.cpp
 * \brief Per-contract order lifecycle manager implementation
 */
#include "OptionQuoteManager.h"
#include "../Includes/IUftStraCtx.h"
#include "../WTSTools/WTSLogger.h"

#include <algorithm>
#include <cmath>

namespace wt_option {

OptionQuoteManager::OptionQuoteManager(const std::string& code, const Config& cfg,
                                       wtp::IUftStraCtx* ctx)
    : m_code(code), m_cfg(cfg), m_ctx(ctx)
{
}

// ============================================================================
// updateOrders — core: desired → cancel old → send new (with STP)
// ============================================================================

int32_t OptionQuoteManager::updateOrders(const MultiMarket& desired, bool cancel_only)
{
    if (!m_active && !cancel_only) return 0;

    if (cancel_only) {
        cancelSide(true);   // cancel all buys
        cancelSide(false);  // cancel all sells
        return m_numCancel;
    }

    // Check if desired == current → skip (avoid_trade)
    // Manual comparison (MultiMarket has no operator==)
    if (m_cfg.avoid_trade) {
        const PriceSize& curBid = m_orderMarketTracker.getBestBid();
        const PriceSize& curAsk = m_orderMarketTracker.getBestAsk();
        const PriceSize& newBid = desired.getBestBid();
        const PriceSize& newAsk = desired.getBestAsk();
        bool same = true;
        if (!curBid.empty() && !newBid.empty()) {
            same = same && (curBid.px() == newBid.px() && curBid.sz() == newBid.sz());
        } else {
            same = same && (curBid.empty() == newBid.empty());
        }
        if (!curAsk.empty() && !newAsk.empty()) {
            same = same && (curAsk.px() == newAsk.px() && curAsk.sz() == newAsk.sz());
        } else {
            same = same && (curAsk.empty() == newAsk.empty());
        }
        if (same) return 0;
    }

    // Extract desired best bid/ask
    const PriceSize& bid = desired.getBestBid();
    const PriceSize& ask = desired.getBestAsk();

    // STP: prevent self-crossing
    if (!bid.empty() && !ask.empty() && is_crossed(bid.px(), ask.px())) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "OQM STP: {} bid {} >= ask {}, skip", m_code, bid.px(), ask.px());
        return 0;
    }

    // Position check
    int32_t potentialPos = m_position;
    if (!bid.empty()) potentialPos += static_cast<int32_t>(bid.sz());
    if (!ask.empty()) potentialPos -= static_cast<int32_t>(ask.sz());

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

    // Cancel limit check
    if (m_cfg.max_cancels_allowed > 0 && m_numCancel >= m_cfg.max_cancels_allowed) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "OQM: {} cancel limit reached ({})", m_code, m_numCancel);
        // Don't cancel, just update if possible
    } else {
        // Cancel old orders before sending new
        if (m_cfg.enable_quote_api && m_bidOrders.empty() && m_askOrders.empty()) {
            // SHFE/CZCE/INE: stra_quote replaces previous automatically
        } else {
            cancelSide(true);
            cancelSide(false);
        }
    }

    // Send new orders
    int32_t txns = 0;
    if (!bid.empty() && !ask.empty() && bid.sz() > 0 && ask.sz() > 0) {
        // Two-legged quote
        auto ids = sendQuote(bid.px(), static_cast<uint32_t>(bid.sz()),
                             ask.px(), static_cast<uint32_t>(ask.sz()));
        if (ids.first != 0 || ids.second != 0) {
            double now = m_getTime ? m_getTime() : 0;
            if (ids.first != 0) {
                m_bidOrders.push_back({ids.first, true, bid.px(),
                    static_cast<uint32_t>(bid.sz()), 0, true, false, now});
                m_numNewOrders++;
            }
            if (ids.second != 0) {
                m_askOrders.push_back({ids.second, false, ask.px(),
                    static_cast<uint32_t>(ask.sz()), 0, true, false, now});
                m_numNewOrders++;
            }
            txns++;
        }
    } else if (!bid.empty() && bid.sz() > 0) {
        // Buy only (limit order or single-side quote)
        auto ids = sendQuote(bid.px(), static_cast<uint32_t>(bid.sz()), 0, 0);
        if (ids.first != 0) {
            double now = m_getTime ? m_getTime() : 0;
            m_bidOrders.push_back({ids.first, true, bid.px(),
                static_cast<uint32_t>(bid.sz()), 0, true, false, now});
            m_numNewOrders++;
            txns++;
        }
    } else if (!ask.empty() && ask.sz() > 0) {
        // Sell only
        auto ids = sendQuote(0, 0, ask.px(), static_cast<uint32_t>(ask.sz()));
        if (ids.second != 0) {
            double now = m_getTime ? m_getTime() : 0;
            m_askOrders.push_back({ids.second, false, ask.px(),
                static_cast<uint32_t>(ask.sz()), 0, true, false, now});
            m_numNewOrders++;
            txns++;
        }
    } else {
        // Both empty — cancel everything
        cancelSide(true);
        cancelSide(false);
    }

    // Check TTL — cancel expired orders
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
    return txns;
}

// ============================================================================
// onOrderStatusChange — from strategy on_order callback
// ============================================================================

void OptionQuoteManager::onOrderStatusChange(uint32_t localid, bool isLong,
    double totalQty, double leftQty, double price, bool isCanceled)
{
    auto& orders = isLong ? m_bidOrders : m_askOrders;
    bool found = false;
    for (auto& o : orders) {
        if (o.localid == localid) {
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
        // Remove inactive orders
        orders.erase(std::remove_if(orders.begin(), orders.end(),
            [](const OrderState& o) { return !o.active; }),
            orders.end());
        rebuildOrderMarketTracker();
    }
}

// ============================================================================
// onFill — from strategy on_trade callback
// ============================================================================

void OptionQuoteManager::onFill(uint32_t localid, bool isLong,
    double fill_px, uint32_t fill_qty)
{
    auto& orders = isLong ? m_bidOrders : m_askOrders;
    for (auto& o : orders) {
        if (o.localid == localid) {
            o.filled += fill_qty;
            break;
        }
    }

    // Update position
    m_position += (isLong ? 1 : -1) * static_cast<int32_t>(fill_qty);
    m_numFill++;
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

bool OptionQuoteManager::sendCancelById(uint32_t localid)
{
    if (!m_ctx) return false;
    return m_ctx->stra_cancel(localid);
}

void OptionQuoteManager::sendCancelAll()
{
    if (!m_ctx) return;
    m_ctx->stra_cancel_all(m_code.c_str());
    m_bidOrders.clear();
    m_askOrders.clear();
    m_orderMarketTracker.clear();
}

void OptionQuoteManager::cancelSide(bool isBuy)
{
    auto& orders = isBuy ? m_bidOrders : m_askOrders;
    for (auto& o : orders) {
        if (o.active && !o.cancelPending) {
            sendCancelById(o.localid);
            o.cancelPending = true;
        }
    }
}

void OptionQuoteManager::rebuildOrderMarketTracker()
{
    m_orderMarketTracker.clear();

    // Rebuild from active bid orders
    for (const auto& o : m_bidOrders) {
        if (o.active && o.filled < o.qty) {
            uint32_t remaining = o.qty - o.filled;
            m_orderMarketTracker.setBest(0, PriceSize(o.price, remaining));
            break;  // best bid only
        }
    }

    // Rebuild from active ask orders
    for (const auto& o : m_askOrders) {
        if (o.active && o.filled < o.qty) {
            uint32_t remaining = o.qty - o.filled;
            m_orderMarketTracker.setBest(1, PriceSize(o.price, remaining));
            break;  // best ask only
        }
    }
}

bool OptionQuoteManager::is_crossed(double bidP, double askP) const
{
    return bidP >= askP;
}

} // namespace wt_option
