/*!
 * \file FutuRuntimeOps.cpp
 * \brief 5A-3 (v7.5): 运行时事件处理外移 — on_trade / on_channel_ready
 *
 * 策略壳瘦身第二波: 成交处理 (组合记账/恢复四道闸/arb复活/毒性/统计)
 * 与通道恢复序列 (持仓同步/风控恢复/AUTO REDUCE) 从 UftFutuMmStrategy 剥离。
 * friend + 引用别名方案: 函数体与原实现逐行一致, 锁在策略壳内持有。
 */
#include "FutuRuntimeOps.h"
#include "UftFutuMmStrategy.h"
#include "../Includes/IUftStraCtx.h"
#include "../WTSTools/WTSLogger.h"
#include "../Share/TimeUtils.hpp"

#include "FutuPortfolio.h"
#include "FutuQuoter.h"
#include "FutuRiskMonitor.h"
#include "UnifiedOrderTracker.h"
#include "PerformanceMonitor.h"
#include "PerformanceAnalyzer.h"
#include "SelfTradeCalibrator.h"
#include "ToxicFlowDetector.h"
#include "SignalAggregator.h"
#include "StrategyCoordinator.h"
#include "OrderRouter.h"  // B6: cancelByPair + IOrderSink upcast
#include "AsyncArbitrageExecutor.h"
#include "RiskLiquidator.h"
#include "MonitorBridge.h"
#include "TradingState.h"

namespace futu
{

void FutuRuntimeOps::processTradeFill(UftFutuMmStrategy& s,
                                      wtp::IUftStraCtx* ctx,
                                      uint32_t localid,
                                      const char* stdCode,
                                      bool isLong,
                                      uint32_t offset,
                                      double vol,
                                      double price)
{
    // 引用别名: 绑定策略成员, 保持原 on_trade 函数体不变
    auto& _risk_monitor = s._risk_monitor;
    auto& _portfolio = s._portfolio;
    auto& _portfolio_ctx_dirty = s._portfolio_ctx_dirty;
    auto& _arb_bridge = s._arb_bridge;
    auto& _quoters = s._quoters;
    auto& _coordinator = s._coordinator;
    auto& _is_backtest = s._is_backtest;
    auto& _exchange_time_ms = s._exchange_time_ms;
    auto& _config = s._config;
    auto& _order_tracker = s._order_tracker;
    auto& _performance_monitor = s._performance_monitor;
    auto& _perf_analyzer = s._perf_analyzer;
    auto& _last_mid = s._last_mid;
    auto& _signal_aggregators = s._signal_aggregators;
    auto& _self_trade_calibrator = s._self_trade_calibrator;
    auto& _toxicity_detector = s._toxicity_detector;
    auto& _order_error_count = s._order_error_count;
    auto& _trading_state = s._trading_state;
    auto& _violations_buf = s._violations_buf;
    auto& _blocked_contracts = s._blocked_contracts;
    auto& _async_arb = s._async_arb;
    auto& _mon_bridge = s._mon_bridge;

    // 更新频率统计
    if (_risk_monitor)
        _risk_monitor->recordTrade();

    // 风控: 同侧连续成交熔断（按合约独立计数, 与报价路径经 PreTradeDecision 联动）。
    // 某合约同侧连续成交达阈值 -> 立即撤该合约全部报价, 暂停期内 refreshQuotes/
    // requoteAfterFill 均不再挂单, 到期自动恢复。CLOSEOUT 阶段豁免
    // （收盘减仓由 CloseoutExecutor 自行限速, 避免暂停循环）。
    if (_risk_monitor && !_trading_state.isCloseoutActive() &&
        _risk_monitor->onSideFill(stdCode, isLong, _exchange_time_ms)) {
        auto qit = _quoters.find(stdCode);
        if (qit != _quoters.end() && qit->second)
            qit->second->cancelAll(ctx);
        WTSLogger::warn("[RISK] {} SIDE_FILL_BREAKER: {} consecutive same-side {} fills -> cancel all + pause quoting",
                        stdCode,
                        _risk_monitor->getRateLimits().max_consecutive_same_side,
                        isLong ? "buy" : "sell");
    }

    // 使用策略本地持仓（而不是账户持仓）
    // 注意：一个账户可能有多个策略，每个策略只管理自己的持仓
    // stra_get_local_position 返回的是本策略的净头寸
    double local_net = ctx->stra_get_local_position(stdCode);

    // P0-1/v7.1: 分向成本簿记账 (offset 标志驱动, 替代净额推断)
    // 旧净额推断在 MM+arb 共享同合约净头寸时必然失真:
    //   arb 腿的开平被误判为 MM 加减仓, avg_cost 被污染 → daily_pnl 假阳性日亏
    //   (回测实证: 引擎真账 +34k, 内部账 -3.7M → 日亏 IRREVERSIBLE halt 误杀)
    {
        _portfolio->onTradeFill(stdCode, isLong, static_cast<int>(offset), vol, price);
        _portfolio_ctx_dirty = true;

        // 净持仓以引擎真值校验 (防漏单/异常路径漂移)
        ContractState cs_chk_buf;
        const ContractState* cs_chk = _portfolio->getContractSnapshot(stdCode, cs_chk_buf) ? &cs_chk_buf : nullptr;
        if (cs_chk && std::abs(cs_chk->position - local_net) > 0.01) {
            WTSLogger::warn("UftFutuMmStrategy[{}] position book divergence: {} book={:.0f} engine={:.0f}, resyncing",
                            s.id(),
                            stdCode,
                            cs_chk->position,
                            local_net);
            _portfolio->resyncPosition(stdCode, local_net);
        }
    }

    // 套利单成交处理 (in_flight 递减 + 残腿对冲, 已拆分至 ArbExecutionBridge, C4)
    _arb_bridge.onTradeFill(ctx, localid, stdCode, isLong, vol, price);

    // 更新 Quoter 订单状态
    // R3 v2: onTrade 补时间参数(UFT: stra_get_time=HHMM, stra_get_secs=SSmmm)
    // v7.2 scout: 自由内层成交 → 撤同侧义务层 (scout_filled 时跳过下方通用重挂,
    //   不以旧价立即重挂义务单 — 规避逆向成交正是设计意图; 下一 tick 按新价重挂)
    bool scout_filled = false;
    {
        uint32_t uTime_HHMM = ctx->stra_get_time();
        uint32_t sec_in_min = ctx->stra_get_secs() / 1000;
        for (auto& [code, quoter] : _quoters) {
            if (quoter->isMyOrder(localid)) {
                quoter->onTrade(localid, vol, price, uTime_HHMM, sec_in_min);
                scout_filled = quoter->onScoutFillCancelObligation(ctx, localid);
                break;
            }
        }
    }

    // v7.1: 单边成交侵蚀挂单深度 → 不满足双边做市义务时, 恢复
    // 回测: 条件式撤单 — 仅义务深度被破坏才 cancelAll (仅 stra_cancel, postTask 安全),
    //   深度仍满足则保留 (黏性受益)。不发 stra_buy/sell, 避免回调内迭代 _orders。
    //   重挂由下一 tick processQuoting 完成 (B1 条件式重挂)。
    // 生产: 同步 requoteAfterFill (stra_buy/sell 发往交易所, 无 _orders 迭代问题)
    if (_coordinator && !scout_filled) {
        if (_is_backtest) {
            auto qit = _quoters.find(stdCode);
            if (qit != _quoters.end()) {
                auto snap = qit->second->getValidQuoteSnapshot();
                if (!(snap.has_valid_bid && snap.has_valid_ask))
                    qit->second->cancelAll(ctx);
            }
        } else {
            _coordinator->requoteAfterFill(ctx, stdCode, _exchange_time_ms);
        }
    }

    // 更新 SpreadOptimizer 成交统计 (P0-2.1: Legacy onFill removed)
    if (_config.modules.use_spread_optimizer) {
        // NO-OP
    }

    // 从共享订单跟踪器中移除 — 仅完全成交时才 untrack。
    // 旧代码无条件 untrack: 部分成交后残留活单从真相源消失,
    // 导致自成交检查绕过/在途量低估/sticky 失效.
    if (_order_tracker && _order_tracker->recordOrderFill(localid, vol)) {
        // P0: quote→fill 延迟埋点 (place_time 为 replay ms 基准, 与 _exchange_time_ms 一致)
        if (_performance_monitor) {
            UnifiedOrderInfo oi_buf;
            if (_order_tracker->getOrderInfoCopy(localid, oi_buf) && oi_buf.place_time > 0 &&
                _exchange_time_ms >= oi_buf.place_time)
                _performance_monitor->recordQuoteToFill((_exchange_time_ms - oi_buf.place_time) * 1000000ULL);
        }
        _order_tracker->untrackOrder(localid);
    }
    // 记录到绩效分析器
    if (_perf_analyzer) {
        TradeRecord trade;
        trade.code = stdCode;
        trade.is_buy = isLong;
        trade.qty = vol;
        trade.price = price;
        trade.timestamp = ctx->stra_get_date() * 1000000ULL + ctx->stra_get_time() * 100ULL + ctx->stra_get_secs();
        auto mid_it = _last_mid.find(stdCode);
        if (mid_it != _last_mid.end()) {
            trade.mid_at_trade = mid_it->second->v.load(std::memory_order_acquire);
            // 计算价差和穿越状态
            ContractState cs_buf;
            const ContractState* cs = _portfolio->getContractSnapshot(stdCode, cs_buf) ? &cs_buf : nullptr;
            trade.spread_at_trade = cs ? cs->tick_size * 2.0 : 0.2; // 与SelfTradeCalibrator一致
            // 判断是否穿越价差：买单成交价>=mid 或 卖单成交价<=mid 表示主动穿越
            double mid = mid_it->second->v.load(std::memory_order_acquire);
            trade.is_crossing = isLong ? (price >= mid) : (price <= mid);
        }
        // 记录成交时的 alpha 信号和波动率（用于 alpha 绩效追踪）
        auto sig_it = _signal_aggregators.find(stdCode);
        if (sig_it != _signal_aggregators.end() && sig_it->second) {
            const SignalContext& sc = sig_it->second->getContext();
            if (sc.alpha.valid)
                trade.alpha_at_trade = sc.alpha.alpha;
            if (sc.volatility.valid)
                trade.volatility = sc.volatility.realized_vol;
        }
        _perf_analyzer->recordTrade(trade);
        _perf_analyzer->updatePosition(stdCode, _portfolio->getPosition(stdCode), 0);
    }

    // 记录到自身成交校准器 (统一管理成交记录，供毒性检测使用)
    if (_self_trade_calibrator) {
        // C2: 统一 replay 时钟 — v7.1 起 coordinator 侧 onTick/getFillRetreat/
        //     decayCalibration 均传 _exchange_time_ms (replay, ~1.75e15), 而此处写入侧
        //     用墙钟 epoch (~1.75e12): pruneHistory 的 cutoff 恒大于 fill_time ->
        //     所有 fill 记录当场被清空; getFillRetreat 的 elapsed 天文数字 ->
        //     retreat 永不激活 (实盘+回测均失效)。改用与读取侧同基准的 _exchange_time_ms。
        uint64_t timestamp = _exchange_time_ms;
        double mid_at_fill = price;
        constexpr double DEFAULT_FILL_SPREAD = 0.2; // 无盘口快照时的默认价差
        double spread_at_fill = DEFAULT_FILL_SPREAD;
        auto mid_it = _last_mid.find(stdCode);
        if (mid_it != _last_mid.end()) {
            mid_at_fill = mid_it->second->v.load(std::memory_order_acquire);
        }

        // 计算当前价差
        ContractState cs_buf;
        const ContractState* cs = _portfolio->getContractSnapshot(stdCode, cs_buf) ? &cs_buf : nullptr;
        if (cs)
            spread_at_fill = cs->tick_size * 2;

        _self_trade_calibrator->recordFill(stdCode, price, vol, isLong, mid_at_fill, spread_at_fill, timestamp);

        // 将校准结果传递给毒性检测器
        if (_config.modules.use_toxicity_detector && _toxicity_detector) {
            auto calibration = _self_trade_calibrator->getCalibration(stdCode);
            _toxicity_detector->onSelfTradeCalibration(calibration);
        }
    }

    // 改进日志格式：显示开平方向，更容易理解
    // isLong + OPEN -> 开多, isLong + CLOSE -> 平多
    // !isLong + OPEN -> 开空, !isLong + CLOSE -> 平空
    // 注意: TraderAdapter::on_trade 将 WOT 枚举转换为数值: 0=OPEN, 1=CLOSE, 2=CLOSETODAY
    // 不能用 offset == '0' (ASCII 48)，因为传入的是数值0而非字符'0'
    bool isOpen = (offset == 0); // 数值0 = WOT_OPEN
    const char* actionStr = "";
    if (isLong) {
        actionStr = isOpen ? "OPEN_LONG" : "CLOSE_LONG";
    } else {
        actionStr = isOpen ? "OPEN_SHORT" : "CLOSE_SHORT";
    }

    // 基于position变化判断实际效果
    // CTP的isLong+offset组合可能不反映实际持仓变化
    // 例如：ag2612有多仓24手时，OPEN_SHORT实际是平多（position减少）
    // 需要同时显示CTP方向和实际效果，避免日志误导
    const char* effectStr = "";
    ContractState cs_buf;
    if (auto* cs = _portfolio->getContractSnapshot(stdCode, cs_buf) ? &cs_buf : nullptr) {
        // position变化：正=增加多头/减少空头，负=减少多头/增加空头
        double pos_change = cs->position - cs->prev_position; // 需要记录prev_position
        if (pos_change > 0) {
            effectStr = "(+long)";
        } else if (pos_change < 0) {
            effectStr = "(+short)";
        } else {
            effectStr = "(flat)";
        }
    }

    // F13: 实盘成交路径最大单笔开销 — fmt(0.5-1μs)+同步文件写(2-20μs)/笔。
    //   逐笔明细降 debug; info 每 50 笔采样一条保留可观测性。
    // C11: defer per-fill debug log to tick path via SPSC (was biggest per-fill cost: fmt+file 2-20us)
    if (!s._tdspi_log_queue.tryPush(TdSpiLogEvent{
            0, FixedString24(stdCode), FixedString24(actionStr),
            vol, price, _portfolio->getTotalDelta(), FixedString24(effectStr), 0
        }))
        s._tdspi_logs_dropped.fetch_add(1, std::memory_order_relaxed);  // C11: queue full
    static uint64_t trade_log_cnt = 0;
    constexpr uint64_t TRADE_SAMPLE_INTERVAL = 50; // info 级成交采样间隔
    if ((++trade_log_cnt % TRADE_SAMPLE_INTERVAL) == 1) {
        WTSLogger::info("UftFutuMmStrategy[{}] TRADE[sample #{}]: {} {} {}@{} | Delta: {}",
                        s.id(),
                        trade_log_cnt,
                        stdCode,
                        actionStr,
                        vol,
                        price,
                        _portfolio->getTotalDelta());
    }

    // P1-3: 成交后重置报单错误计数(成功成交 = 连续错误中断)
    if (_order_error_count > 0) {
        _order_error_count = 0;
    }

    // ============================================================
    // 成交后检查风控状态，如果没有硬指标违规则恢复交易
    // ============================================================
    if (_trading_state.qphase == QuotingPhase::RISK_HALTED && _risk_monitor) {
        _risk_monitor->checkRiskLimits(_portfolio.get(), _violations_buf);
        auto& violations = _violations_buf;
        bool hasHardBreach = false;
        for (const auto& v : violations) {
            if (v.type == RiskLimitType::POSITION_NET || v.type == RiskLimitType::EXPOSURE ||
                v.type == RiskLimitType::DAILY_LOSS) {
                hasHardBreach = true;
                WTSLogger::debug("UftFutuMmStrategy[{}] Still has hard breach: {} {}", s.id(), v.code, (int)v.type);
                break;
            }
        }

        if (!hasHardBreach && _risk_monitor->getHaltCategory() != RiskCategory::IRREVERSIBLE) {
            // do not auto-resume while closeout flattening is in progress.
            // closeout halt must persist until on_session_begin restores it.
            if (_risk_monitor->isCloseoutFlattening() || _risk_monitor->isCloseoutTriggered() ||
                _trading_state.phase == MmPhase::CLOSEOUT) {
                // skip resume — closeout in progress
            } else {
                // 业务#2: 与 checkRisk 恢复路径统一 — 走 checkAndRecover 四道闸
                // (check_interval 节流 + cooldown 30s + max_recovery_count 熔断 + util/PnL 闸)。
                // 原路径任何一笔成交即 resumeTrading 解锁全部风控, 冷却与熔断被架空;
                // 且不复活 arb -> PAUSE 后 arb 静默停摆至 session 末。
                bool recovered = _risk_monitor->checkAndRecover(_portfolio.get());
                if (recovered && !_risk_monitor->isTradingHalted()) {
                    // Use resumeFromRisk() instead of direct assignments
                    _trading_state.resumeFromRisk();
                    _trading_state.unblockLong();
                    _trading_state.unblockShort();
                    _blocked_contracts.clear();
                    // A1: 同步重置协调器软风控倍数 — 本路径绕过 coordinator 的 checkRisk 自动恢复,
                    // 否则 _risk_spread_mult 残留, 恢复后报价宽度被永久放大.
                    if (_coordinator)
                        _coordinator->onExternalResumeFromRisk();
                    // 对称复活 arb executor (HALT/PAUSE/BLOCK 路径均 disable 过,
                    // coordinator 恢复路径 :810-814 同款)
                    if (_async_arb) {
                        AsyncArbConfig arbCfg = _async_arb->getConfig();
                        arbCfg.enabled.store(true);
                        _async_arb->setConfig(arbCfg);
                    }
                    WTSLogger::info("UftFutuMmStrategy[{}] Trading resumed after trade (unified recovery gates passed)",
                                    s.id());
                } else if (recovered) {
                    // 部分恢复(仅 unblock/resumeQuoting), 仍 halted — 等下一笔成交/下个 tick 再试
                    WTSLogger::debug("UftFutuMmStrategy[{}] Partial recovery after trade, still halted", s.id());
                } else {
                    // checkAndRecover 拒绝 (cooldown/次数/util/IRREVERSIBLE) — 保持 halted
                    WTSLogger::debug(
                        "UftFutuMmStrategy[{}] Recovery gates not passed after trade, keeping halted state", s.id());
                }
            }
        }

        // MonitorBridge: 成交后持仓/盈亏已变, 触发一次节流检查刷新快照
        _mon_bridge.maybeFlush(ctx);
    }
}

void FutuRuntimeOps::onChannelReady(UftFutuMmStrategy& s, wtp::IUftStraCtx* ctx)
{
    auto& _channel_ready = s._channel_ready;
    auto& _price_stale = s._price_stale;
    auto& _async_arb = s._async_arb;
    auto& _contract_infos = s._contract_infos;
    auto& _last_mid = s._last_mid;
    auto& _portfolio = s._portfolio;
    auto& _risk_monitor = s._risk_monitor;
    auto& _violations_buf = s._violations_buf;
    auto& _trading_state = s._trading_state;
    auto& _blocked_contracts = s._blocked_contracts;
    auto& _coordinator = s._coordinator;
    auto& _liquidator = s._liquidator;
    auto& _order_router = s._order_router;

    _channel_ready = true;
    _price_stale = true; // P1-4: 标记价格过期，直到收到首个 tick

    // 通道恢复, 重启套利 (与 on_channel_lost 的 setEnabled(false) 对称).
    if (_async_arb) {
        _async_arb->setEnabled(true);
    }

    //============================================================
    // 同步持仓和未成交订单
    //============================================================
    WTSLogger::info("UftFutuMmStrategy[{}] syncing positions and pending orders...", s.id());

    bool has_valid_price = false; // 是否有有效价格用于 delta 计算

    for (const auto& ci : _contract_infos) {
        // 使用策略本地持仓（而不是账户持仓）
        // 一个账户可能有多个策略，每个策略只管理自己的持仓
        double local_net = ctx->stra_get_local_position(ci.code.c_str());

        // 尝试获取当前价格用于 Delta 计算
        // 只使用 _last_mid (最新中间价)，这是从已收到的 tick 中计算的
        double price = 0;
        auto midIt = _last_mid.find(ci.code);
        if (midIt != _last_mid.end() && midIt->second->v.load(std::memory_order_acquire) > 0) {
            price = midIt->second->v.load(std::memory_order_acquire);
            has_valid_price = true;
        }

        // 更新 Portfolio
        double currentPos = _portfolio->getPosition(ci.code);
        if (std::abs(currentPos - local_net) > 0.01) {
            _portfolio->updatePosition(ci.code, local_net, 0);
            if (price > 0) {
                _portfolio->markToMarket(ci.code, price);
            }
            WTSLogger::info("UftFutuMmStrategy[{}] Position sync: {} local_portfolio={} -> local_net={}",
                            s.id(),
                            ci.code,
                            currentPos,
                            local_net);
        }

        // 记录日志
        if (price > 0) {
            WTSLogger::info(
                "UftFutuMmStrategy[{}] Contract {} synced: local_net={}, multiplier={}, tickSize={}, price={:.2f}",
                s.id(),
                ci.code,
                local_net,
                ci.multiplier,
                ci.tick_size,
                price);
        } else {
            WTSLogger::warn("UftFutuMmStrategy[{}] Contract {} synced: local_net={}, NO PRICE (delta will be 0, "
                            "waiting for first tick)",
                            s.id(),
                            ci.code,
                            local_net);
        }
    }

    // 同步后检查风控状态
    double totalDelta = _portfolio->getTotalDelta();
    WTSLogger::info("UftFutuMmStrategy[{}] Total delta after sync: {}", s.id(), totalDelta);

    // 只有在非不可逆风险状态下才恢复
    if (_risk_monitor) {
        if (_risk_monitor->getHaltCategory() != RiskCategory::IRREVERSIBLE) {
            // Delta 不依赖价格，可立即恢复报价
            // Exposure/Loss 风控在 on_tick 中正常触发，无需等第一个 tick
            if (!has_valid_price) {
                WTSLogger::info(
                    "UftFutuMmStrategy[{}] No price yet, resuming quoting (risk will activate on first tick)", s.id());
            }

            _risk_monitor->checkRiskLimits(_portfolio.get(), _violations_buf);
            auto& violations = _violations_buf;
            if (violations.empty()) {
                // Use resumeFromRisk() instead of direct assignments
                _trading_state.resumeFromRisk();
                _trading_state.unblockLong();
                _trading_state.unblockShort();
                _blocked_contracts.clear();
                _risk_monitor->resumeTrading();
                _risk_monitor->resumeQuoting();
                _risk_monitor->unblockLong();
                _risk_monitor->unblockShort();
                // A1: 同步重置协调器软风控倍数 (同 on_trade 恢复路径).
                if (_coordinator)
                    _coordinator->onExternalResumeFromRisk();
                WTSLogger::info("UftFutuMmStrategy[{}] Trading resumed after channel ready (risk normalized)", s.id());
            } else {
                // ============================================================
                // 检查是否有持仓超限，尝试自动平仓到安全水平
                // ============================================================
                bool has_position_breach = false;
                for (const auto& v : violations) {
                    if (v.type == RiskLimitType::POSITION_NET) {
                        has_position_breach = true;
                        // 执行自动平仓
                        ContractState breached_buf;
                        const ContractState* breached =
                            _portfolio->getPositionBreachedSnapshot(breached_buf) ? &breached_buf : nullptr;
                        if (breached) {
                            double reduction = _portfolio->getPositionReductionToLimit(*breached);
                            if (reduction != 0) {
                                // P0-1: 统一 RiskLiquidator 原语 — 对手价+FAK+价格三级校验+qty clamp。
                                //   旧实现用 stra_get_last_tick 最新价被动挂单 (可能不成交) 且无 price>0
                                //   校验 (m19), 语义与 coordinator FORCE FLAT 重复漂移。
                                std::string breached_code = breached->code;
                                _liquidator.setDeps({_order_router.get(), _portfolio.get()});
                                double qty = _liquidator.reduceContract(ctx,
                                                                        breached_code,
                                                                        std::abs(reduction),
                                                                        1,
                                                                        "AUTO REDUCE position breach (channel ready)");
                                if (qty <= 0) {
                                    WTSLogger::warn("UftFutuMmStrategy[{}] AUTO REDUCE skipped: {} reduction={} (price "
                                                    "invalid or no position)",
                                                    s.id(),
                                                    breached_code,
                                                    reduction);
                                }
                            }
                        }
                        break; // 一次只处理一个超限
                    }
                }

                if (!has_position_breach) {
                    WTSLogger::warn("UftFutuMmStrategy[{}] channel ready but risk still exists, keeping halted state",
                                    s.id());
                } else {
                    WTSLogger::info("UftFutuMmStrategy[{}] Auto position reduction triggered, will retry after trade",
                                    s.id());
                }

                // 保持风控状态不变
                // P1-1: TradingState manages its own phase; syncFromRiskMonitor removed.
                // If RiskMonitor is halted, ensure TradingState reflects it.
                if (_risk_monitor->isTradingHalted())
                    _trading_state.setQuotingPhase(QuotingPhase::RISK_HALTED);
            }
        } else {
            WTSLogger::warn("UftFutuMmStrategy[{}] channel ready but trading remains halted (IRREVERSIBLE)", s.id());
        }
    } else {
        // Use resumeFromRisk() instead of direct assignment
        _trading_state.resumeFromRisk();
    }

    WTSLogger::info("UftFutuMmStrategy[{}] channel ready", s.id());
}

void FutuRuntimeOps::onSessionBegin(UftFutuMmStrategy& s, wtp::IUftStraCtx* ctx, uint32_t uTDate)
{
    auto& _risk_monitor = s._risk_monitor;
    auto& _portfolio = s._portfolio;
    auto& _trading_state = s._trading_state;
    auto& _order_error_count = s._order_error_count;
    auto& _quoting_paused_since = s._quoting_paused_since;
    auto& _arb_bridge = s._arb_bridge;
    auto& _closeout_orch = s._closeout_orch;
    auto& _blocked_contracts = s._blocked_contracts;
    auto& _async_arb = s._async_arb;
    auto& _config = s._config;
    auto& _stp = s._stp;
    auto& _quoters = s._quoters;

    // 重置日内状态
    _risk_monitor->resetDaily();
    // 重置组合日内 PnL — 旧代码 realized_pnl 跨日累计, 导致"日亏损"风控
    // 用昨日亏损误判今日 IRREVERSIBLE halt.
    _portfolio->resetDailyPnl();
    // force=true —— 新交易日强制清 closeout state,
    // 否则上一日卡 FLATTENING 时 resetCloseout 会被状态机拒绝,导致 state 永久死锁
    _risk_monitor->resetCloseout(true); // 重置收盘前平仓状态(强制)

    // P1-1: reset() clears phase+qphase+blocks for new session
    _trading_state.reset();

    // 跨日重置下单错误状态机, 避免昨日累计错误带到今日.
    _order_error_count = 0;
    _quoting_paused_since = 0;

    // reset closeout hedge guard so new day can fire hedge if needed
    // (closeout 守卫/执行器复位已收编到 CloseoutOrchestrator, 架构重构 C3)
    // (套利桥状态复位已收编到 ArbExecutionBridge, 架构重构 C4)
    _arb_bridge.resetSession();
    _closeout_orch.resetSession();

    // 重置本地状态
    _blocked_contracts.clear();

    // 启动异步套利执行器
    // useAsyncArbThread=true(实盘默认): 启动独立 arb 线程, pushTick 走 SPSC 队列 (~50ns)
    // useAsyncArbThread=false(回测): 不启动线程, pushTick 主线程同步执行
    // 跨线程安全已修复: computeDerivedSpread 改读 _pair_states(spin保护),
    // canOpenPosition/getQuotingAdjustment 加锁, _min_profit_threshold 原子化.
    if (_async_arb) {
        if (_config.modules.use_async_arb_thread) {
            _async_arb->start();
            WTSLogger::info("AsyncArbitrageExecutor: async mode (arb thread started)");
        } else {
            WTSLogger::info("AsyncArbitrageExecutor: sync mode (arb thread disabled by config)");
        }
    }

    // 清空自成交防护模块
    if (_stp) {
        _stp->clear();
    }

    // R3 v2: BilateralStats 已 Per-Quoter 化,session start 在每个 quoter 上独立触发
    {
        uint32_t uTime_HHMM = ctx->stra_get_time();
        for (auto& [code, quoter] : _quoters) {
            auto& stats = quoter->getBilateralStats();
            if (stats.hasSessionInfo())
                stats.onSessionStart(uTime_HHMM);
        }
    }

    WTSLogger::info("UftFutuMmStrategy[{}] session begin: {}", s.id(), uTDate);
}

void FutuRuntimeOps::onSessionEnd(UftFutuMmStrategy& s, wtp::IUftStraCtx* ctx, uint32_t uTDate)
{
    auto& _trading_state = s._trading_state;
    auto& _async_arb = s._async_arb;
    auto& _quoters = s._quoters;
    auto& _stp = s._stp;
    auto& _perf_analyzer = s._perf_analyzer;
    auto& _portfolio = s._portfolio;
    auto& _closeout_orch = s._closeout_orch;
    auto& _exchange_time_ms = s._exchange_time_ms;
    auto& _mon_bridge = s._mon_bridge;

    // P1-1: enter CLOSEOUT phase on session end
    _trading_state.enterCloseout();

    // 停止异步套利执行器
    if (_async_arb) {
        _async_arb->stop();
    }

    // 撤销所有委托
    for (auto& [code, quoter] : _quoters) {
        quoter->cancelAll(ctx);
    }

    // 清空自成交防护模块
    if (_stp) {
        _stp->clear();
    }

    // R3 v2: BilateralStats Per-Quoter,逐合约 onSessionEnd + formatString 输出
    {
        uint32_t uTime_HHMM = ctx->stra_get_time();
        uint32_t sec_in_min = ctx->stra_get_secs();
        for (auto& [code, quoter] : _quoters) {
            auto& stats = quoter->getBilateralStats();
            if (!stats.hasSessionInfo())
                continue;
            stats.onSessionEnd(uTime_HHMM, sec_in_min);
            WTSLogger::info("[BILATERAL_STATS] {} | {}", code, stats.formatString());
        }
    }

    // 绩效分析报告
    if (_perf_analyzer) {
        auto metrics = _perf_analyzer->getMetrics();
        WTSLogger::info("[PERF] session={} | pnl={:.2f}(unreal={:.2f}) | vol={} trades={} | "
                        "spread_captured={:.4f} capture_rate={:.2f}% | fill_rate={:.2f}% | "
                        "max_dd={:.2f} sharpe={:.2f} win={:.2f}% | adverse={:.4f} real_adv/vol={:.4f} tox_events={} | "
                        "alpha_acc={:.2f}% alpha_pnl={:.2f} | avg_inv={:.1f} turnover={:.2f}",
                        uTDate,
                        metrics.total_pnl,
                        metrics.unrealized_pnl,
                        metrics.total_volume,
                        metrics.total_trades,
                        metrics.avg_spread_captured,
                        metrics.spread_capture_rate * 100,
                        metrics.fill_rate * 100,
                        metrics.max_drawdown,
                        metrics.sharpe_ratio,
                        metrics.win_rate * 100,
                        metrics.adverse_ratio,
                        metrics.real_adverse_per_vol,
                        metrics.toxicity_events,
                        metrics.alpha_accuracy * 100,
                        metrics.alpha_pnl_per_trade,
                        metrics.avg_inventory,
                        metrics.inventory_turnover);
    }

    WTSLogger::info("UftFutuMmStrategy[{}] session end: {}, Delta: {}", s.id(), uTDate, _portfolio->getTotalDelta());

    // session_end closeout 状态强制收尾 (已拆分至 CloseoutOrchestrator, 架构重构 C3)
    _closeout_orch.finalizeAtSessionEnd(_exchange_time_ms > 0 ? _exchange_time_ms.load(std::memory_order_acquire)
                                                              : TimeUtils::getLocalTimeNow());

    // MonitorBridge: 收盘终写 stradata + 追加 funds.csv 历史资金曲线
    _mon_bridge.onSessionEnd(ctx, uTDate);
}

void FutuRuntimeOps::onEntrust(
    UftFutuMmStrategy& s, wtp::IUftStraCtx* ctx, uint32_t localid, bool bSuccess, const char* message)
{
    auto& _trading_state = s._trading_state;
    auto& _order_error_count = s._order_error_count;
    auto& _main_ctx = s._main_ctx;
    auto& _quoters = s._quoters;
    auto& _quoting_paused_since = s._quoting_paused_since;
    auto& _config = s._config;
    auto& _exchange_time_ms = s._exchange_time_ms;
    auto& _coordinator = s._coordinator;
    auto& _order_tracker = s._order_tracker;
    auto& _order_router = s._order_router;
    auto& _risk_monitor = s._risk_monitor;
    auto& _portfolio = s._portfolio;
    auto& _async_arb = s._async_arb;
    auto& _arb_bridge = s._arb_bridge;
    auto& _blocked_contracts = s._blocked_contracts;
    auto& _violations_buf = s._violations_buf;
    auto& _liquidator = s._liquidator;
    auto& _contract_infos = s._contract_infos;
    auto& _last_mid = s._last_mid;

    //============================================================
    // 策略层不做柜台特定错误分类
    // 柜台错误(CTP平仓不足/流控等)由 TraderCTP 适配层处理
    // 策略只做通用错误计数: 达到阈值 → halt
    //============================================================
    // RISK_HALTED 期间 set(ERROR) 会被 canTransitionQuoting 静默拒绝
    // (H 来源仅允许 N), 但 _order_error_count++ 已污染状态.
    // H 是更高优先级的暂停, 不应再被下单错误覆盖, 直接忽略.
    if (_trading_state.qphase == QuotingPhase::RISK_HALTED) {
        WTSLogger::warn("UftFutuMmStrategy[{}] on_entrust during RISK_HALTED (success={}), ignored", s.id(), bSuccess);
        return;
    }

    if (bSuccess) {
        _order_error_count = 0;
        // v7.1: 报单引擎确认 → 双边统计挂单确认入口
        // (在场时间从引擎确认时刻起算, 不含发出→确认的网络延迟;
        //  回测 mocker postTask 异步触发, 回调时 quoter 已注册 ID)
        if (_main_ctx) {
            uint32_t uTime = _main_ctx->stra_get_time();
            uint32_t secs = _main_ctx->stra_get_secs() / 1000;
            for (auto& [code, quoter] : _quoters) {
                if (quoter->isMyOrder(localid)) {
                    quoter->onEntrustAck(localid, uTime, secs);
                    break;
                }
            }
        }
        // P1-6/U1: 用 tryResumeFrom 替代 setQuotingPhase(NORMAL),
        // 仅当 qphase 真的是 ERROR 时才翻 NORMAL (避免在其它态下乱翻)
        if (_trading_state.tryResumeFrom(QuotingPhase::ERROR)) {
            _quoting_paused_since = 0;
            WTSLogger::info("UftFutuMmStrategy[{}] Quoting resumed after successful order", s.id());
        }
        return;
    }

    // 报单失败 — 通用计数
    std::string errMsg = message ? message : "";
    _order_error_count++;

    WTSLogger::error("UftFutuMmStrategy[{}] Order FAILED (count={}/{}): localid={}, error={}",
                     s.id(),
                     _order_error_count,
                     _config.order_control.order_error_threshold,
                     localid,
                     errMsg);

    // v7.1 生产适配: 拒单的 localid 已被 handle*Quote 注册进 quoter level, 但订单
    // 从未上交易所。若不清理, getValidQuoteSnapshot 仍把死单当有效深度 → 统计虚高
    // (sticky/pause 期间死单滞留)。复用 onOrder(canceled) 路径移除死单 + 更新统计。
    if (_main_ctx) {
        uint32_t uTime = _main_ctx->stra_get_time();
        uint32_t secs = _main_ctx->stra_get_secs() / 1000;
        for (auto& [code, quoter] : _quoters) {
            if (quoter->isMyOrder(localid)) {
                quoter->onOrder(localid, true, 0, uTime, secs);
                break;
            }
        }
    }

    // 套利单被拒: 撤同 pair 另一腿,防止裸腿
    if (_async_arb && _order_router) {
        std::string pair_id;
        if (_async_arb->consumePairTag(localid, pair_id)) {
            WTSLogger::warn("Arb order REJECTED: localid={}, pair={}, canceling all arb orders", localid, pair_id);
            _order_router->cancelByPair(_main_ctx, pair_id); // A7: 原 cancelAllBySource 误撤其它 pair
            // A2: 补残腿防护标记 — broker 拒单场景此前仅撤单, 对侧在途成交无 hedge 回补
            _arb_bridge.markLegRejected(pair_id, 0);
        }
    }

    // M1/M2: 拒单无 on_order 终结回调, tracker/router 映射统一幂等清理
    // (quoter 单上方已清 level 状态; 此处清 tracker 订单 + router 活跃表)。
    finalizeOrder(s, localid);

    if (_order_error_count >= _config.order_control.order_error_threshold) {
        _trading_state.setQuotingPhase(QuotingPhase::ERROR);
        // 硬触发分支补设 paused_since, 让 handleQuotingAutoResume 正常工作.
        _quoting_paused_since =
            _exchange_time_ms > 0 ? _exchange_time_ms.load(std::memory_order_acquire) : TimeUtils::getLocalTimeNow();

        WTSLogger::error("UftFutuMmStrategy[{}] Trading HALTED due to consecutive order errors (count={}/threshold={})",
                         s.id(),
                         _order_error_count,
                         _config.order_control.order_error_threshold);

        if (_risk_monitor)
            _risk_monitor->haltTrading(RiskCategory::REVERSIBLE);

        if (_main_ctx) {
            for (auto& [code, quoter] : _quoters)
                quoter->cancelAll(_main_ctx);
        }
    } else {
        // 软触发不 cancelAll, 维持原语义 (临时小问题, 挂单留着等下笔成功)
        _trading_state.setQuotingPhase(QuotingPhase::ERROR);
        _quoting_paused_since =
            _exchange_time_ms > 0 ? _exchange_time_ms.load(std::memory_order_acquire) : TimeUtils::getLocalTimeNow();
        WTSLogger::warn("UftFutuMmStrategy[{}] Quoting temporarily paused due to order error ({}/{})",
                        s.id(),
                        _order_error_count,
                        _config.order_control.order_error_threshold);
    }
}

void FutuRuntimeOps::onOrderEvent(UftFutuMmStrategy& strat,
                                  wtp::IUftStraCtx* ctx,
                                  uint32_t localid,
                                  const char* stdCode,
                                  bool isLong,
                                  uint32_t offset,
                                  double totalQty,
                                  double leftQty,
                                  double price,
                                  bool isCanceled)
{
    auto& _risk_monitor = strat._risk_monitor;
    auto& _exchange_time_ms = strat._exchange_time_ms;
    auto& _quoters = strat._quoters;
    auto& _stp = strat._stp;
    auto& _closeout_orch = strat._closeout_orch;
    auto& _order_router = strat._order_router;
    auto& _async_arb = strat._async_arb;
    auto& _arb_bridge = strat._arb_bridge;

    // 更新频率统计
    if (_risk_monitor && isCanceled)
        _risk_monitor->recordCancel();

    // 计算当前时间戳:
    //   - now_ms (epoch 毫秒) 给 RiskMonitor 用(closeout 超时/重试判定)
    //   - uTime_HHMM / sec_in_min 给 BilateralStats 用
    // 统一使用 TimeUtils::getLocalTimeNow() (epoch ms), 与 RiskMonitor 的
    // checkCloseoutRetry / _current_time 保持同一时间基准。
    // 旧实现 date*86400000 把 YYYYMMDD 当天数, 产生 ~1.75e15 的垃圾时间戳.
    uint32_t time_hhmm = ctx->stra_get_time();
    uint32_t ssmmm = ctx->stra_get_secs();
    uint32_t s = ssmmm / 1000;
    // v7.1: 用 replay 时钟 (与 RiskMonitor._current_time 同基准; 回测可复现)
    uint64_t now_ms =
        _exchange_time_ms > 0 ? _exchange_time_ms.load(std::memory_order_acquire) : TimeUtils::getLocalTimeNow();

    // 更新 Quoter 订单状态(内部会从 UnifiedOrderTracker 移除)
    // 同时触发双边报价统计更新
    // R3 v2: 改用 (uTime_HHMM, sec_in_min) 签名
    uint32_t uTime_HHMM = time_hhmm;
    uint32_t sec_in_min = s;
    for (auto& [code, quoter] : _quoters) {
        if (quoter->isMyOrder(localid)) {
            quoter->onOrder(localid, isCanceled, leftQty, uTime_HHMM, sec_in_min);
            break;
        }
    }

    // 从自成交防护模块中移除（订单撤销或完全成交）
    if ((isCanceled || leftQty == 0) && _stp)
        _stp->untrackOrder(localid);

    // closeout 订单跟踪 (已拆分至 CloseoutOrchestrator, 架构重构 C3)
    // 注意: 必须在 onOrderDone 抹除来源记录之前调用 (内部用 OrderRouter 来源标记识别)
    _closeout_orch.onOrderEvent(ctx, localid, stdCode, isCanceled, leftQty, now_ms);

    // 通知 OrderRouter 订单完成（撤销或完全成交）
    if ((isCanceled || leftQty == 0) && _order_router)
        _order_router->onOrderDone(localid);

    // Scheme B-3: clean up arb localid → pair_id tag on order finalize
    // (full fill or cancel). Defensive: removes stale entries even if
    // onArbOrderFilled (in_flight tracking) already saw the fills.
    if ((isCanceled || leftQty == 0) && _async_arb) {
        // A4: 套利腿撤单 → 通知 bridge (撤对侧 + 残腿标记 + 释放 in_flight).
        // 必须在 onOrderFinalized 清 tag 之前查询 pair_id.
        // consumePairTag 语义为"查询不抹除"(支持 partial fill), 此处调用安全.
        if (isCanceled) {
            std::string pair_id;
            if (_async_arb->consumePairTag(localid, pair_id)) {
                _arb_bridge.onLegCancelled(ctx, pair_id);
            }
        }
        _async_arb->onOrderFinalized(localid);
    }
}

void FutuRuntimeOps::onChannelLost(UftFutuMmStrategy& s, wtp::IUftStraCtx* ctx)
{
    auto& _channel_ready = s._channel_ready;
    auto& _trading_state = s._trading_state;
    auto& _quoting_paused_since = s._quoting_paused_since;
    auto& _exchange_time_ms = s._exchange_time_ms;
    auto& _quoters = s._quoters;
    auto& _risk_monitor = s._risk_monitor;
    auto& _portfolio = s._portfolio;
    auto& _async_arb = s._async_arb;

    _channel_ready = false;

    // 1. 立即暂停所有交易和报价
    // P1-1: enter RISK_HALTED on channel lost
    _trading_state.setQuotingPhase(QuotingPhase::RISK_HALTED);
    _quoting_paused_since =
        _exchange_time_ms > 0 ? _exchange_time_ms.load(std::memory_order_acquire) : TimeUtils::getLocalTimeNow();

    // 2. 撤销所有做市挂单（通道断开时无法保证订单状态）
    for (auto& [code, quoter] : _quoters) {
        quoter->cancelAll(ctx);
    }

    // 3. 通知风控模块
    if (_risk_monitor) {
        _risk_monitor->haltTrading(RiskCategory::REVERSIBLE, _portfolio ? _portfolio->getTotalPnL() : 0);
    }

    // 3.5 停止套利 — 通道断开时 arb 线程继续生成信号只会塞满 _order_queue 后无声丢弃.
    if (_async_arb) {
        _async_arb->setEnabled(false);
    }

    // 4. 快照当前持仓（通道恢复后用于校验）
    if (_portfolio) {
        for (const auto& cs : _portfolio->getAllContractsSnapshot()) {
            WTSLogger::warn(
                "UftFutuMmStrategy[{}] Position snapshot on channel lost: {} pos={:.0f}", s.id(), cs.code, cs.position);
        }
    }

    WTSLogger::error("UftFutuMmStrategy[{}] channel lost - all orders cancelled, trading halted", s.id());
}

void FutuRuntimeOps::finalizeOrder(UftFutuMmStrategy& s, uint32_t localid)
{
    auto& _order_tracker = s._order_tracker;
    auto& _order_router = s._order_router;
    auto& _exchange_time_ms = s._exchange_time_ms;

    // M1/M2: 拒单(on_entrust false, 无 on_order 终结回调)的统一清理:
    //   tracker 侧先标 REJECTED 再 untrack — 否则 untrackOrder 按
    //   "非 pending_cancel" 计入 orders_filled -> fill_rate 虚高;
    //   router 侧清活跃表/pair 映射 — 否则 CLOSEOUT 源死单让
    //   CloseoutExecutor 的 inflight 守卫(无超时)永久卡死, 仓位过夜。
    // 两者对不存在 id 均幂等 no-op, quoter 单/router 单统一走此路径。
    if (_order_tracker) {
        _order_tracker->markPendingCancel(localid, CancelReason::REJECTED);
        _order_tracker->untrackOrder(localid, _exchange_time_ms);
    }
    if (_order_router)
        _order_router->onOrderDone(localid);
}

} // namespace futu
