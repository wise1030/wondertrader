/*!
 * \file FutuQuoter.cpp
 * \brief Multi-level bilateral quoting engine implementation
 */
#include "FutuQuoter.h"
#include "UnifiedOrderTracker.h"
#include "BilateralQuoteStats.h"
#include "../Includes/IUftStraCtx.h"
#include "../WTSTools/WTSLogger.h"
#include <cstring>
#include <utility>  // for std::pair and structured bindings

namespace futu {

FutuQuoter::FutuQuoter()
    : _tracker(nullptr)
{
}

void FutuQuoter::init(const QuoterConfig& cfg)
{
    _cfg = cfg;
    _bid_levels.resize(cfg.num_levels);
    _ask_levels.resize(cfg.num_levels);
    _level_qtys.resize(cfg.num_levels);

    for (uint32_t i = 0; i < cfg.num_levels; i++)
    {
        _bid_levels[i].is_bid = true;
        _bid_levels[i].level_index = static_cast<uint8_t>(i);
        _ask_levels[i].is_bid = false;
        _ask_levels[i].level_index = static_cast<uint8_t>(i);
        
        double qty = cfg.base_qty * std::pow(cfg.qty_decay, i);
        _level_qtys[i] = std::max(1.0, std::round(qty));
    }
}

FutuQuoter::QuoteResult FutuQuoter::computeQuotePrices(
    uint32_t level, double mid,
    double l0_bid_price, double l0_ask_price,
    double spread_mult, bool allow_bid, bool allow_ask,
    double upper_limit, double lower_limit,
    double best_bid, double best_ask,
    double long_util, double short_util,
    bool force_ask_obligation, bool force_bid_obligation,
    bool is_obligation_mode)
{
    QuoteResult qr{};
    qr.is_obligation_bid = false;
    qr.is_obligation_ask = false;

    double level_offset = level * _cfg.level_step * _cfg.tick_size;
    qr.bidPrice = floor((l0_bid_price - level_offset) / _cfg.tick_size) * _cfg.tick_size;
    qr.askPrice = ceil((l0_ask_price + level_offset) / _cfg.tick_size) * _cfg.tick_size;
    qr.bidQty = computeQty(level);
    qr.askQty = computeQty(level);

    const bool apply_obligation = (!_cfg.obligation_only_l0 || level == 0);

    if (is_obligation_mode)
    {
        // 义务模式: 不衰减 qty，价格偏移到义务区间
        if (force_ask_obligation && apply_obligation) {
            qr.askPrice = ceil((mid + _cfg.obligation_max_spread_ticks * _cfg.tick_size) / _cfg.tick_size) * _cfg.tick_size;
            qr.askQty = std::max(computeQty(0), _cfg.obligation_min_qty);
            qr.is_obligation_ask = true;
        }
        if (force_bid_obligation && apply_obligation) {
            qr.bidPrice = floor((mid - _cfg.obligation_max_spread_ticks * _cfg.tick_size) / _cfg.tick_size) * _cfg.tick_size;
            qr.bidQty = std::max(computeQty(0), _cfg.obligation_min_qty);
            qr.is_obligation_bid = true;
        }
        // 义务模式: allow 不阻断，至少保持 baseQty
        qr.bidQty = std::max(qr.bidQty, 1.0);
        qr.askQty = std::max(qr.askQty, 1.0);
    }
    else
    {
        // 自由模式: qty 衰减
        if (long_util > 0.0) {
            double decay = std::exp(-_cfg.qty_decay_factor * long_util);
            qr.bidQty = std::max(0.0, std::round(qr.bidQty * decay));
        }
        if (short_util > 0.0) {
            double decay = std::exp(-_cfg.qty_decay_factor * short_util);
            qr.askQty = std::max(0.0, std::round(qr.askQty * decay));
        }
        // allow 阻断
        if (!allow_bid) qr.bidQty = 0;
        if (!allow_ask) qr.askQty = 0;
    }

    // 价格保护 (不对义务报价应用)
    if (_cfg.price_protection)
    {
        if (qr.bidQty > 0 && !qr.is_obligation_bid && best_bid > 0)
            qr.bidPrice = std::min(qr.bidPrice, best_bid + _cfg.protect_ticks * _cfg.tick_size);
        if (qr.askQty > 0 && !qr.is_obligation_ask && best_ask > 0)
            qr.askPrice = std::max(qr.askPrice, best_ask - _cfg.protect_ticks * _cfg.tick_size);
    }

    // 价格边界验证 (涨跌停)
    if (qr.bidQty > 0 && !validatePrice(qr.bidPrice, mid, upper_limit, lower_limit)) qr.bidQty = 0;
    if (qr.askQty > 0 && !validatePrice(qr.askPrice, mid, upper_limit, lower_limit)) qr.askQty = 0;

    return qr;
}

uint32_t FutuQuoter::handleBilateralQuote(uint32_t level, const QuoteResult& qr, double mid, uint64_t now)
{
    QuoteLevel& bid_level = _bid_levels[level];
    QuoteLevel& ask_level = _ask_levels[level];

    // 做市双边接口: 必须双边报单，顶单自动撤旧
    // 即使一侧 allow=false 也用 stra_quote（价格偏到义务区间）
    bool bid_need_update = (bid_level.order_id == 0);
    bool ask_need_update = (ask_level.order_id == 0);

    if (_tracker) {
        if (auto* oi = _tracker->getOrderByOrderId(bid_level.order_id); oi && !oi->isPendingCancel())
            bid_need_update = checkStickyUpdate(qr.bidPrice, oi->price, true);
        else if (bid_level.order_id != 0)
            bid_need_update = true;
    } else if (bid_level.order_id != 0) {
        bid_need_update = checkStickyUpdate(qr.bidPrice, bid_level.price, true);
    }

    if (_tracker) {
        if (auto* oi = _tracker->getOrderByOrderId(ask_level.order_id); oi && !oi->isPendingCancel())
            ask_need_update = checkStickyUpdate(qr.askPrice, oi->price, false);
        else if (ask_level.order_id != 0)
            ask_need_update = true;
    } else if (ask_level.order_id != 0) {
        ask_need_update = checkStickyUpdate(qr.askPrice, ask_level.price, false);
    }

    if (!bid_need_update && !ask_need_update)
        return 0;

    // 标记旧单 pending_cancel（stra_quote 顶单会自动撤旧）
    if (bid_level.order_id != 0) {
        if (_tracker) _tracker->markPendingCancel(bid_level.order_id, CancelReason::MANUAL);
        bid_level.order_id = 0;
    }
    if (ask_level.order_id != 0) {
        if (_tracker) _tracker->markPendingCancel(ask_level.order_id, CancelReason::MANUAL);
        ask_level.order_id = 0;
    }

    auto [bidId, askId] = _ctx->stra_quote(_cfg.code.c_str(), qr.bidPrice, qr.bidQty,
                                            qr.askPrice, qr.askQty,
                                            (_allow_bid && _allow_ask) ? "MM_BILATERAL" : "MM_OBLIGATION");
    if (bidId != 0 && askId != 0)
    {
        bid_level.order_id = bidId; bid_level.price = qr.bidPrice; bid_level.qty = qr.bidQty;
        ask_level.order_id = askId; ask_level.price = qr.askPrice; ask_level.qty = qr.askQty;
        _order_id_to_level[bidId] = {static_cast<uint8_t>(level), true};
        _order_id_to_level[askId] = {static_cast<uint8_t>(level), false};
        if (_tracker && now > 0) {
            _tracker->trackMMOrder(bidId, static_cast<uint8_t>(level), _cfg.code, qr.bidPrice, qr.bidQty, mid, now, true);
            _tracker->trackMMOrder(askId, static_cast<uint8_t>(level), _cfg.code, qr.askPrice, qr.askQty, mid, now, false);
        }
        return 2;
    }
    return 0;
}

uint32_t FutuQuoter::handleObligationQuote(uint32_t level, const QuoteResult& qr, double mid, uint64_t now)
{
    QuoteLevel& bid_level = _bid_levels[level];
    QuoteLevel& ask_level = _ask_levels[level];

    // 义务模式: 必须双边，先撤所有残留（含部分成交），不走 sticky
    if (bid_level.order_id != 0) {
        _ctx->stra_cancel(bid_level.order_id);
        if (_tracker) _tracker->markPendingCancel(bid_level.order_id, CancelReason::MANUAL);
        bid_level.order_id = 0;
    }
    if (ask_level.order_id != 0) {
        _ctx->stra_cancel(ask_level.order_id);
        if (_tracker) _tracker->markPendingCancel(ask_level.order_id, CancelReason::MANUAL);
        ask_level.order_id = 0;
    }

    uint32_t orders = 0;

    // 双边下单
    auto bidIds = _ctx->stra_buy(_cfg.code.c_str(), qr.bidPrice, qr.bidQty);
    if (!bidIds.empty()) {
        uint32_t bidId = bidIds[0];
        bid_level.order_id = bidId; bid_level.price = qr.bidPrice; bid_level.qty = qr.bidQty;
        _order_id_to_level[bidId] = {static_cast<uint8_t>(level), true};
        if (_tracker && now > 0) _tracker->trackMMOrder(bidId, static_cast<uint8_t>(level), _cfg.code, qr.bidPrice, qr.bidQty, mid, now, true);
        orders++;
    }

    auto askIds = _ctx->stra_sell(_cfg.code.c_str(), qr.askPrice, qr.askQty);
    if (!askIds.empty()) {
        uint32_t askId = askIds[0];
        ask_level.order_id = askId; ask_level.price = qr.askPrice; ask_level.qty = qr.askQty;
        _order_id_to_level[askId] = {static_cast<uint8_t>(level), false};
        if (_tracker && now > 0) _tracker->trackMMOrder(askId, static_cast<uint8_t>(level), _cfg.code, qr.askPrice, qr.askQty, mid, now, false);
        orders++;
    }

    return orders;
}

uint32_t FutuQuoter::handleFlexibleQuote(uint32_t level, const QuoteResult& qr, double mid, uint64_t now)
{
    QuoteLevel& bid_level = _bid_levels[level];
    QuoteLevel& ask_level = _ask_levels[level];
    uint32_t orders = 0;

    // B2 自由模式: sticky + 可单边 + qty 衰减
    // Bid
    if (qr.bidQty == 0) {
        if (bid_level.order_id != 0) {
            _ctx->stra_cancel(bid_level.order_id);
            if (_tracker) _tracker->markPendingCancel(bid_level.order_id, CancelReason::INVENTORY_LIMIT);
            bid_level.order_id = 0;
        }
    } else {
        bool need_update = (bid_level.order_id == 0);
        if (!need_update) {
            if (_tracker) {
                auto* oi = _tracker->getOrderByOrderId(bid_level.order_id);
                if (oi && !oi->isPendingCancel())
                    need_update = checkStickyUpdate(qr.bidPrice, oi->price, true);
                else
                    need_update = true;
            } else {
                need_update = checkStickyUpdate(qr.bidPrice, bid_level.price, true);
            }
        }
        if (need_update) {
            if (bid_level.order_id != 0) {
                _ctx->stra_cancel(bid_level.order_id);
                if (_tracker) _tracker->markPendingCancel(bid_level.order_id, CancelReason::PRICE_DEVIATION);
                bid_level.order_id = 0;
            }
            auto ids = _ctx->stra_buy(_cfg.code.c_str(), qr.bidPrice, qr.bidQty);
            if (!ids.empty()) {
                uint32_t bidId = ids[0];
                bid_level.order_id = bidId; bid_level.price = qr.bidPrice; bid_level.qty = qr.bidQty;
                _order_id_to_level[bidId] = {static_cast<uint8_t>(level), true};
                if (_tracker && now > 0) _tracker->trackMMOrder(bidId, static_cast<uint8_t>(level), _cfg.code, qr.bidPrice, qr.bidQty, mid, now, true);
                orders++;
            }
        }
    }

    // Ask
    if (qr.askQty == 0) {
        if (ask_level.order_id != 0) {
            _ctx->stra_cancel(ask_level.order_id);
            if (_tracker) _tracker->markPendingCancel(ask_level.order_id, CancelReason::INVENTORY_LIMIT);
            ask_level.order_id = 0;
        }
    } else {
        bool need_update = (ask_level.order_id == 0);
        if (!need_update) {
            if (_tracker) {
                auto* oi = _tracker->getOrderByOrderId(ask_level.order_id);
                if (oi && !oi->isPendingCancel())
                    need_update = checkStickyUpdate(qr.askPrice, oi->price, false);
                else
                    need_update = true;
            } else {
                need_update = checkStickyUpdate(qr.askPrice, ask_level.price, false);
            }
        }
        if (need_update) {
            if (ask_level.order_id != 0) {
                _ctx->stra_cancel(ask_level.order_id);
                if (_tracker) _tracker->markPendingCancel(ask_level.order_id, CancelReason::PRICE_DEVIATION);
                ask_level.order_id = 0;
            }
            auto ids = _ctx->stra_sell(_cfg.code.c_str(), qr.askPrice, qr.askQty);
            if (!ids.empty()) {
                uint32_t askId = ids[0];
                ask_level.order_id = askId; ask_level.price = qr.askPrice; ask_level.qty = qr.askQty;
                _order_id_to_level[askId] = {static_cast<uint8_t>(level), false};
                if (_tracker && now > 0) _tracker->trackMMOrder(askId, static_cast<uint8_t>(level), _cfg.code, qr.askPrice, qr.askQty, mid, now, false);
                orders++;
            }
        }
    }

    return orders;
}

uint32_t FutuQuoter::refreshQuotes(wtp::IUftStraCtx* ctx, double mid, double l0_bid_price, double l0_ask_price,
                                    double spread_mult, bool allow_bid, bool allow_ask,
                                    uint64_t now, double upper_limit, double lower_limit,
                                    double best_bid, double best_ask,
                                    double long_util, double short_util,
                                    bool force_ask_obligation, bool force_bid_obligation)
{
    uint32_t orders_placed = 0;
    _ctx = ctx;
    _allow_bid = allow_bid;
    _allow_ask = allow_ask;
    if (!ctx) return 0;

    for (uint32_t i = 0; i < _cfg.num_levels; i++)
    {
        // 判断是否需要履行做市义务(双边报单)
        bool is_obligation = needObligation(i, force_ask_obligation, force_bid_obligation);

        // 统一定价
        QuoteResult qr = computeQuotePrices(
            i, mid, l0_bid_price, l0_ask_price,
            spread_mult, allow_bid, allow_ask,
            upper_limit, lower_limit,
            best_bid, best_ask,
            long_util, short_util,
            force_ask_obligation, force_bid_obligation,
            is_obligation);

        // 路径选择
        if (_cfg.use_bilateral_quote && i == 0)
        {
            // 路径A: 做市双边接口 (stra_quote)
            orders_placed += handleBilateralQuote(i, qr, mid, now);
        }
        else if (is_obligation)
        {
            // 路径B1: 普通报单 + 做市义务 (先撤残留, 双边下单, 不走 sticky)
            orders_placed += handleObligationQuote(i, qr, mid, now);
        }
        else
        {
            // 路径B2: 普通报单 + 自由报价 (sticky, 可单边)
            orders_placed += handleFlexibleQuote(i, qr, mid, now);
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
            if (_tracker) _tracker->markPendingCancel(level.order_id, CancelReason::MANUAL);
            ctx->stra_cancel(level.order_id);
            _order_id_to_level.erase(level.order_id);
            level.order_id = 0;
        }
    }

    for (auto& level : _ask_levels)
    {
        if (level.order_id != 0)
        {
            if (_tracker) _tracker->markPendingCancel(level.order_id, CancelReason::MANUAL);
            ctx->stra_cancel(level.order_id);
            _order_id_to_level.erase(level.order_id);
            level.order_id = 0;
        }
    }
}

void FutuQuoter::onOrder(uint32_t localid, bool isCanceled, double leftQty,
                         uint32_t uTime_HHMM, uint32_t sec_in_min)
{
    // Find level using tracker or linear search
    QuoteLevel* level = nullptr;
    
    if (_tracker)
    {
        auto* orderInfo = _tracker->getOrderByOrderId(localid);
        if (orderInfo)
        {
            uint8_t idx = orderInfo->level_index;
            if (orderInfo->isBid() && idx < _bid_levels.size())
                level = &_bid_levels[idx];
            else if (!orderInfo->isBid() && idx < _ask_levels.size())
                level = &_ask_levels[idx];
        }
    }
    
    if (!level)
    {
        for (auto& l : _bid_levels) if (l.order_id == localid) { level = &l; break; }
        if (!level)
            for (auto& l : _ask_levels) if (l.order_id == localid) { level = &l; break; }
    }
    
    if (level)
    {
        if (isCanceled || leftQty == 0)
        {
            if (level->order_id == localid) {
                level->order_id = 0;
            }
            if (_tracker) _tracker->untrackOrder(localid);
            _order_id_to_level.erase(localid);
        }
        else
        {
            if (level->order_id == localid) {
                level->qty = leftQty;
            }
            if (_tracker) _tracker->updateOrderQty(localid, leftQty);
            
            if (_tracker)
            {
                auto* orderInfo = _tracker->getOrderByOrderId(localid);
                if (orderInfo && !orderInfo->isPendingCancel())
                {
                    bool should_cancel = false;
                    if (orderInfo->isBid() && !_allow_bid)
                        should_cancel = true;
                    else if (!orderInfo->isBid() && !_allow_ask)
                        should_cancel = true;
                    
                    if (should_cancel && _ctx)
                    {
                        WTSLogger::warn("[QUOTER] Post-submit cancel: {} order {} entered UnTrd but side blocked, cancelling",
                            _cfg.code, localid);
                        _ctx->stra_cancel(localid);
                        _tracker->markPendingCancel(localid, CancelReason::INVENTORY_LIMIT);
                    }
                }
            }
        }
        
        // R3 v2: 触发 BilateralStats 更新（uTime_HHMM=0 表示调用方未传时间，跳过；
        // hasSessionInfo()=false 表示 sessinfo 注入失败，统计已 DISABLED）
        if (uTime_HHMM > 0 && _bilateral_stats.hasSessionInfo())
        {
            auto snapshot = getValidQuoteSnapshot();
            _bilateral_stats.update(snapshot, uTime_HHMM, sec_in_min);
        }
    }
}

void FutuQuoter::onTrade(uint32_t localid, double vol, double price,
                         uint32_t uTime_HHMM, uint32_t sec_in_min)
{
    if (_tracker) _tracker->recordFilled();

    // R3 v2: 成交导致挂单 qty 减少，可能跌出 min_valid_qty 累计深度阈值
    // 必须重新评估 valid 状态（onOrder 路径不一定能覆盖所有部分成交场景）
    if (uTime_HHMM > 0 && _bilateral_stats.hasSessionInfo())
    {
        auto snapshot = getValidQuoteSnapshot();
        _bilateral_stats.update(snapshot, uTime_HHMM, sec_in_min);
    }
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

bool FutuQuoter::isMyOrder(uint32_t localid) const
{
    return _order_id_to_level.find(localid) != _order_id_to_level.end();
}

QuoteLevel* FutuQuoter::getLevelByOrder(uint32_t localid)
{
    auto it = _order_id_to_level.find(localid);
    if (it != _order_id_to_level.end())
    {
        // Use is_bid to directly locate the correct level
        uint8_t idx = it->second.level;
        bool is_bid = it->second.is_bid;
        if (is_bid && idx < _bid_levels.size())
            return &_bid_levels[idx];
        if (!is_bid && idx < _ask_levels.size())
            return &_ask_levels[idx];
    }
    return nullptr;
}

ValidQuoteSnapshot FutuQuoter::getValidQuoteSnapshot() const
{
    ValidQuoteSnapshot snapshot;
    snapshot.tick_size = _cfg.tick_size;

    const double target = _cfg.min_valid_qty;
    if (target <= 0)
        return snapshot;

    // Bid 侧累计加权（_bid_levels 已按价格优劣排序，level 0 = 最优 bid）
    {
        double cum_qty = 0;
        double weighted_sum = 0;  // Σ(qty_i × price_i)
        for (const auto& level : _bid_levels)
        {
            if (level.order_id == 0 || level.qty <= 0) continue;
            double remain = target - cum_qty;
            if (remain <= 0) break;
            double take = (level.qty >= remain) ? remain : level.qty;
            weighted_sum += take * level.price;
            cum_qty += take;
            if (cum_qty >= target) break;
        }
        if (cum_qty >= target - 1e-9)
        {
            snapshot.has_valid_bid = true;
            snapshot.weighted_bid_price = weighted_sum / target;
        }
    }

    // Ask 侧累计加权
    {
        double cum_qty = 0;
        double weighted_sum = 0;
        for (const auto& level : _ask_levels)
        {
            if (level.order_id == 0 || level.qty <= 0) continue;
            double remain = target - cum_qty;
            if (remain <= 0) break;
            double take = (level.qty >= remain) ? remain : level.qty;
            weighted_sum += take * level.price;
            cum_qty += take;
            if (cum_qty >= target) break;
        }
        if (cum_qty >= target - 1e-9)
        {
            snapshot.has_valid_ask = true;
            snapshot.weighted_ask_price = weighted_sum / target;
        }
    }

    return snapshot;
}

} // namespace futu
