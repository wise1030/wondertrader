/*!
 * \file ArbExecutionBridge.cpp
 * \brief 套利执行桥实现 (自 UftFutuMmStrategy 搬移, 逻辑零修改)
 */
#include "ArbExecutionBridge.h"
#include "UftFutuMmStrategy.h" // ContractInfo
#include "AsyncArbitrageExecutor.h"
#include "SpreadArbitrageManager.h"
#include "OrderRouter.h"
#include "UnifiedOrderTracker.h"
#include "SelfTradePrevention.h"
#include "FutuPortfolio.h"
#include "FutuRiskMonitor.h"
#include "../../Includes/IUftStraCtx.h"
#include "../../Includes/WTSDataDef.hpp"
#include "../../WTSTools/WTSLogger.h"
#include "../../Share/TimeUtils.hpp"

namespace futu
{

void ArbExecutionBridge::onTick(wtp::IUftStraCtx* ctx, const char* stdCode, wtp::WTSTickData* tick)
{
    RecursiveSpinGuard _g(_lock);
    // v7.1: replay 时钟注入 (stale 对冲清理/时间戳统一基准, 回测可复现)
    {
        uint32_t ad = tick->actiondate();
        uint32_t at = tick->actiontime();
        _now_ms = static_cast<uint64_t>(ad) * 86400000ULL + static_cast<uint64_t>(at / 10000000) * 3600000ULL +
                  static_cast<uint64_t>((at / 100000) % 100) * 60000ULL +
                  static_cast<uint64_t>((at / 1000) % 100) * 1000ULL + (at % 1000);
    }

    if (!_deps.async_arb || !_deps.use_spread_arbitrage)
        return;

    // closeout 活跃平仓期暂停套利 — 新开仓会让 closeout drain (getOrderCount>0) 永不完成,
    // 且 arb 单会被强平单反向成交.
    // V8-A12: 旧条件 isCloseoutTriggered 覆盖 COMPLETED/FAILED 直至下一 session,
    // 期间 pushTick/processPendingOrders/processOrphanLegs 全部冻结 — closeout 失败
    // 回退时裸腿对冲与 in_flight 清理停摆, 敞口持续暴露。改为仅在活跃平仓状态
    // (DRAINING/ASSESSING/EXECUTING) 暂停; COMPLETED(已平) / FAILED(放弃) 后恢复
    // 处理, 孤儿腿对冲可重新保护残余敞口, 新信号仍受自身 B-3/风控闸门约束。
    if (_deps.risk_monitor && _deps.risk_monitor->isCloseoutFlattening())
        return;

    // ============================================================
    // 主线程：快速推送 tick 数据到异步队列（~50ns，非阻塞）
    // ============================================================
    _deps.async_arb->pushTick(stdCode, tick->price(), tick->actiontime());

    // 从 Portfolio(SSOT) 回填套利 pair 仓位 — 使策略退出/止损分支与
    // 风控仓位检查读到真实仓位.
    if (_deps.arb_manager)
        _deps.arb_manager->refreshPositionsFromPortfolio();

    // B5 fix: cleanup stale hedge entries (opposite leg partially filled then cancelled)
    // 超时条件兜底: original_qty>0 的条目 30s 过期, original_qty==0 的条目 2s 过期
    // (撤单无量标记, 对齐"毫秒~秒级"窗口语义). 不能用 hedged_qty < original_qty:
    // original_qty=0 的条目 erase 要求 original_qty>0 永不满足, 会永久泄漏.
    // B1 根治: 新信号订单提交时 (processPendingOrders) 主动清除 original_qty==0 条目,
    // 消除 2s 窗口内新信号成交被错误对冲的风险.
    {
        uint64_t now_ms = _now_ms > 0 ? _now_ms : TimeUtils::getLocalTimeNow();
        for (auto it = _arb_hedge_on_fill.begin(); it != _arb_hedge_on_fill.end();) {
            const uint64_t timeout_ms = (it->second.original_qty > 0) ? 30000 : 2000;
            if (it->second.created_time_ms > 0 && now_ms - it->second.created_time_ms > timeout_ms) {
                WTSLogger::warn("ArbBridge: stale hedge entry expired, pair={}, "
                                "hedged={}/{}",
                                it->first,
                                it->second.hedged_qty,
                                it->second.original_qty);
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
    if (_deps.stp && _deps.order_tracker) {
        uint64_t gen = _deps.order_tracker->getGeneration();
        if (gen != _last_mm_generation) {
            _last_mm_generation = gen;
            _deps.async_arb->updateMMOrders(
                stdCode, _deps.stp->getMMBuyOrders(stdCode), _deps.stp->getMMSellOrders(stdCode));
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
        if (undone > 0) {
            auto it = _arb_last_order_price.find(order.code);
            if (it != _arb_last_order_price.end()) {
                // 如果挂单仍在，且新信号要求的价格和目前挂单价完全一致，
                // 则直接丢弃新信号，避免触发 WT 底层的【自动撤销并重下相同单】机制，
                // 从而保护订单在交易所撮合队列中的排队优先级。
                if (std::abs(it->second - order.price) < 1e-6) {
                    WTSLogger::debug("AsyncArb skipped: {} already has {} pending at identical price {}",
                                     order.code,
                                     undone,
                                     order.price);
                    // B13 fix: drop 时同步释放 B-3 in_flight 闸门,
                    // 否则该 pair 冻结至 60s 超时 (1:N 聚合下多 pair 共享 leg 同价时触发)
                    if (_deps.arb_manager)
                        _deps.arb_manager->onArbSignalDropped(order.pair_id, order.is_close); // V8-A3: 按通道
                    return;
                }
            }
        }

        _arb_last_order_price[order.code] = order.price;

        // 执行订单（通过 OrderRouter）
        if (!_deps.order_router) {
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
        if (order.is_close && _deps.portfolio) {
            double live_pos = _deps.portfolio->getPosition(order.code);
            double signed_qty = order.is_buy ? exe_qty : -exe_qty;
            double predicted = live_pos + signed_qty;
            // C3: 平仓方向与实际仓位同号(含 live_pos==0: MM 已消耗完 / 符号已翻转)时,
            //     "平仓"实为开仓 -> 直接丢弃。原 live_pos*predicted<0 判定不含此情形,
            //     exe_qty<0.5 也不拦 -> 止损/超时平仓单变 5 手裸开仓。
            if (live_pos * signed_qty >= 0) {
                WTSLogger::info("[ARB_CLOSE] {} skip: close dir mismatches live_pos={:.1f} (signed_qty={:.1f}), "
                                "consumed/flipped by MM",
                                order.code,
                                live_pos,
                                signed_qty);
                // V8-A10: skip 路径必须释放 B-3 in_flight (此前直接 return,
                // 止损重试在 5s 内被 B4 防双发抑制, 真实敞口窗口)
                if (!order.pair_id.empty() && _deps.arb_manager)
                    _deps.arb_manager->onArbSignalDropped(order.pair_id, order.is_close); // V8-A3: 按通道
                return;
            }
            if (live_pos * predicted < 0) {
                double clamped = std::abs(live_pos);
                WTSLogger::warn("[ARB_CLOSE] {} clamp qty {:.1f}->{:.1f} avoid overshoot (live_pos={:.1f})",
                                order.code,
                                exe_qty,
                                clamped,
                                live_pos);
                exe_qty = clamped;
            }
            if (exe_qty < 0.5) {
                WTSLogger::info("[ARB_CLOSE] {} skip: live_pos={:.1f} already consumed by MM", order.code, live_pos);
                // V8-A10: 同上, skip 释放 in_flight
                if (!order.pair_id.empty() && _deps.arb_manager)
                    _deps.arb_manager->onArbSignalDropped(order.pair_id, order.is_close); // V8-A3: 按通道
                return;
            }
            if (order.order_flag == 1) // FAK → 对手价
            {
                ContractState cs_buf;
                const ContractState* cs = _deps.portfolio->getContractSnapshot(order.code, cs_buf) ? &cs_buf : nullptr;
                if (cs) {
                    if (order.is_buy && cs->ask1 > 0)
                        exe_price = cs->ask1;
                    else if (!order.is_buy && cs->bid1 > 0)
                        exe_price = cs->bid1;
                }
            }
        }

        OrderSubmitResult router_result;
        if (order.is_buy) {
            router_result = _deps.order_router->submitBuy(
                ctx, order.code.c_str(), exe_price, exe_qty, Source::ARBITRAGE, order.order_flag);
            if (router_result.rejected)
                WTSLogger::warn("AsyncArb BUY {} rejected - invalid price={}", order.code, exe_price);
            else if (!router_result.localids.empty())
                WTSLogger::debug("AsyncArb BUY {} {}@{} via OrderRouter", order.code, exe_qty, exe_price);
        } else {
            router_result = _deps.order_router->submitSell(
                ctx, order.code.c_str(), exe_price, exe_qty, Source::ARBITRAGE, order.order_flag);
            if (router_result.rejected)
                WTSLogger::warn("AsyncArb SELL {} rejected - invalid price={}", order.code, exe_price);
            else if (!router_result.localids.empty())
                WTSLogger::debug("AsyncArb SELL {} {}@{} via OrderRouter", order.code, exe_qty, exe_price);
        }

        // M3: 腿失败兜底 — rejected(价格非法) 或空 localids(引擎下单失败/STP 调价后
        //     price<=0 被 OrderRouter 拒) 与 rate_limited/self_trade_blocked 同为单腿失败:
        //     必须撤对侧 + 标记残腿, 否则已推出的另一条腿裸奔至 in_flight 超时 (60-120s)。
        if ((router_result.rejected || router_result.localids.empty()) && !router_result.rate_limited &&
            !router_result.self_trade_blocked) {
            if (!order.pair_id.empty()) {
                WTSLogger::warn("AsyncArb leg FAILED for pair={} (rejected={}, empty_ids={}), canceling opposite leg",
                                order.pair_id,
                                router_result.rejected,
                                router_result.localids.empty());
                _deps.order_router->cancelByPair(ctx, order.pair_id); // A7
                markLegRejected(order.pair_id, order.qty);
            }
            return;
        }

        // Scheme B-3: tag each returned localid with the pair_id so on_trade can
        // route fills to SpreadArbMgr::onArbOrderFilled (in-flight tracking).
        if (!router_result.localids.empty() && !order.pair_id.empty()) {
            // B1 根治: 新信号订单提交成功 = 旧孤儿腿 hedge-on-fill 条目 (original_qty==0)
            // 已过期. 不清除则 2s 窗口内新 leg1 成交会被 onTradeFill 错误对冲 (旧条目
            // 按全 vol 对冲, 无视这是新信号的开仓而非旧孤儿腿的残余成交).
            // 开仓/平仓均需清除: 平仓信号的新成交也不应触发旧孤儿腿对冲.
            {
                auto hit = _arb_hedge_on_fill.find(order.pair_id);
                if (hit != _arb_hedge_on_fill.end() && hit->second.original_qty <= 0) {
                    WTSLogger::debug("ArbBridge: clearing stale original_qty==0 hedge entry for pair={} "
                                     "(new signal submitted, hedged={})",
                                     order.pair_id,
                                     hit->second.hedged_qty);
                    _arb_hedge_on_fill.erase(hit);
                }
            }
            for (uint32_t lid : router_result.localids) {
                _deps.async_arb->tagOrderPair(lid, order.pair_id);
                _deps.order_router->registerPairOrder(lid, order.pair_id); // A7: cancelByPair 映射
                // A8: 录入 UnifiedOrderTracker (此前 trackArbOrder 全项目无调用者,
                // arb 单不在 tracker → 自成交检查/在途量统计/sticky 对 arb 单全部失效).
                // 录入后现有 recordOrderFill/untrack 链路自动生效 (on_trade/on_order 无条件调用).
                if (_deps.order_tracker) {
                    _deps.order_tracker->trackArbOrder(lid,
                                                       order.code,
                                                       exe_price,
                                                       exe_qty,
                                                       exe_price /*placeMid 近似*/,
                                                       _now_ms > 0 ? _now_ms : TimeUtils::getLocalTimeNow(),
                                                       order.is_buy);
                }
            }
        }

        if (router_result.rate_limited) {
            WTSLogger::warn("AsyncArb order rate limited: {} {}", order.code, order.is_buy ? "BUY" : "SELL");

            // 套利单腿提交失败(流控阻断),撤同 pair_id 已提交的单, 防止裸腿风险
            if (!order.pair_id.empty()) {
                WTSLogger::warn("AsyncArb leg FAILED for pair={}, canceling opposite leg", order.pair_id);
                _deps.order_router->cancelByPair(ctx, order.pair_id); // A7: 原 cancelAllBySource 误撤其它 pair
                // 残腿防护: 若对侧腿已在途成交, 撤单无法挽回 → 标记, onTradeFill 时反向平仓
                markLegRejected(order.pair_id, order.qty);
            }
            return;
        }
        if (router_result.self_trade_blocked) {
            WTSLogger::warn("AsyncArb order self-trade blocked: {} {}", order.code, order.is_buy ? "BUY" : "SELL");

            // STP 阻断 = 单腿失败,撤同 pair_id 已提交的单
            if (!order.pair_id.empty()) {
                WTSLogger::warn("AsyncArb leg STP-BLOCKED for pair={}, canceling opposite leg", order.pair_id);
                _deps.order_router->cancelByPair(ctx, order.pair_id); // A7
                // 残腿防护: 同上
                markLegRejected(order.pair_id, order.qty);
            }
            return;
        }

        // 记录到风险监控
        if (_deps.risk_monitor) {
            _deps.risk_monitor->recordOrder();
        }
    });

    // ============================================================
    // 主线程：处理orphan leg自动对冲
    // ============================================================
    _deps.async_arb->processOrphanLegs(
        // V8-A2: 返回受理状态供 executor 有限重试; 定价只取 leg2 盘口快照
        // (旧 fallback 用 leg1 成交价下 leg2 单, 价格口径错误)
        [this, ctx](const std::string& code, bool is_buy, double qty, bool urgent) -> bool {
            // 从Portfolio获取对手价（对冲方向用对手价确保成交）
            double hedge_price = 0;
            if (_deps.portfolio) {
                ContractState cs_buf;
                const ContractState* cs = _deps.portfolio->getContractSnapshot(code, cs_buf) ? &cs_buf : nullptr;
                if (cs) {
                    // 对冲方向: is_buy → 用ask1买入, !is_buy → 用bid1卖出
                    if (is_buy && cs->ask1 > 0)
                        hedge_price = cs->ask1;
                    else if (!is_buy && cs->bid1 > 0)
                        hedge_price = cs->bid1;
                }
            }

            // urgent时加1个tick确保成交（模拟市价）
            if (urgent) {
                double tick = 0;
                for (const auto& ci : *_deps.contract_infos) {
                    if (ci.code == code) {
                        tick = ci.tick_size;
                        break;
                    }
                }
                if (tick > 0) {
                    hedge_price = is_buy ? hedge_price + tick : hedge_price - tick;
                }
            }

            // 价格保护: hedge_price必须>0
            if (hedge_price <= 0) {
                WTSLogger::warn("OrphanLeg hedge DEFER: {} no valid quote yet, will retry", code);
                return false;
            }

            // 通过OrderRouter下单（Source::HEDGING）
            if (_deps.order_router) {
                OrderSubmitResult result;
                if (is_buy) {
                    result = _deps.order_router->submitBuy(ctx, code.c_str(), hedge_price, qty, Source::HEDGING);
                } else {
                    result = _deps.order_router->submitSell(ctx, code.c_str(), hedge_price, qty, Source::HEDGING);
                }

                if (!result.localids.empty()) {
                    WTSLogger::info("OrphanLeg HEDGE {} {} {}@{} via OrderRouter{}",
                                    is_buy ? "BUY" : "SELL",
                                    code,
                                    qty,
                                    hedge_price,
                                    urgent ? " [URGENT]" : "");
                }
                if (result.rejected) {
                    WTSLogger::warn("OrphanLeg hedge rejected - invalid price: {}", code);
                }
                if (result.rate_limited) {
                    WTSLogger::warn("OrphanLeg hedge rate limited: {}", code);
                }
                if (result.self_trade_blocked) {
                    WTSLogger::warn("OrphanLeg hedge self-trade blocked: {}", code);
                }
                // V8-A2: 拒绝/限流/自成交拦截/未受理 → false 触发 executor 重试
                if (result.rejected || result.rate_limited || result.self_trade_blocked || result.localids.empty())
                    return false;
            } else {
                // V8-R6/P2-5: 删除 ctx 直调 fallback —— 该分支产生完全绕过
                // Router/Tracker 的裸单(无来源标记/无在途统计/无自成交检查),
                // 且 validateDeps 已保证 order_router 非空, 此分支不可达。
                // 保留即地雷: 未来依赖变更后产生账本外订单。fail-fast + 触发重试。
                WTSLogger::error("OrphanLeg hedge DROPPED: order_router null (unreachable, "
                                 "deps validation guarantees non-null) — code={} qty={}",
                                 code,
                                 qty);
                return false;
            }

            // 记录到风险监控
            if (_deps.risk_monitor) {
                _deps.risk_monitor->recordOrder();
            }
            return true;
        },
        // 传入当前组合delta_ratio，用于动态调整对冲超时
        [this]() -> double {
            if (!_deps.portfolio)
                return 0.0;
            return _deps.portfolio->getRawPortfolioDeltaUtilization(); // abs(total_delta)/max_delta (原始口径, 单一定义)
        }());

    // ============================================================
    // in_flight timeout 清理: 撤掉超时 pair 的未成交套利挂单
    // 防止: leg1 成交 + leg2 挂单超时 → in_flight 清零 → 新信号发出
    //       但 leg2 仍在场上 → 可能重复建仓
    // ============================================================
    if (_deps.arb_manager && _deps.order_router) {
        std::vector<std::string> timed_out;
        if (_deps.arb_manager->popTimedOutPairs(timed_out)) {
            for (const auto& pair_id : timed_out) {
                WTSLogger::warn("Arb in_flight timeout cleanup: pair={}, canceling pending arb orders", pair_id);
                _deps.order_router->cancelByPair(ctx, pair_id); // A7: 原 cancelAllBySource 误撤其它 pair
            }
        }

        // B5: 过冲 pair 撤单 (sign-flip 触发, 与超时清理同模式轮询)
        std::vector<std::string> overshoot;
        if (_deps.arb_manager->popOvershootPairs(overshoot)) {
            for (const auto& pair_id : overshoot) {
                WTSLogger::error("Arb OVERSHOOT cleanup: pair={}, canceling pending arb orders", pair_id);
                _deps.order_router->cancelByPair(ctx, pair_id);
            }
        }
    }
}

void ArbExecutionBridge::onTradeFill(
    wtp::IUftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isLong, double vol, double price)
{
    RecursiveSpinGuard _g(_lock);
    if (!_deps.async_arb || !_deps.arb_manager)
        return;

    // Scheme B-3: if this fill is from an arb order, decrement its in_flight tracking.
    std::string arb_pair_id;
    if (_deps.async_arb->consumePairTag(localid, arb_pair_id)) {
        _deps.arb_manager->onArbOrderFilled(arb_pair_id, vol);

        // A3: 残腿对冲 — 分笔成交安全: 按 min(本次vol, 剩余未对冲量) 对冲,
        // 累计覆盖 original_qty 后才 erase (旧实现首笔即 erase, 后续分笔成裸腿).
        auto hedge_it = _arb_hedge_on_fill.find(arb_pair_id);
        if (hedge_it != _arb_hedge_on_fill.end() && _deps.order_router) {
            double remaining = hedge_it->second.original_qty > 0
                                   ? hedge_it->second.original_qty - hedge_it->second.hedged_qty
                                   : vol; // 上限未知(拒单/撤单时无量): 全额对冲本次成交
            double hedge_qty = std::min(vol, remaining);
            if (hedge_qty > 0) {
                ContractState cs_buf;
                const ContractState* cs =
                    (_deps.portfolio && _deps.portfolio->getContractSnapshot(stdCode, cs_buf)) ? &cs_buf : nullptr;
                double hedge_price =
                    isLong ? (cs && cs->bid1 > 0 ? cs->bid1 : price) : (cs && cs->ask1 > 0 ? cs->ask1 : price);
                if (isLong) {
                    // V8-A9: 残腿对冲单此前冒用 Source::CLOSEOUT -- 污染 closeout
                    // 订单统计且意外享受 Fix4 的 REVERSIBLE halt 豁免 (orphan 队列
                    // 路径已正确用 HEDGING, 见 :317-319)
                    auto rr = _deps.order_router->submitSell(ctx, stdCode, hedge_price, hedge_qty, Source::HEDGING, 1);
                    if (rr.rejected)
                        WTSLogger::warn("ORPHAN LEG HEDGE SELL {} rejected - invalid price={}", stdCode, hedge_price);
                } else {
                    auto rr = _deps.order_router->submitBuy(ctx, stdCode, hedge_price, hedge_qty, Source::HEDGING, 1);
                    if (rr.rejected)
                        WTSLogger::warn("ORPHAN LEG HEDGE BUY {} rejected - invalid price={}", stdCode, hedge_price);
                }
                hedge_it->second.hedged_qty += hedge_qty;
                WTSLogger::warn("UftFutuMmStrategy[{}] ORPHAN LEG HEDGE: pair={} {} {} x {} @ {} (cumulative {}/{})",
                                _deps.strategy_id,
                                arb_pair_id,
                                isLong ? "SELL" : "BUY",
                                stdCode,
                                hedge_qty,
                                hedge_price,
                                hedge_it->second.hedged_qty,
                                hedge_it->second.original_qty);
                if (hedge_it->second.original_qty > 0 && hedge_it->second.hedged_qty >= hedge_it->second.original_qty) {
                    _arb_hedge_on_fill.erase(hedge_it);
                }
            }
        }
    }
}

void ArbExecutionBridge::markLegRejected(const std::string& pair_id, double order_qty)
{
    RecursiveSpinGuard _g(_lock);
    auto& st = _arb_hedge_on_fill[pair_id];
    // 多次拒单/撤单: 取最大预期上限; 0(未知) 不覆盖已知上限
    if (st.original_qty <= 0 || order_qty > st.original_qty)
        st.original_qty = order_qty;
    if (st.created_time_ms == 0)
        st.created_time_ms = _now_ms > 0 ? _now_ms : TimeUtils::getLocalTimeNow();
}

void ArbExecutionBridge::onLegCancelled(wtp::IUftStraCtx* ctx, const std::string& pair_id)
{
    RecursiveSpinGuard _g(_lock);
    // A4: 套利腿被撤(超时清理/交易所撤单) = 单腿失败, 与 rate_limited/STP 同语义:
    // 撤对侧在途单(防继续成交扩大裸腿) + 标记残腿防护 + 释放 in_flight.
    // 注意: 撤单时刻已存在的裸腿(对侧已成交部分)无法由本机制回补,
    // 依赖 in_flight timeout 清理与 portfolio 层面对账 (Phase D1 状态机彻底解决).
    WTSLogger::warn("Arb leg CANCELLED for pair={}, cancel opposite + mark hedge-on-fill", pair_id);
    if (_deps.order_router)
        _deps.order_router->cancelByPair(ctx, pair_id); // A7
    markLegRejected(pair_id, 0);
    if (_deps.arb_manager)
        _deps.arb_manager->onArbLegCancelled(pair_id); // V8-A3: 只释放在途通道
}

void ArbExecutionBridge::resetSession()
{
    RecursiveSpinGuard _g(_lock);
    _arb_hedge_on_fill.clear();
    // _arb_last_order_price / _last_mm_generation 有意跨 session 保留:
    // 订单世代号单调递增, 价格去重随新挂单自然覆盖
}

} // namespace futu
