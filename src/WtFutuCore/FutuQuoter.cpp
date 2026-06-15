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

    // v3 软风控：是否走新路径（仅当 use_bilateral_quote=false 时启用）
    const bool v3_mode = !_cfg.use_bilateral_quote;

    for (uint32_t i = 0; i < _cfg.num_levels; i++)
    {
        bool is_bilateral = (_cfg.use_bilateral_quote && i == 0);
        QuoteLevel& bid_level = _bid_levels[i];
        QuoteLevel& ask_level = _ask_levels[i];
        
        // 1. 计算原始报价和挂单量 (统一使用 SpreadOptimizer 定价逻辑)
        double level_offset = i * _cfg.level_step * _cfg.tick_size;
        double bidPrice = floor((l0_bid_price - level_offset) / _cfg.tick_size) * _cfg.tick_size;
        double askPrice = ceil((l0_ask_price + level_offset) / _cfg.tick_size) * _cfg.tick_size;
        double bidQty = computeQty(i);
        double askQty = computeQty(i);
        
        // 2. 报价预处理与风控限制
        bool is_obligation_bid = false;
        bool is_obligation_ask = false;

        if (is_bilateral)
        {
            // ===== 旧路径（use_bilateral_quote=true）：硬 allow + 双边义务，原样保留 =====
            if (!allow_bid && _cfg.max_obligation_spread > 0) {
                bidPrice = floor((mid - _cfg.max_obligation_spread * _cfg.tick_size) / _cfg.tick_size) * _cfg.tick_size;
                is_obligation_bid = true;
            }
            if (!allow_ask && _cfg.max_obligation_spread > 0) {
                askPrice = ceil((mid + _cfg.max_obligation_spread * _cfg.tick_size) / _cfg.tick_size) * _cfg.tick_size;
                is_obligation_ask = true;
            }
        }
        else if (v3_mode)
        {
            // ===== v3 新路径：utilization → qty 指数衰减 + 软 obligation =====
            // 2a. qty 指数衰减（仅 L0 强势衰减，深档保留 baseline；这里对所有层级一致衰减，更保守）
            //     bidQty 受 long_util 影响（多头打满 → 减少 bid）
            //     askQty 受 short_util 影响（空头打满 → 减少 ask）
            if (long_util > 0.0) {
                double decay = std::exp(-_cfg.qty_decay_factor * long_util);
                bidQty = std::max(0.0, std::round(bidQty * decay));
            }
            if (short_util > 0.0) {
                double decay = std::exp(-_cfg.qty_decay_factor * short_util);
                askQty = std::max(0.0, std::round(askQty * decay));
            }
            
            // 2b. 软 obligation：达到 hard cap 时强制反向减仓报价（仅 L0 或全档）
            const bool apply_obligation = (!_cfg.obligation_only_l0 || i == 0);
            if (force_ask_obligation && apply_obligation) {
                // 多头打满 → 必须挂 ask 让自己被减仓
                askPrice = ceil((mid + _cfg.obligation_max_spread_ticks * _cfg.tick_size) / _cfg.tick_size) * _cfg.tick_size;
                askQty = std::max(computeQty(0), _cfg.obligation_min_qty);
                is_obligation_ask = true;
            }
            if (force_bid_obligation && apply_obligation) {
                // 空头打满 → 必须挂 bid 让自己被减仓
                bidPrice = floor((mid - _cfg.obligation_max_spread_ticks * _cfg.tick_size) / _cfg.tick_size) * _cfg.tick_size;
                bidQty = std::max(computeQty(0), _cfg.obligation_min_qty);
                is_obligation_bid = true;
            }
        }

        // 强制风控：如果不允许且不是义务报价，则数量置 0
        // v3 模式下 allow_bid/allow_ask 仍保留兼容（Toxicity/TradingState 可关闭）
        if (!allow_bid && !is_obligation_bid) bidQty = 0;
        if (!allow_ask && !is_obligation_ask) askQty = 0;

        // 3. 价格保护 (不对义务报价应用)
        if (_cfg.price_protection)
        {
            if (bidQty > 0 && !is_obligation_bid && best_bid > 0)
                bidPrice = std::min(bidPrice, best_bid + _cfg.protect_ticks * _cfg.tick_size);
                
            if (askQty > 0 && !is_obligation_ask && best_ask > 0)
                askPrice = std::max(askPrice, best_ask - _cfg.protect_ticks * _cfg.tick_size);
        }

        // 4. 价格边界验证 (涨跌停)
        if (bidQty > 0 && !validatePrice(bidPrice, mid, upper_limit, lower_limit)) bidQty = 0;
        if (askQty > 0 && !validatePrice(askPrice, mid, upper_limit, lower_limit)) askQty = 0;

        // 5. 不对称粘性阈值判断 (Sticky Logic)
        bool bid_need_update = false;
        if (bidQty > 0)
        {
            if (bid_level.order_id == 0) {
                bid_need_update = true;
            } else if (_tracker) {
                auto* orderInfo = _tracker->getOrderByOrderId(bid_level.order_id);
                if (orderInfo && !orderInfo->isPendingCancel()) {
                    bid_need_update = checkStickyUpdate(bidPrice, orderInfo->price, true);
                } else if (!orderInfo) {
                    bid_need_update = checkStickyUpdate(bidPrice, bid_level.price, true);
                }
            } else {
                bid_need_update = checkStickyUpdate(bidPrice, bid_level.price, true);
            }
        }

        bool ask_need_update = false;
        if (askQty > 0)
        {
            if (ask_level.order_id == 0) {
                ask_need_update = true;
            } else if (_tracker) {
                auto* orderInfo = _tracker->getOrderByOrderId(ask_level.order_id);
                if (orderInfo && !orderInfo->isPendingCancel()) {
                    ask_need_update = checkStickyUpdate(askPrice, orderInfo->price, false);
                } else if (!orderInfo) {
                    ask_need_update = checkStickyUpdate(askPrice, ask_level.price, false);
                }
            } else {
                ask_need_update = checkStickyUpdate(askPrice, ask_level.price, false);
            }
        }

        // 6. 执行报单更新
        if (is_bilateral)
        {
            // 双边报价逻辑：单侧被BLOCK时保留减仓侧独立报价，避免仓位死锁
            if (bidQty == 0 && askQty == 0)
            {
                // 双侧都被BLOCK，撤销双边
                if (bid_level.order_id != 0) {
                    ctx->stra_cancel(bid_level.order_id);
                    if (_tracker) _tracker->markPendingCancel(bid_level.order_id, CancelReason::INVENTORY_LIMIT);
                    bid_level.order_id = 0;
                }
                if (ask_level.order_id != 0) {
                    ctx->stra_cancel(ask_level.order_id);
                    if (_tracker) _tracker->markPendingCancel(ask_level.order_id, CancelReason::INVENTORY_LIMIT);
                    ask_level.order_id = 0;
                }
            }
            else if (bidQty == 0 || askQty == 0)
            {
                // 单侧被BLOCK：撤销被BLOCK侧，保留有量侧独立报价
                if (bidQty == 0 && bid_level.order_id != 0) {
                    ctx->stra_cancel(bid_level.order_id);
                    if (_tracker) _tracker->markPendingCancel(bid_level.order_id, CancelReason::INVENTORY_LIMIT);
                    bid_level.order_id = 0;
                }
                if (askQty == 0 && ask_level.order_id != 0) {
                    ctx->stra_cancel(ask_level.order_id);
                    if (_tracker) _tracker->markPendingCancel(ask_level.order_id, CancelReason::INVENTORY_LIMIT);
                    ask_level.order_id = 0;
                }
                // 有量侧独立下单（减仓方向）
                if (bidQty > 0 && bid_need_update) {
                    if (bid_level.order_id != 0) {
                        if (_tracker) _tracker->markPendingCancel(bid_level.order_id, CancelReason::MANUAL);
                        bid_level.order_id = 0;
                    }
                    auto ids = ctx->stra_buy(_cfg.code.c_str(), bidPrice, bidQty);
                    if (!ids.empty()) {
                        uint32_t bidId = ids[0];
                        bid_level.order_id = bidId; bid_level.price = bidPrice; bid_level.qty = bidQty;
                        // Store level+is_bid direction
                        _order_id_to_level[bidId] = {i, true};
                        if (_tracker && now > 0) _tracker->trackMMOrder(bidId, i, _cfg.code, bidPrice, bidQty, mid, now, true);
                        orders_placed += 1;
                    }
                }
                if (askQty > 0 && ask_need_update) {
                    if (ask_level.order_id != 0) {
                        if (_tracker) _tracker->markPendingCancel(ask_level.order_id, CancelReason::MANUAL);
                        ask_level.order_id = 0;
                    }
                    auto ids = ctx->stra_sell(_cfg.code.c_str(), askPrice, askQty);
                    if (!ids.empty()) {
                        uint32_t askId = ids[0];
                        ask_level.order_id = askId; ask_level.price = askPrice; ask_level.qty = askQty;
                        // Store level+is_bid direction
                        _order_id_to_level[askId] = {i, false};
                        if (_tracker && now > 0) _tracker->trackMMOrder(askId, i, _cfg.code, askPrice, askQty, mid, now, false);
                        orders_placed += 1;
                    }
                }
            }
            else if (bid_need_update || ask_need_update)
            {
                // Bilateral Replace (顶单) 功能: 
                // 新发出的双边报价会自动清理上一次的双边未成挂单。
                // 策略层无需调用 stra_cancel，仅需解除旧订单在本地 Tracker 的状态。
                // [关键修复]：不要立即 untrackOrder 和 erase，这会导致在途成交丢失上下文。
                // 而是标记为 PENDING_CANCEL，等待真实的 onOrder 回报来执行物理清理。
                if (bid_level.order_id != 0) {
                    if (_tracker) _tracker->markPendingCancel(bid_level.order_id, CancelReason::MANUAL);
                    bid_level.order_id = 0; // 解绑 Level，腾出空位接收新订单
                }
                if (ask_level.order_id != 0) {
                    if (_tracker) _tracker->markPendingCancel(ask_level.order_id, CancelReason::MANUAL);
                    ask_level.order_id = 0;
                }

                auto [bidId, askId] = ctx->stra_quote(_cfg.code.c_str(), bidPrice, bidQty, askPrice, askQty, 
                                                      (allow_bid && allow_ask) ? "MM_BILATERAL" : "MM_OBLIGATION");
                if (bidId != 0 && askId != 0)
                {
                    bid_level.order_id = bidId; bid_level.price = bidPrice; bid_level.qty = bidQty;
                    ask_level.order_id = askId; ask_level.price = askPrice; ask_level.qty = askQty;
                    // Store level+is_bid direction
                    _order_id_to_level[bidId] = {i, true}; _order_id_to_level[askId] = {i, false};
                    if (_tracker && now > 0) {
                        _tracker->trackMMOrder(bidId, i, _cfg.code, bidPrice, bidQty, mid, now, true);
                        _tracker->trackMMOrder(askId, i, _cfg.code, askPrice, askQty, mid, now, false);
                    }
                    orders_placed += 2;
                }
            }
        }
        else
        {
            // 普通接口：深度档 (Level 1+) 独立报单
            // 处理独立买单
            if (bidQty == 0)
            {
                if (bid_level.order_id != 0) {
                    ctx->stra_cancel(bid_level.order_id);
                    if (_tracker) _tracker->markPendingCancel(bid_level.order_id, CancelReason::INVENTORY_LIMIT);
                    bid_level.order_id = 0;
                }
            }
            else if (bid_need_update)
            {
                if (bid_level.order_id != 0) {
                    ctx->stra_cancel(bid_level.order_id);
                    if (_tracker) _tracker->markPendingCancel(bid_level.order_id, CancelReason::PRICE_DEVIATION);
                    bid_level.order_id = 0;
                }
                auto ids = ctx->stra_buy(_cfg.code.c_str(), bidPrice, bidQty);
                if (!ids.empty()) {
                    uint32_t bidId = ids[0];
                    bid_level.order_id = bidId; bid_level.price = bidPrice; bid_level.qty = bidQty;
                    // Store level+is_bid direction
                    _order_id_to_level[bidId] = {i, true};
                    if (_tracker && now > 0) _tracker->trackMMOrder(bidId, i, _cfg.code, bidPrice, bidQty, mid, now, true);
                    orders_placed += 1;
                }
            }

            // 处理独立卖单
            if (askQty == 0)
            {
                if (ask_level.order_id != 0) {
                    ctx->stra_cancel(ask_level.order_id);
                    if (_tracker) _tracker->markPendingCancel(ask_level.order_id, CancelReason::INVENTORY_LIMIT);
                    ask_level.order_id = 0;
                }
            }
            else if (ask_need_update)
            {
                if (ask_level.order_id != 0) {
                    ctx->stra_cancel(ask_level.order_id);
                    if (_tracker) _tracker->markPendingCancel(ask_level.order_id, CancelReason::PRICE_DEVIATION);
                    ask_level.order_id = 0;
                }
                auto ids = ctx->stra_sell(_cfg.code.c_str(), askPrice, askQty);
                if (!ids.empty()) {
                    uint32_t askId = ids[0];
                    ask_level.order_id = askId; ask_level.price = askPrice; ask_level.qty = askQty;
                    // Store level+is_bid direction
                    _order_id_to_level[askId] = {i, false};
                    if (_tracker && now > 0) _tracker->trackMMOrder(askId, i, _cfg.code, askPrice, askQty, mid, now, false);
                    orders_placed += 1;
                }
            }
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
