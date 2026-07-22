/*!
 * \file ArbExecutionBridge.cpp
 * \brief 套利执行桥实现 (自 UftFutuMmStrategy 搬移, 逻辑零修改)
 */
#include "ArbExecutionBridge.h"
#include "UftFutuMmStrategy.h"      // ContractInfo
#include "AsyncArbitrageExecutor.h"
#include "SpreadArbitrageManager.h"
#include "OrderRouter.h"
#include "UnifiedOrderTracker.h"
#include "SelfTradePrevention.h"
#include "FutuPortfolio.h"
#include "FutuRiskMonitor.h"
#include "../Includes/IUftStraCtx.h"
#include "../Includes/WTSDataDef.hpp"
#include "../WTSTools/WTSLogger.h"
#include "../Share/TimeUtils.hpp"

namespace futu {

void ArbExecutionBridge::onTick(wtp::IUftStraCtx* ctx, const char* stdCode, wtp::WTSTickData* tick)
{
if (!_deps.async_arb || !_deps.use_spread_arbitrage)
return;

// closeout 期间暂停套利 — 新开仓会让 closeout drain (getOrderCount>0) 永不完成,
// 且 arb 单会被强平单反向成交. isCloseoutTriggered 覆盖 TRIGGERED→COMPLETED/FAILED,
// 直至 next session resetCloseout(IDLE) 自动恢复.
if (_deps.risk_monitor && _deps.risk_monitor->isCloseoutTriggered())
return;

// ============================================================
// 主线程：快速推送 tick 数据到异步队列（~50ns，非阻塞）
// ============================================================
_deps.async_arb->pushTick(stdCode, tick->price(), 1.0, tick->actiontime());

// 从 Portfolio(SSOT) 回填套利 pair 仓位 — 使策略退出/止损分支与
// 风控仓位检查读到真实仓位.
if (_deps.arb_manager)
_deps.arb_manager->refreshPositionsFromPortfolio();

// B5 fix: cleanup stale hedge entries (opposite leg partially filled then cancelled)
// 纯超时条件: 任何条目超过 30s 即视为死亡 (hedge-on-fill 窗口为毫秒~秒级).
// 不能用 hedged_qty < original_qty: original_qty=0 的条目 (onLegCancelled 无量标记)
// 在 onTradeFill 中 erase 要求 original_qty>0 永不满足, 会永久泄漏并导致
// 该 pair 后续所有成交被错误反向对冲.
{
uint64_t now_ms = TimeUtils::getLocalTimeNow();
for (auto it = _arb_hedge_on_fill.begin(); it != _arb_hedge_on_fill.end(); ) {
    if (it->second.created_time_ms > 0 &&
        now_ms - it->second.created_time_ms > 30000)
    {
        WTSLogger::warn("ArbBridge: stale hedge entry expired, pair={}, "
                        "hedged={}/{}", it->first, it->second.hedged_qty, it->second.original_qty);
        it = _arb_hedge_on_fill.erase(it);
    } else {
        ++it;
    }
}
}

// ============================================================
// 主线程：更新 MM 订单状态到异步执行器（用于自成交检测）
// 世代号门控: 订单集(track/untrack)未变化时跳过快照深拷贝.
// ============================================================
if (_deps.stp && _deps.order_tracker)
{
uint64_t gen = _deps.order_tracker->getGeneration();
if (gen != _last_mm_generation)
{
_last_mm_generation = gen;
_deps.async_arb->updateMMOrders(stdCode, 
_deps.stp->getMMBuyOrders(stdCode), 
_deps.stp->getMMSellOrders(stdCode));
}
}

// ============================================================
// 主线程：处理异步执行器返回的订单请求（执行订单）
// ============================================================
_deps.async_arb->processPendingOrders([this, ctx](const ArbOrderRequest& order) {

// ==========================================================
// 【核心风控与防死循环】 检查是否有挂单，并防止同价位无限撤单替换
// ==========================================================
double undone = ctx->stra_get_undone(order.code.c_str());
if (undone > 0)
{
auto it = _arb_last_order_price.find(order.code);
if (it != _arb_last_order_price.end())
{
// 如果挂单仍在，且新信号要求的价格和目前挂单价完全一致，
// 则直接丢弃新信号，避免触发 WT 底层的【自动撤销并重下相同单】机制，
// 从而保护订单在交易所撮合队列中的排队优先级。
if (std::abs(it->second - order.price) < 1e-6)
{
WTSLogger::debug("AsyncArb skipped: {} already has {} pending at identical price {}",
order.code, undone, order.price);
return;
}
}
}

_arb_last_order_price[order.code] = order.price;

// 执行订单（通过 OrderRouter）
if (!_deps.order_router)
{
WTSLogger::error("AsyncArb callback invoked with order_router==nullptr; "
                 "OrderRouter must be initialized before arb is enabled. Dropping order.");
return;
}

//----------------------------------------------------------------------
// B3 第二层 (主线程精判): 平仓单下单前读 Portfolio SSOT 实时校验.
//   1) 事前过冲预估: 成交会导致 sign-flip → clamp 到恰好平仓 (B5 前置)
//   2) 零残留: 仓位已被 MM 消耗完 → 丢弃
//   3) FAK 平仓单用对手价替换 (executor 无盘口; 止损场景确保立即成交)
//----------------------------------------------------------------------
double exe_qty = order.qty;
double exe_price = order.price;
if (order.is_close && _deps.portfolio)
{
    double live_pos = _deps.portfolio->getPosition(order.code);
    double signed_qty = order.is_buy ? exe_qty : -exe_qty;
    double predicted = live_pos + signed_qty;
    if (live_pos * predicted < 0)
    {
        double clamped = std::abs(live_pos);
        WTSLogger::warn("[ARB_CLOSE] {} clamp qty {:.1f}->{:.1f} avoid overshoot (live_pos={:.1f})",
            order.code, exe_qty, clamped, live_pos);
        exe_qty = clamped;
    }
    if (exe_qty < 0.5)
    {
        WTSLogger::info("[ARB_CLOSE] {} skip: live_pos={:.1f} already consumed by MM", order.code, live_pos);
        return;
    }
    if (order.order_flag == 1)  // FAK → 对手价
    {
        const ContractState* cs = _deps.portfolio->getContract(order.code);
        if (cs)
        {
            if (order.is_buy && cs->ask1 > 0) exe_price = cs->ask1;
            else if (!order.is_buy && cs->bid1 > 0) exe_price = cs->bid1;
        }
    }
}

OrderSubmitResult router_result;
if (order.is_buy)
{
router_result = _deps.order_router->submitBuy(ctx, order.code.c_str(), exe_price, exe_qty, Source::ARBITRAGE, order.order_flag);
if (!router_result.localids.empty())
WTSLogger::info("AsyncArb BUY {} {}@{} via OrderRouter", order.code, exe_qty, exe_price);
}
else
{
router_result = _deps.order_router->submitSell(ctx, order.code.c_str(), exe_price, exe_qty, Source::ARBITRAGE, order.order_flag);
if (!router_result.localids.empty())
WTSLogger::info("AsyncArb SELL {} {}@{} via OrderRouter", order.code, exe_qty, exe_price);
}

// Scheme B-3: tag each returned localid with the pair_id so on_trade can
// route fills to SpreadArbMgr::onArbOrderFilled (in-flight tracking).
if (!router_result.localids.empty() && !order.pair_id.empty())
{
    for (uint32_t lid : router_result.localids)
    {
        _deps.async_arb->tagOrderPair(lid, order.pair_id);
        _deps.order_router->registerPairOrder(lid, order.pair_id);  // A7: cancelByPair 映射
        // A8: 录入 UnifiedOrderTracker (此前 trackArbOrder 全项目无调用者,
        // arb 单不在 tracker → 自成交检查/在途量统计/sticky 对 arb 单全部失效).
        // 录入后现有 recordOrderFill/untrack 链路自动生效 (on_trade/on_order 无条件调用).
        if (_deps.order_tracker)
        {
            _deps.order_tracker->trackArbOrder(lid, order.code, exe_price, exe_qty,
                exe_price /*placeMid 近似*/, TimeUtils::getLocalTimeNow(), order.is_buy);
        }
    }
}

if (router_result.rate_limited)
{
WTSLogger::warn("AsyncArb order rate limited: {} {}", order.code, order.is_buy ? "BUY" : "SELL");

// 套利单腿提交失败(流控阻断),撤同 pair_id 已提交的单, 防止裸腿风险
if (!order.pair_id.empty()) {
    WTSLogger::warn("AsyncArb leg FAILED for pair={}, canceling opposite leg", order.pair_id);
    _deps.order_router->cancelByPair(ctx, order.pair_id);  // A7: 原 cancelAllBySource 误撤其它 pair
    // 残腿防护: 若对侧腿已在途成交, 撤单无法挽回 → 标记, onTradeFill 时反向平仓
    markLegRejected(order.pair_id, order.qty);
}
return;
}
if (router_result.self_trade_blocked)
{
WTSLogger::warn("AsyncArb order self-trade blocked: {} {}", order.code, order.is_buy ? "BUY" : "SELL");

// STP 阻断 = 单腿失败,撤同 pair_id 已提交的单
if (!order.pair_id.empty()) {
    WTSLogger::warn("AsyncArb leg STP-BLOCKED for pair={}, canceling opposite leg", order.pair_id);
    _deps.order_router->cancelByPair(ctx, order.pair_id);  // A7
    // 残腿防护: 同上
    markLegRejected(order.pair_id, order.qty);
}
return;
}

// 记录到风险监控
if (_deps.risk_monitor)
{
_deps.risk_monitor->recordOrder();
}
});

// ============================================================
// 主线程：处理orphan leg自动对冲
// ============================================================
_deps.async_arb->processOrphanLegs([this, ctx](const std::string& code,
                                            bool is_buy,
                                            double price,
                                            double qty,
                                            bool urgent) {
    // 从Portfolio获取对手价（对冲方向用对手价确保成交）
    double hedge_price = price;  // fallback
    if (_deps.portfolio)
    {
        const ContractState* cs = _deps.portfolio->getContract(code);
        if (cs)
        {
            // 对冲方向: is_buy → 用ask1买入, !is_buy → 用bid1卖出
            if (is_buy && cs->ask1 > 0)
                hedge_price = cs->ask1;
            else if (!is_buy && cs->bid1 > 0)
                hedge_price = cs->bid1;
        }
    }

    // urgent时加1个tick确保成交（模拟市价）
    if (urgent)
    {
        double tick = 0;
        for (const auto& ci : *_deps.contract_infos)
        {
            if (ci.code == code) { tick = ci.tick_size; break; }
        }
        if (tick > 0)
        {
            hedge_price = is_buy ? hedge_price + tick : hedge_price - tick;
        }
    }

    // 价格保护: hedge_price必须>0
    if (hedge_price <= 0)
    {
        WTSLogger::error("OrphanLeg hedge ABORTED: {} price=0, no market data yet", code);
        return;
    }

    // 通过OrderRouter下单（Source::HEDGING）
    if (_deps.order_router)
    {
        OrderSubmitResult result;
        if (is_buy)
        {
            result = _deps.order_router->submitBuy(ctx, code.c_str(), hedge_price,
                                                   qty, Source::HEDGING);
        }
        else
        {
            result = _deps.order_router->submitSell(ctx, code.c_str(), hedge_price,
                                                    qty, Source::HEDGING);
        }

        if (!result.localids.empty())
        {
            WTSLogger::info("OrphanLeg HEDGE {} {} {}@{} via OrderRouter{}",
                is_buy ? "BUY" : "SELL", code, qty, hedge_price,
                urgent ? " [URGENT]" : "");
        }
        if (result.rate_limited)
        {
            WTSLogger::warn("OrphanLeg hedge rate limited: {}", code);
        }
        if (result.self_trade_blocked)
        {
            WTSLogger::warn("OrphanLeg hedge self-trade blocked: {}", code);
        }
    }
    else
    {
        // Fallback: 直接调ctx API
        if (is_buy)
        {
            ctx->stra_enter_long(code.c_str(), hedge_price, qty);
        }
        else
        {
            ctx->stra_enter_short(code.c_str(), hedge_price, qty);
        }
        WTSLogger::info("OrphanLeg HEDGE {} {} {}@{} via ctx{}",
            is_buy ? "BUY" : "SELL", code, qty, hedge_price,
            urgent ? " [URGENT]" : "");
    }

    // 记录到风险监控
    if (_deps.risk_monitor)
    {
        _deps.risk_monitor->recordOrder();
    }
},
// 传入当前组合delta_ratio，用于动态调整对冲超时
[this]() -> double {
    if (!_deps.portfolio) return 0.0;
    return _deps.portfolio->getPortfolioDeltaUtilization();  // abs(net_delta)/max_delta
}());

// ============================================================
// in_flight timeout 清理: 撤掉超时 pair 的未成交套利挂单
// 防止: leg1 成交 + leg2 挂单超时 → in_flight 清零 → 新信号发出
//       但 leg2 仍在场上 → 可能重复建仓
// ============================================================
if (_deps.arb_manager && _deps.order_router)
{
    std::vector<std::string> timed_out;
    if (_deps.arb_manager->popTimedOutPairs(timed_out))
    {
        for (const auto& pair_id : timed_out)
        {
            WTSLogger::warn("Arb in_flight timeout cleanup: pair={}, canceling pending arb orders",
                pair_id);
            _deps.order_router->cancelByPair(ctx, pair_id);  // A7: 原 cancelAllBySource 误撤其它 pair
        }
    }

    // B5: 过冲 pair 撤单 (sign-flip 触发, 与超时清理同模式轮询)
    std::vector<std::string> overshoot;
    if (_deps.arb_manager->popOvershootPairs(overshoot))
    {
        for (const auto& pair_id : overshoot)
        {
            WTSLogger::error("Arb OVERSHOOT cleanup: pair={}, canceling pending arb orders", pair_id);
            _deps.order_router->cancelByPair(ctx, pair_id);
        }
    }
}
}

void ArbExecutionBridge::onTradeFill(wtp::IUftStraCtx* ctx, uint32_t localid, const char* stdCode,
                                      bool isLong, double vol, double price)
{
if (!_deps.async_arb || !_deps.arb_manager)
    return;

// Scheme B-3: if this fill is from an arb order, decrement its in_flight tracking.
std::string arb_pair_id;
if (_deps.async_arb->consumePairTag(localid, arb_pair_id))
{
    _deps.arb_manager->onArbOrderFilled(arb_pair_id, vol);

    // A3: 残腿对冲 — 分笔成交安全: 按 min(本次vol, 剩余未对冲量) 对冲,
    // 累计覆盖 original_qty 后才 erase (旧实现首笔即 erase, 后续分笔成裸腿).
    auto hedge_it = _arb_hedge_on_fill.find(arb_pair_id);
    if (hedge_it != _arb_hedge_on_fill.end() && _deps.order_router)
    {
        double remaining = hedge_it->second.original_qty > 0
            ? hedge_it->second.original_qty - hedge_it->second.hedged_qty
            : vol;  // 上限未知(拒单/撤单时无量): 全额对冲本次成交
        double hedge_qty = std::min(vol, remaining);
        if (hedge_qty > 0)
        {
            const ContractState* cs = _deps.portfolio ? _deps.portfolio->getContract(stdCode) : nullptr;
            double hedge_price = isLong
                ? (cs && cs->bid1 > 0 ? cs->bid1 : price)
                : (cs && cs->ask1 > 0 ? cs->ask1 : price);
            if (isLong)
                _deps.order_router->submitSell(ctx, stdCode, hedge_price, hedge_qty, Source::CLOSEOUT, 1);
            else
                _deps.order_router->submitBuy(ctx, stdCode, hedge_price, hedge_qty, Source::CLOSEOUT, 1);
            hedge_it->second.hedged_qty += hedge_qty;
            WTSLogger::warn("UftFutuMmStrategy[{}] ORPHAN LEG HEDGE: pair={} {} {} x {} @ {} (cumulative {}/{})",
                _deps.strategy_id, arb_pair_id, isLong ? "SELL" : "BUY", stdCode, hedge_qty, hedge_price,
                hedge_it->second.hedged_qty, hedge_it->second.original_qty);
            if (hedge_it->second.original_qty > 0 &&
                hedge_it->second.hedged_qty >= hedge_it->second.original_qty)
            {
                _arb_hedge_on_fill.erase(hedge_it);
            }
        }
    }
}
}

void ArbExecutionBridge::markLegRejected(const std::string& pair_id, double order_qty)
{
    auto& st = _arb_hedge_on_fill[pair_id];
    // 多次拒单/撤单: 取最大预期上限; 0(未知) 不覆盖已知上限
    if (st.original_qty <= 0 || order_qty > st.original_qty)
        st.original_qty = order_qty;
    if (st.created_time_ms == 0)
        st.created_time_ms = TimeUtils::getLocalTimeNow();
}

void ArbExecutionBridge::onLegCancelled(wtp::IUftStraCtx* ctx, const std::string& pair_id)
{
    // A4: 套利腿被撤(超时清理/交易所撤单) = 单腿失败, 与 rate_limited/STP 同语义:
    // 撤对侧在途单(防继续成交扩大裸腿) + 标记残腿防护 + 释放 in_flight.
    // 注意: 撤单时刻已存在的裸腿(对侧已成交部分)无法由本机制回补,
    // 依赖 in_flight timeout 清理与 portfolio 层面对账 (Phase D1 状态机彻底解决).
    WTSLogger::warn("Arb leg CANCELLED for pair={}, cancel opposite + mark hedge-on-fill", pair_id);
    if (_deps.order_router)
        _deps.order_router->cancelByPair(ctx, pair_id);  // A7
    markLegRejected(pair_id, 0);
    if (_deps.arb_manager)
        _deps.arb_manager->onArbSignalDropped(pair_id);
}

void ArbExecutionBridge::resetSession()
{
    _arb_hedge_on_fill.clear();
    // _arb_last_order_price / _last_mm_generation 有意跨 session 保留:
    // 订单世代号单调递增, 价格去重随新挂单自然覆盖
}

} // namespace futu
