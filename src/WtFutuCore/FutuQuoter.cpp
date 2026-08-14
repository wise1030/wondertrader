/*!
 * \file FutuQuoter.cpp
 * \brief Multi-level bilateral quoting engine implementation
 */
#include "FutuQuoter.h"
#include "OrderApiGuard.h"
#include "UnifiedOrderTracker.h"
#include "BilateralQuoteStats.h"
#include "../Includes/IUftStraCtx.h"
#include "../WTSTools/WTSLogger.h"
#include <cstring>
#include <utility>   // for std::pair and structured bindings
#include <algorithm> // for std::remove

namespace futu
{

namespace
{
// 撤单前防御：tracker 已 force-untrack 或订单已进入 PENDING_CANCEL 时，
// FutuQuoter 本地 level 仍可能残留 stale id。此时不应再发 stra_cancel，
// 只由调用方清理 level.order_ids / _order_id_to_level。
// tracker 为空时保持旧行为（无条件撤单），避免破坏回测/无 tracker 路径。
bool canSendCancel(const UnifiedOrderTracker* tracker, uint32_t order_id)
{
    if (!tracker)
        return true;

    UnifiedOrderInfo oi;
    if (!tracker->getOrderInfoCopy(order_id, oi))
        return false;

    return !oi.isPendingCancel();
}
} // namespace

FutuQuoter::FutuQuoter() : _tracker(nullptr)
{
    RecursiveSpinGuard _g(_lock);
}

void FutuQuoter::init(const QuoterConfig& cfg)
{
    RecursiveSpinGuard _g(_lock);
    _cfg = cfg;
    _bid_levels.resize(cfg.num_levels);
    _ask_levels.resize(cfg.num_levels);
    _level_qtys.resize(cfg.num_levels);

    for (uint32_t i = 0; i < cfg.num_levels; i++) {
        _bid_levels[i].is_bid = true;
        _bid_levels[i].level_index = static_cast<uint8_t>(i);
        _ask_levels[i].is_bid = false;
        _ask_levels[i].level_index = static_cast<uint8_t>(i);

        double qty = cfg.base_qty * std::pow(cfg.level_qty_multiplier, i);
        _level_qtys[i] = std::max(1.0, std::round(qty));
    }
}

FutuQuoter::QuoteResult FutuQuoter::computeObligationPrices(uint32_t level,
                                                                double mid,
                                                                double l0_bid_price,
                                                                double l0_ask_price,
                                                                bool allow_bid,
                                                                bool allow_ask,
                                                                double upper_limit,
                                                                double lower_limit,
                                                                double best_bid,
                                                                double best_ask,
                                                                const StrategyInputs& strategy,
                                                                double long_decay,
                                                                double short_decay)
{
    RecursiveSpinGuard _g(_lock);
    QuoteResult qr{};
    qr.is_obligation_bid = false;
    qr.is_obligation_ask = false;

    if (long_decay <= 0.0)
        long_decay = (strategy.long_util > 0.0) ? std::exp(-_cfg.qty_decay_factor * strategy.long_util) : 1.0;
    if (short_decay <= 0.0)
        short_decay = (strategy.short_util > 0.0) ? std::exp(-_cfg.qty_decay_factor * strategy.short_util) : 1.0;

    double level_offset = level * _cfg.level_step * _cfg.tick_size;
    qr.bidPrice = floor((l0_bid_price - level_offset) / _cfg.tick_size) * _cfg.tick_size;
    qr.askPrice = ceil((l0_ask_price + level_offset) / _cfg.tick_size) * _cfg.tick_size;
    qr.bidQty = computeQty(level);
    qr.askQty = computeQty(level);

    // Obligation band (exchange max spread requirement)
    // 注: 本函数仅被 obligation level 调用 (needObligation 保证 level==obligation_level),
    //     原 apply_obligation 检查恒 true, 已删除
    double ask_cap =
        ceil((mid + _cfg.obligation_max_spread_ticks * _cfg.tick_size) / _cfg.tick_size) * _cfg.tick_size;
    double bid_floor =
        floor((mid - _cfg.obligation_max_spread_ticks * _cfg.tick_size) / _cfg.tick_size) * _cfg.tick_size;

    if (strategy.force_ask_obligation) {
        qr.askPrice = std::min(qr.askPrice, ask_cap);
        qr.askQty = std::max(computeQty(0), _cfg.obligation_min_qty);
        qr.is_obligation_ask = true;
        if (qr.bidQty > 0)
            qr.bidPrice = std::max(qr.bidPrice, bid_floor);
    }
    if (strategy.force_bid_obligation) {
        qr.bidPrice = std::max(qr.bidPrice, bid_floor);
        qr.bidQty = std::max(computeQty(0), _cfg.obligation_min_qty);
        qr.is_obligation_bid = true;
        if (qr.askQty > 0)
            qr.askPrice = std::min(qr.askPrice, ask_cap);
    }
    // Obligation: hard_block does NOT block (Phase 2). Adding side at passive price.
    qr.bidQty = std::max(qr.bidQty, computeQty(0));
    qr.askQty = std::max(qr.askQty, computeQty(0));

    // Pending drain overrides obligation (flow control)
    if (!allow_bid) {
        qr.bidQty = 0;
        qr.is_obligation_bid = false;
    }
    if (!allow_ask) {
        qr.askQty = 0;
        qr.is_obligation_ask = false;
    }

    applyPriceProtection(qr, mid, upper_limit, lower_limit, best_bid, best_ask);
    return qr;
}

FutuQuoter::QuoteResult FutuQuoter::computeFlexiblePrices(uint32_t level,
                                                           double mid,
                                                           double l0_bid_price,
                                                           double l0_ask_price,
                                                           bool allow_bid,
                                                           bool allow_ask,
                                                           double upper_limit,
                                                           double lower_limit,
                                                           double best_bid,
                                                           double best_ask,
                                                           const StrategyInputs& strategy,
                                                           double long_decay,
                                                           double short_decay)
{
    RecursiveSpinGuard _g(_lock);
    QuoteResult qr{};
    qr.is_obligation_bid = false;
    qr.is_obligation_ask = false;

    if (long_decay <= 0.0)
        long_decay = (strategy.long_util > 0.0) ? std::exp(-_cfg.qty_decay_factor * strategy.long_util) : 1.0;
    if (short_decay <= 0.0)
        short_decay = (strategy.short_util > 0.0) ? std::exp(-_cfg.qty_decay_factor * strategy.short_util) : 1.0;

    double level_offset = level * _cfg.level_step * _cfg.tick_size;
    qr.bidPrice = floor((l0_bid_price - level_offset) / _cfg.tick_size) * _cfg.tick_size;
    qr.askPrice = ceil((l0_ask_price + level_offset) / _cfg.tick_size) * _cfg.tick_size;
    qr.bidQty = computeQty(level);
    qr.askQty = computeQty(level);

    // Flexible: withdraw during obligation period (only obligation layer quotes)
    if (strategy.force_ask_obligation || strategy.force_bid_obligation) {
        qr.bidQty = 0;
        qr.askQty = 0;
    } else if (level < _cfg.obligation_level) {
        qr.bidQty = std::min(qr.bidQty, _cfg.scout_qty);
        qr.askQty = std::min(qr.askQty, _cfg.scout_qty);
    }

    // Qty decay based on utilization
    if (strategy.long_util > 0.0) {
        qr.bidQty = std::max(0.0, std::round(qr.bidQty * long_decay));
    }
    if (strategy.short_util > 0.0) {
        qr.askQty = std::max(0.0, std::round(qr.askQty * short_decay));
    }

    // block_add: 策略库存管理 (非风控), flexible 加仓侧 qty=0
    if (strategy.block_add_long) {
        qr.bidQty = 0;
    }
    if (strategy.block_add_short) {
        qr.askQty = 0;
    }

    // Soft block (allow=false): flexible 直接 qty=0
    // (原"obligation level 加宽保义务"分支已删除: obligation level 永远走 computeObligationPrices,
    //  flexible 单绝不获得 is_obligation 标记/价格保护豁免)
    if (!allow_bid && !strategy.block_add_long) {
        qr.bidQty = 0;
    }
    if (!allow_ask && !strategy.block_add_short) {
        qr.askQty = 0;
    }

    applyPriceProtection(qr, mid, upper_limit, lower_limit, best_bid, best_ask);
    return qr;
}

void FutuQuoter::applyPriceProtection(QuoteResult& qr, double mid,
                                       double upper_limit, double lower_limit,
                                       double best_bid, double best_ask) const
{
    // Hard constraint: prevent crossing market (applies to all levels including obligation)
    if (qr.bidQty > 0 && best_ask > 0)
        qr.bidPrice = std::min(qr.bidPrice, best_ask - _cfg.tick_size);
    if (qr.askQty > 0 && best_bid > 0)
        qr.askPrice = std::max(qr.askPrice, best_bid + _cfg.tick_size);

    // Soft constraint: price protection (non-obligation only)
    if (_cfg.price_protection) {
        if (qr.bidQty > 0 && !qr.is_obligation_bid && best_bid > 0)
            qr.bidPrice = std::min(qr.bidPrice, best_bid + _cfg.protect_ticks * _cfg.tick_size);
        if (qr.askQty > 0 && !qr.is_obligation_ask && best_ask > 0)
            qr.askPrice = std::max(qr.askPrice, best_ask - _cfg.protect_ticks * _cfg.tick_size);
    }

    // Price boundary validation (limit up/down)
    if (qr.bidQty > 0) {
        if (qr.is_obligation_bid) {
            if (std::isnan(qr.bidPrice) || std::isinf(qr.bidPrice) || qr.bidPrice <= 0)
                qr.bidQty = 0;
            else {
                if (upper_limit > 0 && qr.bidPrice > upper_limit)
                    qr.bidPrice = upper_limit;
                if (lower_limit > 0 && qr.bidPrice < lower_limit)
                    qr.bidPrice = lower_limit;
            }
        } else if (!validatePrice(qr.bidPrice, mid, upper_limit, lower_limit)) {
            qr.bidQty = 0;
        }
    }
    if (qr.askQty > 0) {
        if (qr.is_obligation_ask) {
            if (std::isnan(qr.askPrice) || std::isinf(qr.askPrice) || qr.askPrice <= 0)
                qr.askQty = 0;
            else {
                if (upper_limit > 0 && qr.askPrice > upper_limit)
                    qr.askPrice = upper_limit;
                if (lower_limit > 0 && qr.askPrice < lower_limit)
                    qr.askPrice = lower_limit;
            }
        } else if (!validatePrice(qr.askPrice, mid, upper_limit, lower_limit)) {
            qr.askQty = 0;
        }
    }
}

uint32_t FutuQuoter::handleBilateralQuote(uint32_t level, const QuoteResult& qr, double mid, uint64_t now)
{
    RecursiveSpinGuard _g(_lock);
    QuoteLevel& bid_level = _bid_levels[level];
    QuoteLevel& ask_level = _ask_levels[level];

    // 做市双边接口: 必须双边报单，顶单自动撤旧
    bool bid_need_update = !bid_level.hasOrders();
    bool ask_need_update = !ask_level.hasOrders();

    if (_tracker && !bid_level.order_ids.empty()) {
        UnifiedOrderInfo bid_oi;
        if (bool bid_found = _tracker->getOrderInfoCopy(bid_level.order_ids[0], bid_oi);
            bid_found && !bid_oi.isPendingCancel())
            bid_need_update = checkStickyUpdate(qr.bidPrice, bid_oi.price, true);
        else
            bid_need_update = true;
    } else if (bid_level.hasOrders()) {
        bid_need_update = checkStickyUpdate(qr.bidPrice, bid_level.price, true);
    }

    if (_tracker && !ask_level.order_ids.empty()) {
        UnifiedOrderInfo ask_oi;
        if (bool ask_found = _tracker->getOrderInfoCopy(ask_level.order_ids[0], ask_oi);
            ask_found && !ask_oi.isPendingCancel())
            ask_need_update = checkStickyUpdate(qr.askPrice, ask_oi.price, false);
        else
            ask_need_update = true;
    } else if (ask_level.hasOrders()) {
        ask_need_update = checkStickyUpdate(qr.askPrice, ask_level.price, false);
    }

    if (!bid_need_update && !ask_need_update)
        return 0;

    // 标记旧单 pending_cancel（stra_quote 顶单会自动撤旧）
    for (uint32_t id : bid_level.order_ids) {
        if (_tracker)
            _tracker->markPendingCancel(id, CancelReason::MANUAL);
    }
    bid_level.order_ids.clear();
    for (uint32_t id : ask_level.order_ids) {
        if (_tracker)
            _tracker->markPendingCancel(id, CancelReason::MANUAL);
    }
    ask_level.order_ids.clear();

    auto [bidId, askId] = orderApiCall([&] {
        return _ctx->stra_quote(_cfg.code.c_str(),
                                qr.bidPrice,
                                qr.bidQty,
                                qr.askPrice,
                                qr.askQty,
                                (_allow_bid && _allow_ask) ? "MM_BILATERAL" : "MM_OBLIGATION");
    });
    // 单侧成功也要登记跟踪 — 否则成功侧订单成为孤儿单(在场但无人管理/撤单)
    uint32_t placed = 0;
    if (bidId != 0) {
        bid_level.order_ids = {bidId};
        bid_level.price = qr.bidPrice;
        bid_level.qty = qr.bidQty;
        _order_id_to_level[bidId] = {static_cast<uint8_t>(level), true};
        if (_tracker && now > 0)
            _tracker->trackMMOrder(
                bidId, static_cast<uint8_t>(level), _cfg.code, qr.bidPrice, qr.bidQty, mid, now, true);
        ++placed;
    }
    if (askId != 0) {
        ask_level.order_ids = {askId};
        ask_level.price = qr.askPrice;
        ask_level.qty = qr.askQty;
        _order_id_to_level[askId] = {static_cast<uint8_t>(level), false};
        if (_tracker && now > 0)
            _tracker->trackMMOrder(
                askId, static_cast<uint8_t>(level), _cfg.code, qr.askPrice, qr.askQty, mid, now, false);
        ++placed;
    }
    if (placed == 1) {
        WTSLogger::warn("FutuQuoter[{}]: stra_quote partial failure (bidId={}, askId={}), tracking orphan side",
                        _cfg.code,
                        bidId,
                        askId);
    }
    return placed;
}

uint32_t FutuQuoter::handleObligationQuote(uint32_t level, const QuoteResult& qr, double mid, uint64_t now)
{
    RecursiveSpinGuard _g(_lock);
    QuoteLevel& bid_level = _bid_levels[level];
    QuoteLevel& ask_level = _ask_levels[level];

    // 义务模式: 必须双边。v7.1 条件式重挂 (黏性+深度), 替代旧"每tick无脑全撤重挂":
    //   各侧 need = 无单 || 深度<min_valid_qty || 价格超黏性阈值; 仅 need 侧撤+重挂,
    //   双侧都不 need 则零撤单零报单 (减少信息流, 与 requoteAfterFill 深度语义一致)。
    auto sideNeed = [&](QuoteLevel& lv, double newPrice, bool is_bid) -> bool {
        if (!lv.hasOrders())
            return true;
        if (lv.qty < _cfg.min_valid_qty)
            return true; // 深度被部分成交侵蚀至义务以下
        double cur = lv.price;
        if (_tracker && !lv.order_ids.empty()) {
            UnifiedOrderInfo lv_oi;
            if (bool lv_found = _tracker->getOrderInfoCopy(lv.order_ids[0], lv_oi);
                lv_found && !lv_oi.isPendingCancel())
                cur = lv_oi.price;
            else
                return true; // 死单/pending-cancel -> 重挂
        }
        return checkStickyUpdate(newPrice, cur, is_bid);
    };

    // 义务模式 qr.bidQty/askQty 通常 >= 1 (computeObligationPrices 保底);
    // 但 allow 阻断 (drain/TradingState) 会清零 -> 此时须主动撤存量单,
    // sideNeed 只比较价格/深度不知道 qty 被清零, 需要显式 cancel-only 路径
    // (与 handleFlexibleQuote 的 qty==0 撤单行为对齐, 消除两条执行路径的不对称)
    bool bid_need = sideNeed(bid_level, qr.bidPrice, true);
    bool ask_need = sideNeed(ask_level, qr.askPrice, false);
    bool bid_cancel_only = (qr.bidQty == 0) && bid_level.hasOrders();
    bool ask_cancel_only = (qr.askQty == 0) && ask_level.hasOrders();
    if (!bid_need && !ask_need && !bid_cancel_only && !ask_cancel_only) {
        WTSLogger::debug("[STICKY] {} skip requote (bid={} ask={} within threshold, depth ok)",
                         _cfg.code,
                         bid_level.price,
                         ask_level.price);
        return 0;
    }

    uint32_t orders = 0;

    // qty=0 侧: 仅撤存量单 (allow 阻断), 不挂新单
    if (bid_cancel_only) {
        for (uint32_t id : bid_level.order_ids) {
            if (canSendCancel(_tracker, id)) {
                orderApiCall([&] { return _ctx->stra_cancel(id); });
                if (_tracker)
                    _tracker->markPendingCancel(id, CancelReason::INVENTORY_LIMIT);
            } else {
                WTSLogger::warn("FutuQuoter[{}]: skip stale/pending cancel order {}", _cfg.code, id);
            }
        }
        bid_level.order_ids.clear();
    }
    if (ask_cancel_only) {
        for (uint32_t id : ask_level.order_ids) {
            if (canSendCancel(_tracker, id)) {
                orderApiCall([&] { return _ctx->stra_cancel(id); });
                if (_tracker)
                    _tracker->markPendingCancel(id, CancelReason::INVENTORY_LIMIT);
            } else {
                WTSLogger::warn("FutuQuoter[{}]: skip stale/pending cancel order {}", _cfg.code, id);
            }
        }
        ask_level.order_ids.clear();
    }

    // 仅 need 侧: 撤残留 + 重挂 (stra_buy/sell 可能返回多个子单 ID, 全部跟踪)
    if (bid_need && qr.bidQty > 0) {
        for (uint32_t id : bid_level.order_ids) {
            if (canSendCancel(_tracker, id)) {
                orderApiCall([&] { return _ctx->stra_cancel(id); });
                if (_tracker)
                    _tracker->markPendingCancel(id, CancelReason::MANUAL);
            } else {
                WTSLogger::warn("FutuQuoter[{}]: skip stale/pending cancel order {}", _cfg.code, id);
            }
        }
        bid_level.order_ids.clear();
        auto bidIds = orderApiCall([&] { return _ctx->stra_buy(_cfg.code.c_str(), qr.bidPrice, qr.bidQty); });
        for (uint32_t bidId : bidIds) {
            if (bidId != 0) {
                bid_level.order_ids.push_back(bidId);
                _order_id_to_level[bidId] = {static_cast<uint8_t>(level), true};
                if (_tracker && now > 0)
                    _tracker->trackMMOrder(
                        bidId, static_cast<uint8_t>(level), _cfg.code, qr.bidPrice, qr.bidQty, mid, now, true);
                orders++;
            }
        }
        if (!bid_level.order_ids.empty()) {
            bid_level.price = qr.bidPrice;
            bid_level.qty = qr.bidQty;
        }
    }

    if (ask_need && qr.askQty > 0) {
        for (uint32_t id : ask_level.order_ids) {
            if (canSendCancel(_tracker, id)) {
                orderApiCall([&] { return _ctx->stra_cancel(id); });
                if (_tracker)
                    _tracker->markPendingCancel(id, CancelReason::MANUAL);
            } else {
                WTSLogger::warn("FutuQuoter[{}]: skip stale/pending cancel order {}", _cfg.code, id);
            }
        }
        ask_level.order_ids.clear();
        auto askIds = orderApiCall([&] { return _ctx->stra_sell(_cfg.code.c_str(), qr.askPrice, qr.askQty); });
        for (uint32_t askId : askIds) {
            if (askId != 0) {
                ask_level.order_ids.push_back(askId);
                _order_id_to_level[askId] = {static_cast<uint8_t>(level), false};
                if (_tracker && now > 0)
                    _tracker->trackMMOrder(
                        askId, static_cast<uint8_t>(level), _cfg.code, qr.askPrice, qr.askQty, mid, now, false);
                orders++;
            }
        }
        if (!ask_level.order_ids.empty()) {
            ask_level.price = qr.askPrice;
            ask_level.qty = qr.askQty;
        }
    }

    return orders;
}

uint32_t FutuQuoter::handleFlexibleQuote(uint32_t level, const QuoteResult& qr, double mid, uint64_t now)
{
    RecursiveSpinGuard _g(_lock);
    QuoteLevel& bid_level = _bid_levels[level];
    QuoteLevel& ask_level = _ask_levels[level];
    uint32_t orders = 0;

    // B2 自由模式: sticky + 可单边 + qty 衰减
    // Bid
    if (qr.bidQty == 0) {
        for (uint32_t id : bid_level.order_ids) {
            if (canSendCancel(_tracker, id)) {
                orderApiCall([&] { return _ctx->stra_cancel(id); });
                if (_tracker)
                    _tracker->markPendingCancel(id, CancelReason::INVENTORY_LIMIT);
            } else {
                WTSLogger::warn("FutuQuoter[{}]: skip stale/pending cancel order {}", _cfg.code, id);
            }
        }
        bid_level.order_ids.clear();
    } else {
        bool need_update = !bid_level.hasOrders();
        if (!need_update) {
            if (_tracker && !bid_level.order_ids.empty()) {
                UnifiedOrderInfo oi;
                if (_tracker->getOrderInfoCopy(bid_level.order_ids[0], oi) && !oi.isPendingCancel())
                    need_update = checkStickyUpdate(qr.bidPrice, oi.price, true);
                else
                    need_update = true;
            }
        }
        if (need_update) {
            for (uint32_t id : bid_level.order_ids) {
                if (canSendCancel(_tracker, id)) {
                    orderApiCall([&] { return _ctx->stra_cancel(id); });
                    if (_tracker)
                        _tracker->markPendingCancel(id, CancelReason::PRICE_DEVIATION);
                } else {
                    WTSLogger::warn("FutuQuoter[{}]: skip stale/pending cancel order {}", _cfg.code, id);
                }
            }
            bid_level.order_ids.clear();
            auto ids = orderApiCall([&] { return _ctx->stra_buy(_cfg.code.c_str(), qr.bidPrice, qr.bidQty); });
            for (uint32_t bidId : ids) {
                if (bidId != 0) {
                    bid_level.order_ids.push_back(bidId);
                    _order_id_to_level[bidId] = {static_cast<uint8_t>(level), true};
                    if (_tracker && now > 0)
                        _tracker->trackMMOrder(
                            bidId, static_cast<uint8_t>(level), _cfg.code, qr.bidPrice, qr.bidQty, mid, now, true);
                    orders++;
                }
            }
            if (!bid_level.order_ids.empty()) {
                bid_level.price = qr.bidPrice;
                bid_level.qty = qr.bidQty;
            }
        }
    }

    // Ask
    if (qr.askQty == 0) {
        for (uint32_t id : ask_level.order_ids) {
            if (canSendCancel(_tracker, id)) {
                orderApiCall([&] { return _ctx->stra_cancel(id); });
                if (_tracker)
                    _tracker->markPendingCancel(id, CancelReason::INVENTORY_LIMIT);
            } else {
                WTSLogger::warn("FutuQuoter[{}]: skip stale/pending cancel order {}", _cfg.code, id);
            }
        }
        ask_level.order_ids.clear();
    } else {
        bool need_update = !ask_level.hasOrders();
        if (!need_update) {
            if (_tracker && !ask_level.order_ids.empty()) {
                UnifiedOrderInfo oi;
                if (_tracker->getOrderInfoCopy(ask_level.order_ids[0], oi) && !oi.isPendingCancel())
                    need_update = checkStickyUpdate(qr.askPrice, oi.price, false);
                else
                    need_update = true;
            }
        }
        if (need_update) {
            for (uint32_t id : ask_level.order_ids) {
                if (canSendCancel(_tracker, id)) {
                    orderApiCall([&] { return _ctx->stra_cancel(id); });
                    if (_tracker)
                        _tracker->markPendingCancel(id, CancelReason::PRICE_DEVIATION);
                } else {
                    WTSLogger::warn("FutuQuoter[{}]: skip stale/pending cancel order {}", _cfg.code, id);
                }
            }
            ask_level.order_ids.clear();
            auto ids = orderApiCall([&] { return _ctx->stra_sell(_cfg.code.c_str(), qr.askPrice, qr.askQty); });
            for (uint32_t askId : ids) {
                if (askId != 0) {
                    ask_level.order_ids.push_back(askId);
                    _order_id_to_level[askId] = {static_cast<uint8_t>(level), false};
                    if (_tracker && now > 0)
                        _tracker->trackMMOrder(
                            askId, static_cast<uint8_t>(level), _cfg.code, qr.askPrice, qr.askQty, mid, now, false);
                    orders++;
                }
            }
            if (!ask_level.order_ids.empty()) {
                ask_level.price = qr.askPrice;
                ask_level.qty = qr.askQty;
            }
        }
    }

    return orders;
}

uint32_t FutuQuoter::refreshQuotes(wtp::IUftStraCtx* ctx, const QuoteRequest& req)
{
    RecursiveSpinGuard _g(_lock);
    // B8: unpack QuoteRequest (function body uses these locals unchanged)
    double mid = req.mid;
    double l0_bid_price = req.l0_bid_price;
    double l0_ask_price = req.l0_ask_price;
    bool allow_bid = req.allow_bid;
    bool allow_ask = req.allow_ask;
    uint64_t now = req.now;
    double upper_limit = req.upper_limit;
    double lower_limit = req.lower_limit;
    double best_bid = req.best_bid;
    double best_ask = req.best_ask;
    uint32_t orders_placed = 0;
    _ctx = ctx;
    _allow_bid = allow_bid;
    _allow_ask = allow_ask;
    if (!ctx)
        return 0;

    // 风控闸门: 净头寸超过 maxPosition → 暂停该合约全部报单 (cancelAll + 不再挂新单)
    // cancelAll 内部获取 _lock (RecursiveSpinLock), 与本函数顶部 _g 同线程递归安全.
    if (req.verdict.halt_quoting) {
        cancelAll(ctx);
        return 0;
    }

    // 风控闸门: 同侧连续成交熔断（按合约独立计数）→ 撤该合约全部报价 + 本轮不挂新单。
    // 暂停期过后 verdict 自动失效, 报价随下一 tick 恢复（无需手动清除状态）。
    if (req.verdict.side_pause_bid || req.verdict.side_pause_ask) {
        cancelAll(ctx);
        return 0;
    }

    // F8: qty 衰减 exp 与 level 无关, 提升到入口每 tick 各算 1 次
    //   (原每 level 各 2 次 std::exp ~25-40ns/次, num_levels=3 时 6 次)
    const double long_decay = (req.strategy.long_util > 0.0) ? std::exp(-_cfg.qty_decay_factor * req.strategy.long_util) : 1.0;
    const double short_decay = (req.strategy.short_util > 0.0) ? std::exp(-_cfg.qty_decay_factor * req.strategy.short_util) : 1.0;

    for (uint32_t i = 0; i < _cfg.num_levels; i++) {
        // 判断是否需要履行做市义务(双边报单)
        bool is_obligation = needObligation(i);

        // Pricing: obligation vs flexible (Phase 3: separated for clarity)
        QuoteResult qr;
        if (is_obligation) {
            qr = computeObligationPrices(i, mid, l0_bid_price, l0_ask_price,
                                          allow_bid, allow_ask,
                                          upper_limit, lower_limit,
                                          best_bid, best_ask,
                                          req.strategy, long_decay, short_decay);
        } else {
            qr = computeFlexiblePrices(i, mid, l0_bid_price, l0_ask_price,
                                        allow_bid, allow_ask,
                                        upper_limit, lower_limit,
                                        best_bid, best_ask,
                                        req.strategy, long_decay, short_decay);
        }

        // 路径选择
        if (_cfg.use_bilateral_quote && i == 0) {
            // 路径A: 做市双边接口 (stra_quote)
            orders_placed += handleBilateralQuote(i, qr, mid, now);
        } else if (is_obligation) {
            // 路径B1: 普通报单 + 做市义务 (先撤残留, 双边下单, 不走 sticky)
            orders_placed += handleObligationQuote(i, qr, mid, now);
        } else {
            // 路径B2: 普通报单 + 自由报价 (sticky, 可单边)
            orders_placed += handleFlexibleQuote(i, qr, mid, now);
        }
    }

    return orders_placed;
}

void FutuQuoter::cancelAll(wtp::IUftStraCtx* ctx)
{
    RecursiveSpinGuard _g(_lock);
    if (!ctx)
        return;

    for (auto& level : _bid_levels) {
        for (uint32_t id : level.order_ids) {
            if (canSendCancel(_tracker, id)) {
                if (_tracker)
                    _tracker->markPendingCancel(id, CancelReason::MANUAL);
                orderApiCall([&] { return ctx->stra_cancel(id); });
            } else {
                WTSLogger::warn("FutuQuoter[{}]: skip stale/pending cancel order {}", _cfg.code, id);
            }
            _order_id_to_level.erase(id);
        }
        level.order_ids.clear();
    }

    for (auto& level : _ask_levels) {
        for (uint32_t id : level.order_ids) {
            if (canSendCancel(_tracker, id)) {
                if (_tracker)
                    _tracker->markPendingCancel(id, CancelReason::MANUAL);
                orderApiCall([&] { return ctx->stra_cancel(id); });
            } else {
                WTSLogger::warn("FutuQuoter[{}]: skip stale/pending cancel order {}", _cfg.code, id);
            }
            _order_id_to_level.erase(id);
        }
        level.order_ids.clear();
    }
}

void FutuQuoter::cancelSide(wtp::IUftStraCtx* ctx, bool cancel_bid)
{
    RecursiveSpinGuard _g(_lock);
    if (!ctx)
        return;

    auto& levels = cancel_bid ? _bid_levels : _ask_levels;
    for (auto& level : levels) {
        for (uint32_t id : level.order_ids) {
            if (canSendCancel(_tracker, id)) {
                if (_tracker)
                    _tracker->markPendingCancel(id, CancelReason::INVENTORY_LIMIT);
                orderApiCall([&] { return ctx->stra_cancel(id); });
            } else {
                WTSLogger::warn("FutuQuoter[{}]: skip stale/pending cancel order {}", _cfg.code, id);
            }
            _order_id_to_level.erase(id);
        }
        level.order_ids.clear();
    }
}

bool FutuQuoter::onScoutFillCancelObligation(wtp::IUftStraCtx* ctx, uint32_t localid)
{
    RecursiveSpinGuard _g(_lock);
    if (!ctx)
        return false;
    if (_cfg.obligation_level >= _bid_levels.size())
        return false; // 防御: 配置校验应已拦截

    auto it = _order_id_to_level.find(localid);
    if (it == _order_id_to_level.end())
        return false;
    const OrderLevelInfo info = it->second;

    // scout = 自由内层 (level < obligation_level, 价格优于义务层);
    // 义务层自身/外层自由单成交不触发 (走通用重挂逻辑)
    if (info.level >= _cfg.obligation_level)
        return false;

    QuoteLevel& ob_lv = info.is_bid ? _bid_levels[_cfg.obligation_level] : _ask_levels[_cfg.obligation_level];
    if (!ob_lv.order_ids.empty()) {
        WTSLogger::info("[SCOUT] {} scout fill (lvl{}, {}) → cancel obligation lvl{} orders (avoid adverse fill)",
                        _cfg.code,
                        info.level,
                        info.is_bid ? "bid" : "ask",
                        _cfg.obligation_level);
        for (uint32_t id : ob_lv.order_ids) {
            if (canSendCancel(_tracker, id)) {
                if (_tracker)
                    _tracker->markPendingCancel(id, CancelReason::MANUAL);
                orderApiCall([&] { return ctx->stra_cancel(id); });
            } else {
                WTSLogger::warn("FutuQuoter[{}]: skip stale/pending cancel order {}", _cfg.code, id);
            }
            _order_id_to_level.erase(id);
        }
        ob_lv.order_ids.clear();
    }
    return true; // 是 scout 单: 调用方跳过通用重挂, 下一 tick 按新价重挂
}

void FutuQuoter::onOrder(uint32_t localid, bool isCanceled, double leftQty, uint32_t uTime_HHMM, uint32_t sec_in_min)
{
    RecursiveSpinGuard _g(_lock);
    // Find level using tracker or linear search
    QuoteLevel* level = nullptr;

    if (_tracker) {
        UnifiedOrderInfo orderInfoBuf;
        if (_tracker->getOrderInfoCopy(localid, orderInfoBuf)) {
            const UnifiedOrderInfo* orderInfo = &orderInfoBuf;
            uint8_t idx = orderInfo->level_index;
            if (orderInfo->isBid() && idx < _bid_levels.size())
                level = &_bid_levels[idx];
            else if (!orderInfo->isBid() && idx < _ask_levels.size())
                level = &_ask_levels[idx];
        }
    }

    if (!level) {
        for (auto& l : _bid_levels) {
            for (uint32_t id : l.order_ids)
                if (id == localid) {
                    level = &l;
                    break;
                }
            if (level)
                break;
        }
        if (!level) {
            for (auto& l : _ask_levels) {
                for (uint32_t id : l.order_ids)
                    if (id == localid) {
                        level = &l;
                        break;
                    }
                if (level)
                    break;
            }
        }
    }

    if (level) {
        if (isCanceled || leftQty == 0) {
            // Remove this order ID from the level
            level->order_ids.erase(std::remove(level->order_ids.begin(), level->order_ids.end(), localid),
                                   level->order_ids.end());
            if (_tracker)
                _tracker->untrackOrder(localid);
            _order_id_to_level.erase(localid);
        } else {
            level->qty = leftQty;
            if (_tracker)
                _tracker->updateOrderQty(localid, leftQty);

            if (_tracker) {
                UnifiedOrderInfo orderInfoBuf;
                const UnifiedOrderInfo* orderInfo =
                    _tracker->getOrderInfoCopy(localid, orderInfoBuf) ? &orderInfoBuf : nullptr;
                if (orderInfo && !orderInfo->isPendingCancel()) {
                    bool should_cancel = false;
                    if (orderInfo->isBid() && !_allow_bid)
                        should_cancel = true;
                    else if (!orderInfo->isBid() && !_allow_ask)
                        should_cancel = true;

                    if (should_cancel && _ctx) {
                        WTSLogger::warn(
                            "[QUOTER] Post-submit cancel: {} order {} entered UnTrd but side blocked, cancelling",
                            _cfg.code,
                            localid);
                        orderApiCall([&] { return _ctx->stra_cancel(localid); });
                        _tracker->markPendingCancel(localid, CancelReason::INVENTORY_LIMIT);
                    }
                }
            }
        }

        // R3 v2: 触发 BilateralStats 更新（uTime_HHMM=0 表示调用方未传时间，跳过；
        // hasSessionInfo()=false 表示 sessinfo 注入失败，统计已 DISABLED）
        if (uTime_HHMM > 0 && _bilateral_stats.hasSessionInfo()) {
            auto snapshot = getValidQuoteSnapshot();
            _bilateral_stats.update(snapshot, uTime_HHMM, sec_in_min);
        }
    }
}

void FutuQuoter::onEntrustAck(uint32_t localid, uint32_t uTime_HHMM, uint32_t sec_in_min)
{
    RecursiveSpinGuard _g(_lock);
    // 引擎确认报单 — 订单状态已在 handle*Quote 注册, 此处只驱动统计.
    // 语义: 报单"在场时间"从引擎确认时刻起算, 不含发出→确认的网络延迟.
    (void)localid;
    if (uTime_HHMM > 0 && _bilateral_stats.hasSessionInfo()) {
        auto snapshot = getValidQuoteSnapshot();
        _bilateral_stats.update(snapshot, uTime_HHMM, sec_in_min);
    }
}

void FutuQuoter::onTrade(uint32_t localid, double vol, double price, uint32_t uTime_HHMM, uint32_t sec_in_min)
{
    RecursiveSpinGuard _g(_lock);
    // 注意: 不在这里调 _tracker->recordFilled() — orders_filled 由
    // untrackOrder(完全成交路径)统一计数, 避免每笔成交 +2 的双重计数.
    // v7.1: 统计更新已移除 — 每笔 on_trade 必伴随 on_order(leftQty) 回调
    //       (mocker UftMocker.cpp:985-988 / 实盘柜台回报同序), onOrder 独立
    //       完成统计, 此处重复更新属于双计.
    (void)localid;
    (void)vol;
    (void)price;
    (void)uTime_HHMM;
    (void)sec_in_min;
}

double FutuQuoter::totalBidQty() const
{
    RecursiveSpinGuard _g(_lock);
    double total = 0;
    for (const auto& level : _bid_levels)
        if (level.hasOrders())
            total += level.qty;
    return total;
}

double FutuQuoter::totalAskQty() const
{
    RecursiveSpinGuard _g(_lock);
    double total = 0;
    for (const auto& level : _ask_levels)
        if (level.hasOrders())
            total += level.qty;
    return total;
}

bool FutuQuoter::isMyOrder(uint32_t localid) const
{
    RecursiveSpinGuard _g(_lock);
    return _order_id_to_level.find(localid) != _order_id_to_level.end();
}

QuoteLevel* FutuQuoter::getLevelByOrder(uint32_t localid)
{
    RecursiveSpinGuard _g(_lock);
    auto it = _order_id_to_level.find(localid);
    if (it != _order_id_to_level.end()) {
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
    RecursiveSpinGuard _g(_lock);
    ValidQuoteSnapshot snapshot;
    snapshot.tick_size = _cfg.tick_size;

    const double target = _cfg.min_valid_qty;
    if (target <= 0)
        return snapshot;

    // Bid 侧累计加权（_bid_levels 已按价格优劣排序，level 0 = 最优 bid）
    {
        double cum_qty = 0;
        double weighted_sum = 0; // Σ(qty_i × price_i)
        for (const auto& level : _bid_levels) {
            if (!level.hasOrders() || level.qty <= 0)
                continue;
            double remain = target - cum_qty;
            if (remain <= 0)
                break;
            double take = (level.qty >= remain) ? remain : level.qty;
            weighted_sum += take * level.price;
            cum_qty += take;
            if (cum_qty >= target)
                break;
        }
        if (cum_qty >= target - 1e-9) {
            snapshot.has_valid_bid = true;
            snapshot.weighted_bid_price = weighted_sum / target;
        }
    }

    // Ask 侧累计加权
    {
        double cum_qty = 0;
        double weighted_sum = 0;
        for (const auto& level : _ask_levels) {
            if (!level.hasOrders() || level.qty <= 0)
                continue;
            double remain = target - cum_qty;
            if (remain <= 0)
                break;
            double take = (level.qty >= remain) ? remain : level.qty;
            weighted_sum += take * level.price;
            cum_qty += take;
            if (cum_qty >= target)
                break;
        }
        if (cum_qty >= target - 1e-9) {
            snapshot.has_valid_ask = true;
            snapshot.weighted_ask_price = weighted_sum / target;
        }
    }

    return snapshot;
}

} // namespace futu
