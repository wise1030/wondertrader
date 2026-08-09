/*!
 * \file RiskCoordinator.cpp
 * \brief 风控协调器实现 (从 StrategyCoordinator 拆分, P1.3 Step 2a)
 *
 * 纯迁移: checkTakerReduce 行为与原 coordinator 实现完全一致。
 */
#include "RiskCoordinator.h"
#include "StrategyCoordinator.h"   // CoordinatorConfig / FutuRiskMonitor / RiskLiquidator (transitive)
#include "FutuPortfolio.h"          // ContractState / getAllContractsSnapshot
#include "OrderRouter.h"            // Source / OrderSubmitResult / submitSell/Buy
#include <cmath>
#include "../Share/TimeUtils.hpp"
#include "../WTSTools/WTSLogger.h"

namespace futu {

bool RiskCoordinator::checkTakerReduce(wtp::IUftStraCtx* ctx, uint64_t exchange_time_ms)
{
    if (!ctx || !_deps.portfolio || !_deps.order_router)
        return false;
    if (_deps.cfg->taker_reduce_threshold <= 0.0)
        return false;

    // v7.1: 限频计时统一用 replay 时钟 (回测可复现); 未注入时回退墙钟
    uint64_t now_ms = exchange_time_ms > 0 ? exchange_time_ms : TimeUtils::getLocalTimeNow();
    bool triggered = false;

    for (const auto& c : _deps.portfolio->getAllContractsSnapshot()) {
        if (c.max_position <= 0 || std::abs(c.position) < 1.0)
            continue;

        double util = std::abs(c.position) / c.max_position;
        if (util < _deps.cfg->taker_reduce_threshold)
            continue;

        // 每合约限频
        auto it = _last_taker_reduce.find(c.code);
        if (it != _last_taker_reduce.end() && now_ms - it->second < _deps.cfg->taker_reduce_cooldown_ms) {
            continue;
        }

        // 平掉超出 target×maxPos 的部分 (FAK 对手价, 不追价)
        double target = c.max_position * _deps.cfg->taker_reduce_target_util;
        double qty = std::floor(std::abs(c.position) - target);
        qty = clampReduceQty(qty, c.position);  // P0-2: 统一截断, 不开反向仓
        if (qty < 1.0)
            continue;

        bool is_long = c.position > 0;
        double price = is_long ? c.bid1 : c.ask1; // 对手价
        if (price <= 0)
            continue;

        WTSLogger::warn("[TAKER_REDUCE] {} util={:.2f} >= {:.2f}: {} {:.0f}@{} (pos={:.0f}/{:.0f} -> target={:.0f})",
                        c.code,
                        util,
                        _deps.cfg->taker_reduce_threshold,
                        is_long ? "SELL_CLOSE" : "BUY_CLOSE",
                        qty,
                        price,
                        c.position,
                        c.max_position,
                        target);

        OrderSubmitResult rr = is_long ? _deps.order_router->submitSell(ctx, c.code.c_str(), price, qty, Source::CLOSEOUT, 1)
                                       : _deps.order_router->submitBuy(ctx, c.code.c_str(), price, qty, Source::CLOSEOUT, 1);

        if (rr.rate_limited) {
            WTSLogger::warn("[TAKER_REDUCE] {} rate limited, will retry next cooldown", c.code);
            continue;
        }
        if (rr.self_trade_blocked) {
            WTSLogger::warn("[TAKER_REDUCE] {} self-trade blocked (MM quotes on the way), will retry", c.code);
            continue;
        }

        if (!rr.localids.empty()) {
            _last_taker_reduce[c.code] = now_ms;
            if (_deps.risk_monitor)
                _deps.risk_monitor->recordOrder();
            triggered = true;
        } else {
            WTSLogger::error("[TAKER_REDUCE] {} order FAILED", c.code);
        }
    }

    return triggered;
}

} // namespace futu
