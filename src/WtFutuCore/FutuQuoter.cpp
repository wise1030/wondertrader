/*!
 * \file FutuQuoter.cpp
 * \brief Multi-level bilateral quoting engine implementation
 * 
 * Performance optimized: O(1) order lookup via hash map
 */
#include "FutuQuoter.h"
#include "../Includes/IUftStraCtx.h"

namespace futu {

FutuQuoter::FutuQuoter()
{
}

void FutuQuoter::init(const QuoterConfig& cfg)
{
    _cfg = cfg;
    _bid_levels.resize(cfg.num_levels);
    _ask_levels.resize(cfg.num_levels);

    for (uint32_t i = 0; i < cfg.num_levels; i++)
    {
        _bid_levels[i].is_bid = true;
        _ask_levels[i].is_bid = false;
    }

    // Pre-allocate order lookup map
    _order_to_level.reserve(cfg.num_levels * 2);
}

uint32_t FutuQuoter::refreshQuotes(wtp::IUftStraCtx* ctx, double mid, double skew,
                                double spread_mult, bool allow_bid, bool allow_ask)
{
    uint32_t orders_placed = 0;
    
    if (!ctx) return 0;

    // Process bid levels
    for (uint32_t i = 0; i < _cfg.num_levels; i++)
    {
        double newPrice = computeBidPrice(mid, skew, spread_mult, i);
        double newQty = computeQty(i);
        QuoteLevel& level = _bid_levels[i];

        if (!allow_bid)
        {
            // Cancel if active
            if (level.order_id != 0)
            {
                ctx->stra_cancel(level.order_id);
                unregisterOrder(level.order_id);
                level.order_id = 0;
            }
            continue;
        }

        // Sticky 策略：只有当价格偏离超过阈值时才撤单重报
        // 这样减少了频繁的撤单-重报循环，降低系统负载
        if (level.order_id != 0)
        {
            double priceDeviation = std::abs(level.price - newPrice) / _cfg.tick_size;
            if (priceDeviation <= _cfg.sticky_threshold)
            {
                // 价格偏离在容忍范围内，保留原订单
                continue;
            }
        }

        // Cancel old order if exists
        if (level.order_id != 0)
        {
            ctx->stra_cancel(level.order_id);
            unregisterOrder(level.order_id);
            level.order_id = 0;
        }

        // Place new bid
        wtp::OrderIDs ids = ctx->stra_buy(_cfg.code.c_str(), newPrice, newQty);
        if (!ids.empty())
        {
            level.order_id = ids[0];
            level.price = newPrice;
            level.qty = newQty;
            registerOrder(level.order_id, &level);
            orders_placed++;
        }
    }

    // Process ask levels
    for (uint32_t i = 0; i < _cfg.num_levels; i++)
    {
        double newPrice = computeAskPrice(mid, skew, spread_mult, i);
        double newQty = computeQty(i);
        QuoteLevel& level = _ask_levels[i];

        if (!allow_ask)
        {
            if (level.order_id != 0)
            {
                ctx->stra_cancel(level.order_id);
                unregisterOrder(level.order_id);
                level.order_id = 0;
            }
            continue;
        }

        // Sticky 策略：只有当价格偏离超过阈值时才撤单重报
        if (level.order_id != 0)
        {
            double priceDeviation = std::abs(level.price - newPrice) / _cfg.tick_size;
            if (priceDeviation <= _cfg.sticky_threshold)
            {
                // 价格偏离在容忍范围内，保留原订单
                continue;
            }
        }

        if (level.order_id != 0)
        {
            ctx->stra_cancel(level.order_id);
            unregisterOrder(level.order_id);
            level.order_id = 0;
        }

        wtp::OrderIDs ids = ctx->stra_sell(_cfg.code.c_str(), newPrice, newQty);
        if (!ids.empty())
        {
            level.order_id = ids[0];
            level.price = newPrice;
            level.qty = newQty;
            registerOrder(level.order_id, &level);
            orders_placed++;
        }
    }
    
    return orders_placed;
}

void FutuQuoter::cancelAll(wtp::IUftStraCtx* ctx)
{
    if (!ctx) return;

    for (auto& level : _bid_levels)
    {
        if (level.order_id != 0)
        {
            ctx->stra_cancel(level.order_id);
            unregisterOrder(level.order_id);
            level.order_id = 0;
        }
    }

    for (auto& level : _ask_levels)
    {
        if (level.order_id != 0)
        {
            ctx->stra_cancel(level.order_id);
            unregisterOrder(level.order_id);
            level.order_id = 0;
        }
    }
}

void FutuQuoter::onOrder(uint32_t localid, bool isCanceled, double leftQty)
{
    // O(1) lookup via hash map
    QuoteLevel* level = getLevelByOrder(localid);
    if (level == nullptr)
        return;

    if (isCanceled || leftQty == 0)
    {
        level->order_id = 0;
        unregisterOrder(localid);
    }
}

void FutuQuoter::onTrade(uint32_t localid, double vol, double price)
{
    // O(1) lookup via hash map
    QuoteLevel* level = getLevelByOrder(localid);
    if (level == nullptr)
        return;

    // Trade just clears the level if fully filled (handled by onOrder with leftQty=0)
    // Here we can update quantity if partial fill tracking is needed
}

double FutuQuoter::totalBidQty() const
{
    double total = 0;
    for (const auto& level : _bid_levels)
        if (level.order_id != 0) total += level.qty;
    return total;
}

double FutuQuoter::totalAskQty() const
{
    double total = 0;
    for (const auto& level : _ask_levels)
        if (level.order_id != 0) total += level.qty;
    return total;
}

} // namespace futu