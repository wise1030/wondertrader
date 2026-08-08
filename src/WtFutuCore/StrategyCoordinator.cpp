/*!
* \file StrategyCoordinator.cpp
* \brief Strategy Coordinator Implementation
*
* Complete tick processing pipeline - replaces inline on_tick logic.
*/

#include "StrategyCoordinator.h"
#include "OrderApiGuard.h"
#include "FutuPortfolio.h"
#include "FutuQuoter.h"
#include "FutuRiskMonitor.h"
#include "ToxicFlowDetector.h"
#include "UnifiedOrderTracker.h"
#include "CorrelationManager.h"

#include "SpreadOptimizer.h"
#include "MarketDataContext.h"
#include "SelfTradeCalibrator.h"
#include "PerformanceMonitor.h"
#include "RiskLiquidator.h"
#include "TscClock.h"
#include "SignalAggregator.h"         // 新增：信号聚合器
#include "OrderRouter.h"              // 新增：统一下单路由器
#include "SpreadArbitrageManager.h"   // B2/B6: 平仓 intent / 聚合 z-score
#include "../WTSUtils/WTSCfgLoader.h" // YAML 加载器
#include "../Includes/IUftStraCtx.h"
#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../WTSTools/WTSLogger.h"

#include <algorithm>
#include <cmath>

namespace futu
{

StrategyCoordinator::StrategyCoordinator() : _channel_ready(true), _tick_count(0), _portfolio_ctx_dirty(true) {}

StrategyCoordinator::~StrategyCoordinator()
{
    // 智能指针自动清理
}

//==========================================================================
// Configuration Loading
//==========================================================================

bool StrategyCoordinator::loadConfig(const std::string& config_file)
{
    // 使用 WTSCfgLoader 加载 YAML 配置文件
    wtp::WTSVariant* cfg = WTSCfgLoader::load_from_file(config_file);
    if (!cfg) {
        WTSLogger::error("StrategyCoordinator: failed to load config file '{}'", config_file);
        return false;
    }

    // 获取 coordinator 节点
    wtp::WTSVariant* coordinator = cfg->get("coordinator");
    if (!coordinator) {
        // 尝试直接使用根节点（兼容无 coordinator 包裹的配置）
        WTSLogger::warn("StrategyCoordinator: no 'coordinator' section in '{}', using root", config_file);
        coordinator = cfg;
    }

    // 调用 loadConfigFromVariant 解析配置
    loadConfigFromVariant(coordinator);

    WTSLogger::info("StrategyCoordinator: loaded from '{}' (signal_aggregator={})",
                    config_file,
                    _cfg.use_signal_aggregator ? "ON" : "OFF");

    return true;
}

void StrategyCoordinator::loadConfigFromVariant(wtp::WTSVariant* cfg)
{
    if (!cfg)
        return;

    _cfg._raw_variant = cfg;

    // Helper functions
    auto readBool = [](wtp::WTSVariant* v, const char* key, bool defVal) -> bool {
        if (!v)
            return defVal;
        wtp::WTSVariant* node = v->get(key);
        return node ? node->asBoolean() : defVal;
    };
    auto readUInt32 = [](wtp::WTSVariant* v, const char* key, uint32_t defVal) -> uint32_t {
        if (!v)
            return defVal;
        wtp::WTSVariant* node = v->get(key);
        return node ? (uint32_t)node->asInt64() : defVal;
    };
    auto readDouble = [](wtp::WTSVariant* v, const char* key, double defVal) -> double {
        if (!v)
            return defVal;
        wtp::WTSVariant* node = v->get(key);
        return node ? node->asDouble() : defVal;
    };

    // =====================================================================
    // 策略级开关 (coordinator 根级, 唯一权威位置, 不依赖 modules 节点存在)
    // =====================================================================
    _cfg.use_market_making = readBool(cfg, "useMarketMaking", _cfg.use_market_making);
    _cfg.use_spread_arbitrage = readBool(cfg, "useSpreadArbitrage", _cfg.use_spread_arbitrage);
    _cfg.use_signal_aggregator = readBool(cfg, "use_signal_aggregator", _cfg.use_signal_aggregator);

    // Read modules section (modules.<name>.enabled 是模块级开关的唯一权威位置)
    wtp::WTSVariant* modules = cfg->get("modules");
    if (modules) {
        // 模块级开关: 只读 modules.<name>.enabled, 不再 fallback 到 use_xxx 扁平键
        auto readModuleEnabled = [&](const char* name, bool defVal) -> bool {
            wtp::WTSVariant* mod = modules->get(name);
            if (mod) {
                return readBool(mod, "enabled", defVal);
            }
            return defVal;
        };

        // Map module names to config flags (market making modules)
        // 注：use_alpha_engine 和 use_market_state 已移除，由 SignalAggregator 内部管理
        _cfg.use_toxicity_detector = readModuleEnabled("toxicityDetector", _cfg.use_toxicity_detector);
        _cfg.use_spread_optimizer = readModuleEnabled("spreadOptimizer", _cfg.use_spread_optimizer);
        _cfg.use_adaptive_params = readModuleEnabled("adaptiveParam", _cfg.use_adaptive_params);
        _cfg.use_self_trade_prevention = readModuleEnabled("selfTradePrevention", _cfg.use_self_trade_prevention);

        // If market making is disabled, disable all MM-specific modules
        if (!_cfg.use_market_making) {
            _cfg.use_toxicity_detector = false;
            _cfg.use_spread_optimizer = false;
            WTSLogger::info("Market making disabled, MM modules deactivated");
        }

        // 新架构依赖 MarketDataContext 作为数据源，由 SignalAggregator 内部管理
        if (_cfg.use_signal_aggregator) {
            WTSLogger::info("SignalAggregator enabled: MarketDataContext auto-enabled as data source");
        }
    }

    // Read pipeline section
    wtp::WTSVariant* pipeline = cfg->get("pipeline");
    if (pipeline) {
        _cfg.param_update_interval = readUInt32(pipeline, "paramUpdateInterval", _cfg.param_update_interval);
        // 除零保护: param_update_interval 用于每 tick 取模, 配 0 会 SIGFPE
        if (_cfg.param_update_interval == 0)
            _cfg.param_update_interval = 1;
        _cfg.modules.alpha_sensitivity = readDouble(pipeline, "alphaSensitivity", _cfg.modules.alpha_sensitivity);
    }

    // closeout/perf params are propagated from config.yaml via FutuMmConfig, not read here

    // signal_pipeline section removed - weights not used in current implementation

    // Read module parameters (for strategy to create modules)
    if (modules) {
        // ToxicityDetector parameters (cooloff_ms 仍需保留，其余已迁移到 fromVariant)
        wtp::WTSVariant* toxicity = modules->get("toxicityDetector");
        if (toxicity) {
            _cfg.modules.toxicity_cooloff_ms = readUInt32(toxicity, "cooloffMs", _cfg.modules.toxicity_cooloff_ms);
        }

        // SpreadOptimizer parameters: 已迁移到 GLFTParams::fromVariant
        // SpreadOptimizer: 已迁移到 GLFTParams::fromVariant
        // alphaSensitivity 仍需保留在 ModuleParams 中
        wtp::WTSVariant* spread = modules->get("spreadOptimizer");
        if (spread) {
        }
        // MarketState: 无消费者，已删除

        // AutoCancel parameters
        wtp::WTSVariant* autoCancel = modules->get("autoCancel");
        if (autoCancel) {
            _cfg.modules.auto_cancel_max_age_ms =
                readUInt32(autoCancel, "maxAgeMs", _cfg.modules.auto_cancel_max_age_ms);
            _cfg.modules.auto_cancel_price_deviation =
                readDouble(autoCancel, "priceDeviation", _cfg.modules.auto_cancel_price_deviation);
            _cfg.modules.auto_cancel_inventory_cooldown_ms =
                readUInt32(autoCancel, "inventoryLimitCooldownMs", _cfg.modules.auto_cancel_inventory_cooldown_ms);
        }

        // SelfTradePrevention: 已迁移到 StpConfig::fromVariant

        // Adaptive parameters
        wtp::WTSVariant* adaptive = modules->get("adaptiveParam");
        if (adaptive) {
            _cfg.modules.adaptive_update_interval =
                readUInt32(adaptive, "updateInterval", _cfg.modules.adaptive_update_interval);
            _cfg.modules.adaptive_min_phi = readDouble(adaptive, "minPhi", _cfg.modules.adaptive_min_phi);
            _cfg.modules.adaptive_max_phi = readDouble(adaptive, "maxPhi", _cfg.modules.adaptive_max_phi);
        }

        // CorrelationManager: 已迁移到 CorrelationConfig::fromVariant

        // SignalAggregator: 已迁移到 SignalAggregatorConfig::fromVariant
    }

    // v7.1 taker 紧急减仓参数
    _cfg.taker_reduce_threshold = readDouble(cfg, "takerReduceThreshold", _cfg.taker_reduce_threshold);
    _cfg.taker_reduce_target_util = readDouble(cfg, "takerReduceTargetUtil", _cfg.taker_reduce_target_util);
    _cfg.taker_reduce_cooldown_ms = readUInt32(cfg, "takerReduceCooldownMs", _cfg.taker_reduce_cooldown_ms);

    // v7.1 成交后立即重挂参数
    _cfg.requote_after_fill_min_interval_ms =
        readUInt32(cfg, "requoteAfterFillMinIntervalMs", _cfg.requote_after_fill_min_interval_ms);

    // v7.1 session 休息段参数
    _cfg.section_break_minutes_before = readUInt32(cfg, "sectionBreakMinutesBefore", _cfg.section_break_minutes_before);

    syncPhaseConfig();

    WTSLogger::info("StrategyCoordinator: loaded config from variant (toxicity={}, perf={})",
                    _cfg.use_toxicity_detector,
                    _cfg.perf_enabled);
}

void StrategyCoordinator::syncPhaseConfig()
{
    // 5A-1: 同步会话阶段判定配置 (closeout 窗口参数与 _cfg 同源;
    //       loadConfig / setConfig 后各调一次, 保持与 processCloseout 判定一致)
    SessionPhaseConfig pcfg;
    pcfg.section_break_minutes_before = _cfg.section_break_minutes_before;
    pcfg.close_time = _cfg.close_time;
    pcfg.closeout_minutes_before = _cfg.closeout_minutes_before;
    pcfg.night_close_time = _cfg.night_close_time;
    pcfg.night_minutes_before = _cfg.night_minutes_before;
    _phase_mgr.configure(pcfg);
}

void StrategyCoordinator::initialize()
{
    WTSLogger::info("StrategyCoordinator: initialized (perf={})", _cfg.perf_enabled);
    // v7.7 业务#3: adaptive 模块现状文档化 — updateAdaptiveParams 是空占位,
    // use_adaptive_params 开启时实际不工作, 启动期明示避免误导
    if (_cfg.use_adaptive_params) {
        WTSLogger::warn("StrategyCoordinator: use_adaptive_params=true but updateAdaptiveParams "
                        "is a placeholder (no-op). Adaptive tuning is NOT active.");
    }
}

//==========================================================================
// Main Entry Point
//==========================================================================

ProcessingResult StrategyCoordinator::processTick(
    wtp::IUftStraCtx* ctx, const char* stdCode, wtp::WTSTickData* tick, uint64_t now_ms, uint64_t tsc_tick0)
{
    ProcessingResult result;

    // F5: 计时门控 — perf 关闭时零 chrono 开销 (vDSO ~20-25ns/次 ×2)
    const bool perf_on = (_perf_monitor && _cfg.perf_enabled);
    std::chrono::high_resolution_clock::time_point start_time;
    if (perf_on)
        start_time = std::chrono::high_resolution_clock::now();

    // Build tick context
    TickContext tc;
    tc.code = stdCode;
    tc.time_hms = ctx->stra_get_time();
    tc.date = ctx->stra_get_date();
    // 毫秒时间戳(epoch), 用于冷却期/closeout 等计算.
    // now_ms 由调用方(on_tick)注入时复用, 避免每 tick 重复读墙钟.
    tc.timestamp = (now_ms > 0) ? now_ms : TimeUtils::getLocalTimeNow();

    // v7.7 性能#1: 本 tick 唯一一次合约快照, 后续 Stage (preCheck/quoting) 复用
    if (_portfolio)
        tc.cs_valid = _portfolio->getContractSnapshot(tc.code, tc.cs);

    // Stage 0: Closeout state machine (always needed)
    if (processCloseout(ctx, tc)) {
        result.closeout_executed = true;
        result.processed = true;
        // T2: closeout 窗口不再是风控盲区 — 活跃平仓态下仍跑硬风控 (仅跳报价):
        //   强平过程中若日亏击穿/CRITICAL, HALT_TRADING 立即介入
        //   (FORCE FLAT 与 closeout 目标一致); executor 侧由 isTradingHalted 门收尾。
        if (_cfg.use_market_making && _risk_monitor && _portfolio) {
            CloseoutSub cs = _risk_monitor->getCloseoutSub();
            if (cs == CloseoutSub::DRAINING || cs == CloseoutSub::ASSESSING || cs == CloseoutSub::EXECUTING ||
                cs == CloseoutSub::RETRYING) {
                tc.total_delta = _portfolio->getTotalDelta(); // FORCE FLAT qty 需要
                checkRisk(ctx, tc);
            }
        }
        return result;
    }

    // Stage 0.5: v7.1 session 休息段 (每节收盘前 N 分钟暂停; 最后一节归 closeout)
    if (processSectionBreak(ctx, tc)) {
        result.processed = true;
        return result;
    }

    // Stage 1: Pre-check (always needed)
    if (!preCheck(ctx, tc, tick)) {
        return result;
    }

    // 一次性解析本合约组件指针, 后续各 Stage 复用 (消除重复字符串哈希查找)
    if (_signal_aggregators) {
        auto it = _signal_aggregators->find(tc.code);
        if (it != _signal_aggregators->end())
            tc.aggregator = it->second.get();
    }
    if (_market_data) {
        auto it = _market_data->find(tc.code);
        if (it != _market_data->end())
            tc.book = it->second.get();
    }
    if (_quoters) {
        auto it = _quoters->find(tc.code);
        if (it != _quoters->end())
            tc.quoter = it->second.get();
    }
    if (_spread_opts) {
        auto it = _spread_opts->find(tc.code);
        if (it != _spread_opts->end())
            tc.spread_opt = it->second.get();
    }

    // Stage 2: Update market data (always needed)
    updateMarketData(ctx, tc, tick);

    // Bug C: 夜盘 closeout 完成后, 报价暂停到夜盘收盘(每日开盘前)才恢复.
    // closeout 在 nightMinutesBefore 前完成, 此刻夜盘未收; on_session_begin 不在日盘
    // 开盘触发(仅进程启动), 故恢复点放此. 首个 >= night_close_time 的 tick 恢复报价.
    if (_cfg.night_close_time > 0 && _trading_state && _trading_state->isCloseoutActive() &&
        _risk_monitor->getCloseoutSubInfo().night_closeout_done &&
        ((tc.time_hms >= 10000) ? (tc.time_hms / 100) : tc.time_hms) >= _cfg.night_close_time) {
        _trading_state->exitToQuoting();
        WTSLogger::info("[CLOSEOUT] Night session ended (now >= {}), resuming quoting for day session",
                        _cfg.night_close_time);
    }

    // ===== Market Making Pipeline =====
    if (_cfg.use_market_making) {
        // Stage 3: Update signals (MM only)
        updateSignals(ctx, tc, tick);

        // Stage 4: Check risk
        if (!checkRisk(ctx, tc)) {
            // BLOCK_SIDE / HALT 时仍执行 taker 减仓 - 减仓是恢复正常的关键手段.
            // 旧逻辑 HALT 时跳过 checkTakerReduce 是反的: 恰恰 HALT 时最需要强平减仓
            // (13:50 HALT 后 TAKER_REDUCE 不复燃, 持仓任由 requote 放大). requote 路径
            // 已独立拦截 HALT(见 requoteAfterFill), 此处只管主动减仓.
            checkTakerReduce(ctx);
            result.processed = true;
            return result;
        }

        // Stage 5: Process auto-cancel (先撤旧单,再报新单)
        result.order_canceled = processAutoCancel(ctx, tc);

        // Stage 5.5: v7.1 taker 紧急减仓 (穿仓主动吃单; 报价不停, 与之并行)
        checkTakerReduce(ctx);

        // Stage 6: Process quoting (MM core)
        result.quote_placed = processQuoting(ctx, tc, tick);
    }

    // ===== Arbitrage Pipeline =====
    // Note: Arbitrage processing is handled by UftFutuMmStrategy::processSpreadArbitrage
    // when use_spread_arbitrage is enabled

    // Stage 7.5: Position reduction removed — skew+clamp handles inventory reduction via quote offset
    // (attemptPositionReduction used 3-tick cross-spread which was too costly;
    //  enhanced skew with clamp+scale now drives ask to mid for natural reduction)

    // Stage 8: Update adaptive parameters
    updateAdaptiveParams(ctx, tc);

    result.processed = true;
    _tick_count++;

    // Record to performance monitor (F5: 门控, 含 chrono 计时本身)
    if (perf_on) {
        auto end_time = std::chrono::high_resolution_clock::now();
        result.processing_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
        _perf_monitor->recordTickToQuote(result.processing_time_ns);
        _perf_monitor->recordTickProcessed();

        // P0: on_tick 入口 → 主流水线结束 全链路延迟 (rdtsc, 含策略层 preamble
        //   markToMarket/correlation; 语义存 SIGNAL_TO_ORDER 通道)
        if (tsc_tick0 > 0) {
            _perf_monitor->recordSignalToOrder(TscClock::toNs(TscClock::now() - tsc_tick0));
        }

        // P0-4: 每秒更新一次性能统计
        if (tc.timestamp - _last_perf_ms >= 1000) {
            _perf_monitor->updatePerSecondCounters();
            _last_perf_ms = tc.timestamp;
        }

        // P0: 每 60s 输出延迟摘要 (p50/p90/p99; getLatencyStats 排序 16K 元素,
        //   低频调用可接受)
        if (tc.timestamp - _last_summary_ms >= 60000) {
            WTSLogger::info("[PERF] {}", _perf_monitor->getSummary());
            _last_summary_ms = tc.timestamp;
        }
    }

    return result;
}

//==========================================================================
// Stage 0.5: Session Section Break (v7.1)
//   每节收盘前 N 分钟: 撤全部报价 + arb 在途单, 暂停报价/套利;
//   下一节首 tick 自动恢复。每日最后一节跳过 (closeout 状态机处理)。
//   例 EC(FD0900): 10:15/11:30 休息段, 15:00 归 closeout。
//==========================================================================

bool StrategyCoordinator::processSectionBreak(wtp::IUftStraCtx* ctx, TickContext& tc)
{
    if (_cfg.section_break_minutes_before == 0)
        return false;

    // 5A-1: 窗口判定统一走 SessionPhaseManager; sess 指针取回缓存进 TickContext
    //   (F7: preCheck 复用; v7.7 A4: tc 改非 const 引用, 消除 const_cast)
    wtp::WTSSessionInfo* sess = _phase_mgr.getSession(tc.code);
    tc.session = sess;
    if (!sess)
        return false;

    // 当前分钟 (stra_get_time 返回 HHMM(4位), 兼容 HHMMSS)
    uint32_t cur_hhmm = (tc.time_hms >= 10000) ? tc.time_hms / 100 : tc.time_hms;

    bool in_break = _phase_mgr.inSectionBreak(tc.code, tc.time_hms);

    if (in_break) {
        if (!_section_break_active) {
            _section_break_active = true;
            WTSLogger::info("[SECTION_BREAK] {} entering break window at {} ({}min before section end), cancel all + "
                            "pause quoting/arb",
                            tc.code,
                            cur_hhmm,
                            _cfg.section_break_minutes_before);
            // 撤全部做市报价
            if (_quoters) {
                for (auto& [code, quoter] : *_quoters) {
                    if (quoter)
                        quoter->cancelAll(ctx);
                }
            }
            // 撤 arb/hedge 在途单 + 停 arb 信号执行
            if (_order_router) {
                _order_router->cancelAllBySource(ctx, Source::ARBITRAGE);
                _order_router->cancelAllBySource(ctx, Source::HEDGING);
            }
            if (_arb_executor) {
                AsyncArbConfig arbCfg = _arb_executor->getConfig();
                arbCfg.enabled.store(false);
                _arb_executor->setConfig(arbCfg);
            }
        }
        return true; // 休息段内: 跳过 MM pipeline (报价/对冲/taker)
    }

    // 休息段结束 → 恢复
    if (_section_break_active) {
        _section_break_active = false;
        bool arb_resumed = false;
        if (_arb_executor) {
            // B2 fix: RISK_HALTED 时不复活 arb - 由 checkRisk 恢复路径
            // (resumeFromRisk + enabled=true) 在风险正常化后统一复活,
            // 避免 break 期间触发的风控 halt 被此处绕过
            if (!isTradingHalted()) {
                AsyncArbConfig arbCfg = _arb_executor->getConfig();
                arbCfg.enabled.store(true);
                _arb_executor->setConfig(arbCfg);
                arb_resumed = true;
            }
        }
        WTSLogger::info("[SECTION_BREAK] {} break window ended at {}, resuming quoting{}",
                        tc.code,
                        cur_hhmm,
                        arb_resumed ? "/arb" : " (arb deferred: risk halted)");
    }
    return false;
}

//==========================================================================
// Stage 0: Closeout State Machine
//==========================================================================

bool StrategyCoordinator::processCloseout(wtp::IUftStraCtx* ctx, TickContext& tc)
{
    if (!_risk_monitor) {
        return false;
    }

    // 至少有一个触发点启用才继续
    if (_cfg.closeout_minutes_before <= 0 && (_cfg.night_close_time == 0 || _cfg.night_minutes_before <= 0)) {
        return false;
    }

    CloseoutSub state = _risk_monitor->getCloseoutSub();
    uint32_t closeTime = _cfg.close_time;

    switch (state) {
    case CloseoutSub::IDLE: {
        bool triggered = _risk_monitor->checkCloseout(tc.time_hms, closeTime);
        if (triggered) {
            if (_quoters) {
                for (auto& [code, quoter] : *_quoters) {
                    if (quoter)
                        quoter->cancelAll(ctx);
                }
            }

            if (_cfg.closeout_flatten_position && _portfolio) {
                _risk_monitor->markCloseoutDraining(tc.timestamp);
            } else {
                _risk_monitor->markCloseoutCompleted(tc.timestamp);
            }
            return true;
        }
        return false;
    }

    case CloseoutSub::FAILED: {
        uint64_t now_ms = tc.timestamp;
        if (_risk_monitor->checkCloseoutRetry(now_ms)) {
            if (_quoters) {
                for (auto& [code, quoter] : *_quoters) {
                    if (quoter)
                        quoter->cancelAll(ctx);
                }
            }
            if (_portfolio) {
                _risk_monitor->markCloseoutDraining(now_ms);
            }
        }
        return true;
    }

    case CloseoutSub::TRIGGERED:
    case CloseoutSub::DRAINING:
    case CloseoutSub::ASSESSING:
    case CloseoutSub::EXECUTING:
    case CloseoutSub::RETRYING:
        return true;

    case CloseoutSub::COMPLETED: {
        //======================================================================
        // 区分夜盘/白盘 closeout 完成
        //
        // 夜盘平仓完成后，立即重置状态+恢复做市。
        // 白盘 closeout 完成后，不再重置（终态，直到日内交易结束）。
        //
        // 旧逻辑: 等 currentHour>=6 才 reset → 凌晨完成时 hour=0 不满足
        //         → 整个白盘都不做市！
        // 新逻辑: 夜盘 closeout 完成立即 reset + resume，白盘 closeout
        //         在 minutes_before=15 时才重新触发 (14:45)，期间正常做市。
        //======================================================================
        const auto& closeoutInfo = _risk_monitor->getCloseoutSubInfo();

        if (_cfg.night_close_time > 0 && closeoutInfo.is_night_closeout) {
            // 夜盘 closeout 完成 → 立即 reset，让白盘可以正常做市
            _risk_monitor->resetCloseout();
            // Bug C: 不调 exitToQuoting - phase 保持 CLOSEOUT, 报价暂停, 等夜盘收盘恢复.
            // (旧代码此处 exitToQuoting -> 收盘前 90s 报价器恢复重建仓, 把 anchor 对冲打掉.
            //  on_session_begin 仅进程启动触发, 常驻系统日盘开盘无 session_begin, 故恢复点
            //  放 processTick 夜盘收盘检测, 见 MM pipeline 前)
            WTSLogger::info("[CLOSEOUT] Night session closeout completed, holding quoting paused until night close {}",
                            _cfg.night_close_time);
            // 重新检查白盘 closeout (09:00 不会触发，14:45 才触发)
            bool triggered = _risk_monitor->checkCloseout(tc.time_hms, closeTime);
            if (triggered) {
                if (_quoters) {
                    for (auto& [code, quoter] : *_quoters) {
                        if (quoter)
                            quoter->cancelAll(ctx);
                    }
                }
                if (_cfg.closeout_flatten_position && _portfolio) {
                    _risk_monitor->markCloseoutDraining(tc.timestamp);
                } else {
                    _risk_monitor->markCloseoutCompleted(tc.timestamp);
                }
                return true;
            }
            return false;
        }

        // 白盘 closeout 完成 → 终态，不做市直到日内交易结束
        // 只在首次进入 COMPLETED 时打日志+halt，避免每 tick 循环
        if (_trading_state) {
            _trading_state->enterCloseout();
        }
        if (_quoters) {
            for (auto& [code, quoter] : *_quoters) {
                if (quoter)
                    quoter->cancelAll(ctx);
            }
        }
        return true;
    }

    default:
        return false;
    }
}

//==========================================================================
// Stage 1: Pre-check
//==========================================================================

bool StrategyCoordinator::preCheck(wtp::IUftStraCtx* ctx, TickContext& tc, wtp::WTSTickData* tick)
{
    if (!_channel_ready || !tick) {
        return false;
    }

    if (_risk_monitor && _risk_monitor->isCloseoutTriggered()) {
        return false;
    }

    // Extract prices
    tc.bid_px = tick->bidprice(0);
    tc.ask_px = tick->askprice(0);

    // nan/inf tick 防御 — IEEE754 下 nan<=0 == false,会绕过 <=0 校验
    // 历史教训: EC 数据存在 ts=0859... 预开盘 nan tick 污染 SpreadCalculator 的 leg_history,
    // 进而 calculateBeta()=nan → hedge_ratio=nan → delta()=position*nan=nan,蔓延全局
    if (!std::isfinite(tc.bid_px) || !std::isfinite(tc.ask_px)) {
        if ((++_nan_tick_cnt & 0xFFF) == 1) { // 每 4096 次打一条,避免日志洪水
            WTSLogger::warn("StrategyCoordinator: {} non-finite tick bid={} ask={} (cnt={}), skipping",
                            tc.code,
                            tc.bid_px,
                            tc.ask_px,
                            _nan_tick_cnt);
        }
        return false;
    }

    if (tc.bid_px <= 0 || tc.ask_px <= 0) {
        return false;
    }

    tc.mid = (tc.bid_px + tc.ask_px) / 2.0;

    // P0-1: 隔夜持仓用昨收(pre_close)作为成本基准
    // v7.7 性能#1: tc.cs 快照已在 processTick 入口填充, 此处只读
    if (_portfolio) {
        const ContractState* cs = tc.cs_valid ? &tc.cs : nullptr;
        if (cs && cs->position != 0 && cs->avg_cost == 0) {
            // 首次收到 tick 且有隔夜持仓,用昨收设置成本基准
            double pre_close = tick->preclose();
            if (pre_close > 0) {
                _portfolio->setReferencePrice(tc.code, pre_close);
                WTSLogger::info(
                    "Overnight position reference: {} cost={} (pre_close), pos={}", tc.code, pre_close, cs->position);
            }
        }
    }

    // P0-2: 填充涨跌停价到 TickContext
    tc.upper_limit = tick->upperlimit();
    tc.lower_limit = tick->lowerlimit();

    // Get tick size from portfolio (v7.7: 复用 tc.cs 快照)
    if (_portfolio) {
        if (tc.cs_valid) {
            tc.tick_size = tc.cs.tick_size;
        }
    }

    // tick_size=0保护 — 合约信息缺失时除零会导致报价计算崩溃
    if (tc.tick_size <= 0) {
        WTSLogger::warn("StrategyCoordinator: {} tick_size=0 (contract info missing), skipping tick", tc.code);
        return false;
    }

    // 真正的交易时段检查（修复休市期间报价问题）
    // F7: 优先复用 processSectionBreak 缓存的 session 指针, 未缓存再查 manager
    wtp::WTSSessionInfo* sessInfo = tc.session;
    if (!sessInfo)
        sessInfo = _phase_mgr.getSession(tc.code);
    if (sessInfo) {
        // stra_get_time() 全栈约定返回 HHMM(4位), 不是 HHMMSS(6位)
        // 之前 /100 → HH, 把 23:14 误判成 00:23, 导致回测全 skip
        // F6: 复用 tc.time_hms (processTick 入口已 stra_get_time, 消除重复跨 DSO 虚调用)
        uint32_t currentTime = tc.time_hms; // HHMM 格式
        tc.is_trading_session = sessInfo->isInTradingTime(currentTime);

        if (!tc.is_trading_session) {
            WTSLogger::debug(
                "StrategyCoordinator: {} not in trading session at {:04d}, skipping", tc.code, currentTime);
        }
    } else {
        // 无 session 信息，默认允许交易（兼容旧行为）
        tc.is_trading_session = true;
    }

    return tc.is_trading_session;
}

//==========================================================================
// Stage 2: Update Market Data
//==========================================================================

void StrategyCoordinator::updateMarketData(wtp::IUftStraCtx* ctx, TickContext& tc, wtp::WTSTickData* tick)
{
    // Update portfolio (position and prices)
    if (_portfolio) {
        _portfolio->onTick(tc.code.c_str(), tick);
        _last_mid[tc.code] = tc.mid;

        _global_portfolio_ctx.total_delta = _portfolio->getTotalDelta();
        _global_portfolio_ctx.total_exposure = _portfolio->getTotalExposure();
        _global_portfolio_ctx.related.clear();
        _portfolio_ctx_dirty = false;

        // perf#4: 单 tick 内复用 (processTick 期间持仓不变), 后续 Stage 免重复 O(n) 扫描
        tc.total_delta = _global_portfolio_ctx.total_delta;
        tc.total_exposure = _global_portfolio_ctx.total_exposure;

        // A2 fix: publish atomic PnL snapshot for arb thread (lock-free on x86-64)
        _portfolio->publishPnLSnapshot();
    }
}

//==========================================================================
// Stage 3: Update Signals
//==========================================================================

void StrategyCoordinator::updateSignals(wtp::IUftStraCtx* ctx, const TickContext& tc, wtp::WTSTickData* tick)
{
    //==========================================================================
    // 错误处理：SignalAggregator 未初始化
    // 新架构默认启用，此分支仅在初始化失败时执行
    //==========================================================================
    if (!_signal_aggregators || !_market_data) {
        WTSLogger::error("StrategyCoordinator: SignalAggregator not initialized, skipping signal update");
        return;
    }

    // 1. 更新 MarketDataContext（唯一数据源）— 复用 processTick 入口解析的指针
    MarketDataContext* book = tc.book;
    if (!book) {
        auto book_it = _market_data->find(tc.code);
        if (book_it == _market_data->end() || !book_it->second) {
            return;
        }
        book = book_it->second.get();
    }
    book->onTick(tick);

    // 2. 使用 SignalAggregator 聚合所有信号 — 复用入口解析的指针
    SignalAggregator* aggregator = tc.aggregator;
    if (!aggregator) {
        auto agg_it = _signal_aggregators->find(tc.code);
        if (agg_it == _signal_aggregators->end() || !agg_it->second) {
            return;
        }
        aggregator = agg_it->second.get();
    }

    const SignalContext& sig_ctx = aggregator->update(*book);
    SignalContext& mutable_sig_ctx = aggregator->getContext();

    // 3. 更新市场状态暂停标志
    // 使用TradingState方法
    // P1-6/U1: enter 用 setQuotingPhase (会被 RISK_HALTED 校验拒绝, 安全),
    //         exit 用 tryResumeFrom(MARKET) (仅 qphase==MARKET 时翻 NORMAL,
    //         避免在 HALT/ERROR/TOXICITY 期间被误翻).
    if (sig_ctx.shouldPause()) {
        if (_trading_state)
            _trading_state->setQuotingPhase(QuotingPhase::MARKET);
        // DIAG: 限频诊断日志，确认shouldPause()触发原因
        {
            uint64_t now_ms = TimeUtils::getLocalTimeNow();
            if (now_ms - _last_pause_diag_ms > 5000) {
                WTSLogger::warn("[DIAG] {} shouldPause=true: should_pause={} toxic_detected={} "
                                "vol_tier={} realized_vol={:.6f} vol_percentile={:.1f}",
                                tc.code,
                                sig_ctx.market_state.should_pause,
                                sig_ctx.toxicity.toxic_detected,
                                (int)sig_ctx.volatility.vol_tier,
                                sig_ctx.volatility.realized_vol,
                                sig_ctx.volatility.vol_percentile);
                _last_pause_diag_ms = now_ms;
            }
        }
    } else {
        if (_trading_state)
            _trading_state->tryResumeFrom(QuotingPhase::MARKET);
    }

    if (_trading_state && _trading_state->qphase == QuotingPhase::MARKET && tc.quoter) {
        tc.quoter->cancelAll(ctx);
    }

    // 4. 更新毒性检测器 (使用 SignalContext 的信号)
    if (_cfg.use_toxicity_detector && _toxicity && sig_ctx.alpha.valid) {
        AlphaResult alpha_res;
        alpha_res.alpha = sig_ctx.alpha.alpha;
        alpha_res.is_strong_signal = sig_ctx.alpha.is_strong_signal;
        alpha_res.timestamp = sig_ctx.timestamp;

        TradeImbalanceResult trade_res;
        trade_res.net_flow = sig_ctx.trade_flow.net_flow;
        trade_res.imbalance_ratio = sig_ctx.trade_flow.net_flow_normalized;

        _toxicity->updateMarketAlpha(alpha_res, trade_res);
    }

    // 5. 更新 spread optimizer (用于交易统计，报价计算已纯函数化)
    if (_cfg.use_spread_optimizer && _spread_opts) {
        // NO-OP: onTick removed, SpreadOptimizer is now a functional engine
    }

    // 6. 更新 self trade calibrator
    if (_self_trade_calibrator) {
        _self_trade_calibrator->onTick(tc.code, tc.mid, tc.timestamp);
    }

    // 7. 更新 VPIN
    if (_cfg.use_toxicity_detector && _toxicity) {
        _toxicity->onTickVolume(tc.code.c_str(), tick);

        ToxicityMetrics tox = _toxicity->analyze();

        // 每tick输出毒性分数（debug级别）— 降采样: 每 50 tick 一次,
        // 热路径上避免每 tick fmt 格式化开销
        if ((_tick_count % 50) == 0) {
            WTSLogger::debug("[TOXIC] {} score={:.4f} vpin={:.4f} is_toxic={}",
                             tc.code,
                             tox.toxic_score,
                             tox.predictive_toxicity,
                             tox.is_toxic);
        }

        if (tox.is_toxic) {
            mutable_sig_ctx.toxicity.toxicity_score = tox.toxic_score;
            mutable_sig_ctx.toxicity.toxic_detected = true;
            mutable_sig_ctx.toxicity.toxic_side = tox.toxic_side;
            mutable_sig_ctx.toxicity.valid = true;
        } else {
            // toxic_detected每tick重算，不复位锁存
            // 与should_pause相同的锁存: 只设true不复位false
            // 导致toxic_detected一旦被设就永久锁死
            mutable_sig_ctx.toxicity.toxic_detected = false;
            mutable_sig_ctx.toxicity.valid = true;
        }
    }
}

//==========================================================================
// Stage 4: Check Risk Limits
//==========================================================================

bool StrategyCoordinator::checkRisk(wtp::IUftStraCtx* ctx, const TickContext& tc)
{
    if (!_risk_monitor || !_portfolio)
        return true;

    // Check if previously halted (hard limit)
    if (_risk_monitor->isTradingHalted()) {
        // T2: closeout 窗口内禁止自动恢复 (与 on_trade 路径的 "closeout halt must
        //     persist until on_session_begin" 语义对齐); 正常路径 processCloseout
        //     活跃态已提前 return, 此守卫只影响 T2 的 closeout 窗口复跑。
        if (_risk_monitor->isCloseoutFlattening() || _risk_monitor->isCloseoutTriggered()) {
            return false;
        }
        // 尝试自动恢复(REVERSIBLE halt): checkAndRecover 内部有节流(check_interval_ms)
        // + cooldown + canRecover 全套校验, IRREVERSIBLE 会被拒绝.
        if (_risk_monitor->checkAndRecover(_portfolio) && !_risk_monitor->isTradingHalted()) {
            if (_trading_state) {
                _trading_state->resumeFromRisk();
                _trading_state->unblockLong();
                _trading_state->unblockShort();
            }
            if (_arb_executor) {
                AsyncArbConfig arbCfg = _arb_executor->getConfig();
                arbCfg.enabled.store(true);
                _arb_executor->setConfig(arbCfg);
            }
            WTSLogger::info("StrategyCoordinator[{}]: Recovered from REVERSIBLE halt, resuming operations", tc.code);
            // fall through: 恢复成功后继续走正常风控检查
        } else {
            if (_trading_state) {
                _trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
            }
            // 限频日志：每5秒输出一次，避免刷屏
            {
                uint64_t now_ms = TimeUtils::getLocalTimeNow();
                if (now_ms - _last_halt_log_ms > 5000) {
                    WTSLogger::error("[RISK] Trading still halted (isTradingHalted=true), skipping risk check");
                    _last_halt_log_ms = now_ms;
                }
            }
            return false;
        }
    }

    if (_risk_monitor->checkDeltaRate()) {
        // 使用TradingState方法
        // B3: delta-rate 停机只在此设置; 恢复走下方 violations.empty() 分支的统一
        // 恢复路径 (以 !checkDeltaRate() 为门, 等 RiskMonitor 15s 冷却清除标志后
        // 一次性完整恢复). 不要在此加 else 恢复分支 — 它会对违规类 RISK_HALTED
        // (PAUSE/BLOCK) 误触发, 抢在完整恢复前翻转 qphase, 导致 arb 永久禁用/
        // 方向 block 残留/spread_mult 不复位.
        if (_trading_state) {
            _trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
        }
        // 业务#3: delta 剧烈异动 = 价格正在快速移动, 黏性/追价机制全部停摆,
        // 旧价位双边义务单原样留在场上是最危险场景 (最长滞留 5s cooldown)。
        // 与 PAUSE/HALT 路径对齐: 停机同步撤单。
        if (_quoters) {
            for (auto& [code, quoter] : *_quoters) {
                quoter->cancelAll(ctx);
            }
        }
    }

    // R2.4: 策略性软响应 (delta util 0.8/0.9 → WIDEN_SPREAD, 不产生硬 violation).
    //   在 hard check 之前执行; soft action 不阻断 hard check (两者可叠加).
    //   设计: WIDEN_SPREAD 是策略行为 (调整报价), 不是硬风控 (BLOCK/PAUSE/HALT).
    //   v7.1 无状态化: 每 tick 由当前 util 重算, util 回落即回 1.0,
    //   消除旧 std::max 闩锁 (util 回落后 spread 仍被永久放大直到完整恢复).
    //   5A-2: 状态移入 RiskWidenPolicy。
    {
        double cur_util = _portfolio ? _portfolio->getPortfolioDeltaUtilization() : 0;
        double l1 = _risk_monitor->getRateLimits().position_warning_l1;
        double l2 = _risk_monitor->getRateLimits().position_warning_l2;
        bool halted = _trading_state && _trading_state->qphase == QuotingPhase::RISK_HALTED;
        _quote_chain.riskWiden().tickSoft(cur_util, l1, l2, halted);
    }

    // P0-1.1: Active risk check every tick (复用缓冲, 零堆分配)
    _risk_monitor->checkRiskLimits(_portfolio, _violations_buf);
    auto& violations = _violations_buf;
    if (!violations.empty()) {
        RiskCategory category;
        RiskAction action = _risk_monitor->determineActionWithCategory(violations, category);

        switch (action) {
        case RiskAction::HALT_TRADING:
            // 使用TradingState方法
            if (_trading_state) {
                _trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
            }
            _risk_monitor->haltTrading(category, _portfolio->getTotalPnL());

            // P0-2: halt 后动作补全 — 撤所有做市单
            if (_quoters) {
                for (auto& [code, quoter] : *_quoters) {
                    quoter->cancelAll(ctx);
                }
            }
            // 撤所有非做市活跃单
            if (_order_router) {
                _order_router->cancelAllBySource(ctx, Source::CLOSEOUT);
                _order_router->cancelAllBySource(ctx, Source::HEDGING);
                _order_router->cancelAllBySource(ctx, Source::ARBITRAGE);
            }

            // IRREVERSIBLE → 全组合强平(对手价FAK) — P0-1: 统一 RiskLiquidator 原语;
            //   v7.7 业务#2: forceFlatAnchor(仅anchor×delta手数) → forceFlatAll(逐合约实际持仓)
            if (category == RiskCategory::IRREVERSIBLE && _portfolio && _order_router) {
                _liquidator.setDeps({_order_router, _portfolio});
                _liquidator.forceFlatAll(ctx, "HALT IRREVERSIBLE FORCE FLAT");
            }

            if (_arb_executor) {
                AsyncArbConfig arbCfg = _arb_executor->getConfig();
                arbCfg.enabled.store(false);
                _arb_executor->setConfig(arbCfg);
                WTSLogger::error("StrategyCoordinator[{}]: Arbitrage executor disabled due to HALT_TRADING", tc.code);
            }
            break;

        case RiskAction::PAUSE_QUOTING:
            // v7.3: 不可达死分支已删除 — determineActionWithCategory 不再返回 PAUSE_QUOTING
            //   (原判定 long_breach&&short_breach 数学上不可能同时成立)。
            //   仓位控制全部由软连续控制链承担 (skew/force/takerReduce), 硬停只有 HALT。
            break;

        case RiskAction::BLOCK_SIDE_LONG:
            // R2.7: 进入 RISK_HALTED, 走统一恢复路径 (此前 blockLong 后 qphase 仍 NORMAL,
            // 恢复分支要求 qphase==RISK_HALTED → block 永久残留, 仅 channel_ready 可清)
            if (_trading_state) {
                _trading_state->blockLong();
                _trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
            }
            if (_risk_monitor)
                _risk_monitor->pauseQuoting();
            // 业务#4: EXPOSURE breach 恰是最该停 arb 的场景 (毛暴露超限, arb 继续开仓
            // 会进一步放大暴露), 与 HALT/PAUSE 对齐停 executor; 恢复走统一路径复活。
            if (_arb_executor) {
                AsyncArbConfig arbCfg = _arb_executor->getConfig();
                arbCfg.enabled.store(false);
                _arb_executor->setConfig(arbCfg);
                WTSLogger::warn("StrategyCoordinator[{}]: Arbitrage executor disabled due to BLOCK_SIDE_LONG", tc.code);
            }
            WTSLogger::warn("[RISK] BLOCK_SIDE_LONG: halted until recovery");
            break;

        case RiskAction::BLOCK_SIDE_SHORT:
            // R2.7: 同 BLOCK_SIDE_LONG
            if (_trading_state) {
                _trading_state->blockShort();
                _trading_state->setQuotingPhase(QuotingPhase::RISK_HALTED);
            }
            if (_risk_monitor)
                _risk_monitor->pauseQuoting();
            // 业务#4: 同 BLOCK_SIDE_LONG
            if (_arb_executor) {
                AsyncArbConfig arbCfg = _arb_executor->getConfig();
                arbCfg.enabled.store(false);
                _arb_executor->setConfig(arbCfg);
                WTSLogger::warn("StrategyCoordinator[{}]: Arbitrage executor disabled due to BLOCK_SIDE_SHORT",
                                tc.code);
            }
            WTSLogger::warn("[RISK] BLOCK_SIDE_SHORT: halted until recovery");
            break;

        case RiskAction::WIDEN_SPREAD:
            // R2.5: 分级倍数 — L1(util≥0.8)→×1.5, L2(util≥0.9)→×2.0.
            // (soft check 已在 hard check 之前处理了主要的 WIDEN; 此处处理 WARNING 级别升级路径,
            //  即 determineActionWithCategory 末尾 breachCount>=widen_threshold 的返回)
            // 5A-2: 状态移入 RiskWidenPolicy。
            {
                double cur_util = _portfolio ? _portfolio->getPortfolioDeltaUtilization() : 0;
                double l2 = _risk_monitor->getRateLimits().position_warning_l2;
                _quote_chain.riskWiden().onHardWiden(cur_util, l2);
            }
            break;

            // R2.5/D5: REDUCE_SIZE 已删除 — 做市有最低报价数量要求, 不能 reduce qty;
            //   统一用 WIDEN_SPREAD 分级倍数替代 (加宽 spread 降低成交率, 近似 qty 缩减)

        case RiskAction::FLATTEN_POSITION: {
            // v7.3: 不可达死分支已删除 — breachCount 恒 <=1 (仅 EXPOSURE 产 BREACH,
            //   每 tick 至多一条), flatten_threshold=2 永不可达。
            //   强平职能由 HALT_TRADING 的 IRREVERSIBLE FORCE FLAT 承担。
            break;
        }

        default:
            break;
        }
    } else {
        // Auto-recovery check (仅针对 RISK_HALTED — MARKET/TOXICITY/ERROR 暂停
        // 有各自的恢复路径, 旧代码 !isActive() 会把 MARKET 暂停误翻 NORMAL 造成状态闪烁)
        // B3: delta-rate 停机期间 (!checkDeltaRate() 为 false) 禁止恢复 —
        // 等 RiskMonitor 冷却清除 _delta_rate_breached 后才允许走统一恢复.
        if (_trading_state && _trading_state->qphase == QuotingPhase::RISK_HALTED) {
            // v7.8: 区分 delta-rate-only halt 与 hard violation halt
            // delta-rate breach 是速率问题(瞬时变化太快), 恢复不应被 delta_util 绝对水平阻止,
            // 否则形成死锁: 高delta阻止恢复 -> 无法报价 -> 无法减仓 -> delta无法降低
            bool _v78_hard_violation = _risk_monitor->isTradingHalted() || _risk_monitor->isQuotingPaused();
            bool _v78_delta_cleared = !_risk_monitor->checkDeltaRate();
            if (_v78_delta_cleared && (!_v78_hard_violation || _risk_monitor->canRecover(_portfolio))) {
                // P1-1: resumeFromRisk() unconditionally sets qphase=NORMAL
                // (replaces old 3-call recovery that cleared individual bool flags)
                _trading_state->resumeFromRisk();
                _trading_state->unblockLong();
                _trading_state->unblockShort();

                // P-11 fix: 同步RiskMonitor的atomic状态，保持单一source of truth
                _risk_monitor->resumeQuoting();
                _risk_monitor->unblockLong();
                _risk_monitor->unblockShort();

                // R2: 重置软风控倍数 (WIDEN_SPREAD 分级设置的, 恢复时归 1.0)
                _quote_chain.riskWiden().reset();

                if (_arb_executor) {
                    AsyncArbConfig arbCfg = _arb_executor->getConfig();
                    arbCfg.enabled.store(true);
                    _arb_executor->setConfig(arbCfg);
                }
                WTSLogger::info("StrategyCoordinator[{}]: Risk normalized, resuming operations{}",
                                tc.code,
                                _v78_hard_violation ? "" : " (delta-rate halt recovery)");
            }
        }
    }

    // Check toxicity cooldown
    // 空指针保护 + P0-12: 使用TradingState方法
    // P1-6/U1: enter 用 setQuotingPhase, exit 用 tryResumeFrom(TOXICITY)
    // 避免冷却期结束时在 HALT/ERROR/MARKET 期间被误翻 NORMAL.
    if (_toxicity && _quote_chain.toxicity().inCooloff(tc.timestamp)) {
        if (_trading_state)
            _trading_state->setQuotingPhase(QuotingPhase::TOXICITY);
        if (_self_trade_calibrator) {
            _self_trade_calibrator->decayCalibration(tc.code, tc.timestamp, _cfg.modules.toxicity_cooloff_ms);
        }
    } else {
        if (_trading_state)
            _trading_state->tryResumeFrom(QuotingPhase::TOXICITY);
    }

    // 空指针保护
    return !_trading_state || _trading_state->qphase != QuotingPhase::RISK_HALTED;
}

//==========================================================================
// Stage 5: Process Quoting
//==========================================================================

bool StrategyCoordinator::processQuoting(wtp::IUftStraCtx* ctx, const TickContext& tc, wtp::WTSTickData* tick)
{
    if (!_trading_state || !_trading_state->canQuote()) {
        return false;
    }

    if (!_quoters || !_portfolio)
        return false;

    if (!tc.quoter)
        return false;

    //==========================================================================
    // 0.5 冷启动保护：信号源未热身时使用 maxSpreadMult 保守报价
    //==========================================================================
    bool cold_start = false;
    if (tc.aggregator) {
        const SignalContext& sc = tc.aggregator->getContext();
        if (!sc.alpha.valid ||
            sc.alpha.confidence < _cfg.modules.alpha_sensitivity * _cfg.modules.cold_start_confidence_factor) {
            cold_start = true;
        }
    }

    //==========================================================================
    // 1. 获取市场信号上下文 (Alpha, Volatility Tier)
    //==========================================================================
    double alpha = 0.0;
    const SignalContext* sig_ctx = nullptr;
    if (tc.aggregator) {
        sig_ctx = &(tc.aggregator->getContext());
        alpha = sig_ctx->alpha.valid ? sig_ctx->alpha.alpha : 0.0;
    }

    //==========================================================================
    // 2. 准备组合上下文 (从全局缓存构建单合约特定部分)
    //==========================================================================
    PortfolioContext p_ctx = _global_portfolio_ctx;

    const ContractState* cs = tc.cs_valid ? &tc.cs : nullptr; // v7.7: 复用 preCheck 快照
    if (cs) {
        p_ctx.current_multiplier = cs->multiplier;
        p_ctx.current_hedge_ratio = cs->hedge_ratio;
        p_ctx.current_price = tc.mid;
        p_ctx.contract_max_delta = cs->contract_max_delta;
    }

    //==========================================================================
    // 2.5 v3/v7.1 软风控前置: 统一仓位利用率口径
    //   (pos+同向pending)/maxPos — skew 归一化注入 + quoter qty衰减/义务 共用
    //==========================================================================
    double _v3_long_util = 0.0;
    double _v3_short_util = 0.0;
    bool _v3_force_ask_obligation = false;
    bool _v3_force_bid_obligation = false;
    bool _v3_pending_drain_bid = false;
    bool _v3_pending_drain_ask = false;
    bool _v3_hard_block_bid = false;
    bool _v3_hard_block_ask = false;
    if (_risk_monitor) {
        auto pre_trade = _risk_monitor->checkPreTradePosition(tc.code, _portfolio, _order_tracker);
        _v3_long_util = pre_trade.long_utilization;
        _v3_short_util = pre_trade.short_utilization;
        _v3_force_ask_obligation = pre_trade.force_ask_obligation;
        _v3_force_bid_obligation = pre_trade.force_bid_obligation;
        _v3_pending_drain_bid = pre_trade.pending_drain_bid;
        _v3_pending_drain_ask = pre_trade.pending_drain_ask;
        _v3_hard_block_bid = pre_trade.hard_block_bid;
        _v3_hard_block_ask = pre_trade.hard_block_ask;
        // v7.1: 带符号仓位利用率注入 skew (正=多 负=空, 取较大侧)
        if (cs && cs->max_position > 0) {
            p_ctx.contract_pos_util = (_v3_long_util >= _v3_short_util) ? _v3_long_util : -_v3_short_util;
            p_ctx.contract_pos_util_valid = true;
        }
    }

    //==========================================================================
    // 3. 使用 SpreadOptimizer 计算动态 Skew 和价差倍数
    //==========================================================================
    double skew = 0.0;
    double spread_mult = 1.0;
    double fallback_spread = 2.0;
    if (tc.spread_opt) {
        fallback_spread = tc.spread_opt->getParams().base_spread;
    }
    double l0_bid = tc.mid - fallback_spread * tc.tick_size;
    double l0_ask = tc.mid + fallback_spread * tc.tick_size;

    if (tc.spread_opt && _cfg.use_spread_optimizer && sig_ctx) {
        double contractDelta = cs ? cs->delta() : 0.0;

        GLFTResult res =
            tc.spread_opt->computeOptimalQuote(tc.mid, contractDelta, *sig_ctx, _cfg.modules.alpha_sensitivity, &p_ctx);

        skew = res.inventory_skew;
        spread_mult = res.spread_mult;
        l0_bid = res.bid_price;
        l0_ask = res.ask_price;
    }

    //==========================================================================
    // 3.1-3.3 报价决策链 (5A-2 QuotePolicyChain)
    //   执行顺序与旧内联实现严格一致:
    //   RiskWiden(软风控倍数) → ArbCloseSync(B2抑制+B6观测) → Toxicity(毒性冷却)
    //   → LimitPrice(涨跌停L1/L2/L3) → ColdStart(冷启动保守重算) → FillRetreat(成交后退)
    //==========================================================================
    QuotePolicyContext pctx;
    pctx.code = tc.code;
    pctx.mid = tc.mid;
    pctx.tick_size = tc.tick_size;
    pctx.upper_limit = tc.upper_limit;
    pctx.lower_limit = tc.lower_limit;
    pctx.timestamp = tc.timestamp;
    pctx.cold_start = cold_start;
    pctx.spread_opt = tc.spread_opt;
    pctx.arb_manager = _arb_manager;
    pctx.toxicity = _toxicity;
    pctx.calibrator = _self_trade_calibrator;
    pctx.use_toxicity_detector = _cfg.use_toxicity_detector;
    pctx.toxicity_cooloff_ms = _cfg.modules.toxicity_cooloff_ms;

    QuoteState qs;
    qs.skew = skew;
    qs.spread_mult = spread_mult;
    qs.l0_bid = l0_bid;
    qs.l0_ask = l0_ask;
    // allow 初始化: 使用TradingState查询方法 (毒性/ARB/LIMIT 抑制在此基础上叠加)
    qs.allow_bid = _trading_state ? _trading_state->canBuy() : true;
    qs.allow_ask = _trading_state ? _trading_state->canSell() : true;

    _quote_chain.run(pctx, qs);

    skew = qs.skew;
    spread_mult = qs.spread_mult;
    l0_bid = qs.l0_bid;
    l0_ask = qs.l0_ask;
    bool allow_bid = qs.allow_bid;
    bool allow_ask = qs.allow_ask;

    //==========================================================================
    // 4. 执行报价发布
    //==========================================================================
    // Pending drain: per-side pending 超限 -> 撤该侧旧单 + 该侧 allow=false
    if (_v3_pending_drain_bid || _v3_pending_drain_ask) {
        if (_v3_pending_drain_bid) {
            tc.quoter->cancelSide(ctx, true);
            allow_bid = false;
        }
        if (_v3_pending_drain_ask) {
            tc.quoter->cancelSide(ctx, false);
            allow_ask = false;
        }
        WTSLogger::debug("[DRAIN] {} bid_drain={} ask_drain={} -> cancel+skip drained side",
                         tc.code,
                         _v3_pending_drain_bid,
                         _v3_pending_drain_ask);
    }

    // v7.1: 缓存最终报价参数, 供 requoteAfterFill 成交后立即重挂使用
    {
        RecursiveSpinGuard _g(_last_quote_lock);
        CachedQuote& cq = _last_quote_params[tc.code];
        cq.mid = tc.mid;
        cq.l0_bid = l0_bid;
        cq.l0_ask = l0_ask;
        cq.spread_mult = spread_mult;
        cq.allow_bid = allow_bid;
        cq.allow_ask = allow_ask;
        cq.long_util = _v3_long_util;
        cq.short_util = _v3_short_util;
        cq.force_ask_obligation = _v3_force_ask_obligation;
        cq.force_bid_obligation = _v3_force_bid_obligation;
        cq.hard_block_bid = _v3_hard_block_bid;
        cq.hard_block_ask = _v3_hard_block_ask;
        cq.upper_limit = tick->upperlimit();
        cq.lower_limit = tick->lowerlimit();
        cq.best_bid = tick->bidprice(0);
        cq.best_ask = tick->askprice(0);
        cq.timestamp = tc.timestamp;
        cq.valid = true;
    }

    tc.quoter->refreshQuotes(ctx,
                             tc.mid,
                             l0_bid,
                             l0_ask,
                             spread_mult,
                             allow_bid,
                             allow_ask,
                             tc.timestamp,
                             tick->upperlimit(),
                             tick->lowerlimit(),
                             tick->bidprice(0),
                             tick->askprice(0),
                             _v3_long_util,
                             _v3_short_util,
                             _v3_force_ask_obligation,
                             _v3_force_bid_obligation,
                             _v3_hard_block_bid,
                             _v3_hard_block_ask);

    return true;
}
//==========================================================================
// Stage 6: Process Auto-cancel
//==========================================================================

bool StrategyCoordinator::processAutoCancel(wtp::IUftStraCtx* ctx, const TickContext& tc)
{
    if (!_order_tracker)
        return false;

    double tick_size = tc.tick_size > 0 ? tc.tick_size : 1.0;

    // Check auto-cancel on the tracker directly
    bool inventory_hit = _portfolio ? _portfolio->isAnyLimitBreached() : false;
    double current_risk_delta = tc.total_delta; // perf#4: Stage 2 缓存值 (本 tick 持仓未变)

    const auto& actions = _order_tracker->checkAutoCancel(
        tc.code, tc.timestamp, tc.mid, tick_size, false, inventory_hit, current_risk_delta);

    if (!actions.empty()) {
        for (const auto& action : actions) {
            orderApiCall([&] { return ctx->stra_cancel(action.order_id); });
        }
        return true;
    }

    return false;
}

//==========================================================================
// Stage 4.5: v7.1 Taker 紧急减仓
//   合约 util ≥ taker_reduce_threshold (默认1.3) 时, FAK 对手价主动平掉
//   (|pos| - target×maxPos) 超出部分。应对"大量成交突然穿仓"场景:
//   被动减仓(skew穿越)回归太慢时主动吃单, 报价永不停(做市义务不受影响)。
//==========================================================================

bool StrategyCoordinator::checkTakerReduce(wtp::IUftStraCtx* ctx)
{
    if (!ctx || !_portfolio || !_order_router)
        return false;
    if (_cfg.taker_reduce_threshold <= 0.0)
        return false;

    // v7.1: 限频计时统一用 replay 时钟 (回测可复现); 未注入时回退墙钟
    uint64_t now_ms = _last_exchange_time_ms > 0 ? _last_exchange_time_ms : TimeUtils::getLocalTimeNow();
    bool triggered = false;

    for (const auto& c : _portfolio->getAllContractsSnapshot()) {
        if (c.max_position <= 0 || std::abs(c.position) < 1.0)
            continue;

        double util = std::abs(c.position) / c.max_position;
        if (util < _cfg.taker_reduce_threshold)
            continue;

        // 每合约限频
        auto it = _last_taker_reduce.find(c.code);
        if (it != _last_taker_reduce.end() && now_ms - it->second < _cfg.taker_reduce_cooldown_ms) {
            continue;
        }

        // 平掉超出 target×maxPos 的部分 (FAK 对手价, 不追价)
        double target = c.max_position * _cfg.taker_reduce_target_util;
        double qty = std::floor(std::abs(c.position) - target);
        if (qty < 1.0)
            continue;

        bool is_long = c.position > 0;
        double price = is_long ? c.bid1 : c.ask1; // 对手价
        if (price <= 0)
            continue;

        WTSLogger::warn("[TAKER_REDUCE] {} util={:.2f} >= {:.2f}: {} {:.0f}@{} (pos={:.0f}/{:.0f} -> target={:.0f})",
                        c.code,
                        util,
                        _cfg.taker_reduce_threshold,
                        is_long ? "SELL_CLOSE" : "BUY_CLOSE",
                        qty,
                        price,
                        c.position,
                        c.max_position,
                        target);

        OrderSubmitResult rr = is_long ? _order_router->submitSell(ctx, c.code.c_str(), price, qty, Source::CLOSEOUT, 1)
                                       : _order_router->submitBuy(ctx, c.code.c_str(), price, qty, Source::CLOSEOUT, 1);

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
            if (_risk_monitor)
                _risk_monitor->recordOrder();
            triggered = true;
        } else {
            WTSLogger::error("[TAKER_REDUCE] {} order FAILED", c.code);
        }
    }

    return triggered;
}

//==========================================================================
// v7.1: 成交后立即重挂 (做市义务恢复)
//   单边成交把该侧挂单深度侵蚀到 min_valid_qty 以下 → 双边义务不满足 →
//   立即撤剩余单 + 按最近 tick 参数重新挂单, 不再等下一个 tick。
//   obligation 路径 (always_obligation&&L0) 本身即"先撤残留再双边下单",
//   天然满足"撤剩余单重新挂单"语义。
//==========================================================================

bool StrategyCoordinator::requoteAfterFill(wtp::IUftStraCtx* ctx, const std::string& code, uint64_t now_ms)
{
    if (!ctx || !_quoters)
        return false;
    if (_cfg.requote_after_fill_min_interval_ms == 0)
        return false;
    // 报价状态门: RISK_HALTED/MARKET/TOXICITY 等期间不补挂 (与 processQuoting 同门)
    if (!_trading_state || !_trading_state->canQuote())
        return false;
    // 风控 HALT 显式拦截: haltTrading() 只置 _trading_halted, 不联动 qphase=RISK_HALTED,
    // 故 canQuote() 仍 true -> 旧实现 HALT 期间仍重挂(13:50 后 943 笔成交绕过风控).
    if (_risk_monitor && _risk_monitor->isTradingHalted())
        return false;

    auto it = _quoters->find(code);
    if (it == _quoters->end() || !it->second)
        return false;
    FutuQuoter* quoter = it->second.get();

    // 义务检查: 双边深度是否仍满足 min_valid_qty; 满足则无需动作
    ValidQuoteSnapshot snap = quoter->getValidQuoteSnapshot();
    if (snap.has_valid_bid && snap.has_valid_ask)
        return false;

    // v7.6: 锁内取快照, 释放锁后再 refreshQuotes (防与 quoter 锁嵌套)
    CachedQuote q;
    {
        RecursiveSpinGuard _g(_last_quote_lock);
        auto qit = _last_quote_params.find(code);
        if (qit == _last_quote_params.end() || !qit->second.valid)
            return false;
        q = qit->second;
    }

    // 限频防 churn (密集成交时合并重挂)
    uint64_t& last = _last_requote_ms[code];
    if (last > 0 && now_ms > last && now_ms - last < _cfg.requote_after_fill_min_interval_ms)
        return false;
    last = now_ms;

    // ===== [FIX] 成交后按最新价重算报价+数量, 并作用 retreat =====
    // 旧实现直接用缓存 q.l0_bid/q.l0_ask (上一 processQuoting tick、本次成交前的价格)
    // 重挂, 绕过 QuotePolicyChain/FillRetreatPolicy -> 旧价即挂即成交 -> 死循环
    // (11:01 ao2610 ~500手/90s)。改为: (1) 最新 mid 重算 l0_bid/l0_ask;
    // (2) 重跑 checkPreTradePosition 刷 util/hard_block/obligation(数量据此重算);
    // (3) getFillRetreat 与新算报价比较, 取更保守价(退价防立即再成交)。

    // (1) 最新 mid (行情可能已动, 不用缓存旧 mid); 按 mid 平移重算, 保留原 spread/skew 结构
    double latest_mid = q.mid;
    auto mid_it = _last_mid.find(code);
    if (mid_it != _last_mid.end() && mid_it->second > 0)
        latest_mid = mid_it->second;
    double mid_delta = latest_mid - q.mid;
    double new_l0_bid = q.l0_bid + mid_delta;
    double new_l0_ask = q.l0_ask + mid_delta;

    // (2) 重跑 pre-trade 风控: 成交后持仓已变, util/hard_block/obligation 必须刷新(数量据此重算)
    double long_util = q.long_util;
    double short_util = q.short_util;
    bool force_ask_obligation = q.force_ask_obligation;
    bool force_bid_obligation = q.force_bid_obligation;
    bool hard_block_bid = q.hard_block_bid;
    bool hard_block_ask = q.hard_block_ask;
    bool allow_bid = q.allow_bid;
    bool allow_ask = q.allow_ask;
    if (_risk_monitor && _portfolio && _order_tracker) {
        auto pre = _risk_monitor->checkPreTradePosition(code, _portfolio, _order_tracker);
        long_util = pre.long_utilization;
        short_util = pre.short_utilization;
        force_ask_obligation = pre.force_ask_obligation;
        force_bid_obligation = pre.force_bid_obligation;
        hard_block_bid = pre.hard_block_bid;
        hard_block_ask = pre.hard_block_ask;
        // pending_drain 覆盖 allow (与 processQuoting 的 drain 逻辑一致)
        if (pre.pending_drain_bid)
            allow_bid = false;
        if (pre.pending_drain_ask)
            allow_ask = false;
    }

    // (3) 作用 retreat: 与新算报价比较, 取更保守价
    //     买单成交 -> bid <= 成交价-retreat_ticks; 卖单成交 -> ask >= 成交价+retreat_ticks
    //     (getFillRetreat 返回价已 on-tick: 内部 retreat_ticks*tick_size)
    if (_self_trade_calibrator) {
        FillRetreat retreat = _self_trade_calibrator->getFillRetreat(code, now_ms);
        if (retreat.bid_retreat_active && new_l0_bid > retreat.bid_retreat_price)
            new_l0_bid = retreat.bid_retreat_price;
        if (retreat.ask_retreat_active && new_l0_ask < retreat.ask_retreat_price)
            new_l0_ask = retreat.ask_retreat_price;
    }

    WTSLogger::debug("[REQUOTE] {} fill eroded obligation depth (bid_valid={} ask_valid={}), "
                     "re-quoting: fresh mid={:.2f} (cached={:.2f}) delta={:.2f}, retreat applied, "
                     "util L={:.2f}/S={:.2f} hardB={}/hardA={}",
                     code,
                     snap.has_valid_bid,
                     snap.has_valid_ask,
                     latest_mid,
                     q.mid,
                     mid_delta,
                     long_util,
                     short_util,
                     hard_block_bid,
                     hard_block_ask);

    quoter->refreshQuotes(ctx,
                          latest_mid,
                          new_l0_bid,
                          new_l0_ask,
                          q.spread_mult,
                          allow_bid,
                          allow_ask,
                          q.timestamp,
                          q.upper_limit,
                          q.lower_limit,
                          q.best_bid,
                          q.best_ask,
                          long_util,
                          short_util,
                          force_ask_obligation,
                          force_bid_obligation,
                          hard_block_bid,
                          hard_block_ask);
    return true;
}

//==========================================================================
// Stage 7.5: Position Reduction — REMOVED
// Replaced by enhanced skew (clamp + inventory_skew_scale) which drives
// ask to mid for natural passive reduction, avoiding 3-tick cross-spread cost.
//==========================================================================

//==========================================================================
// Stage 8: Update Adaptive Parameters
//==========================================================================

void StrategyCoordinator::updateAdaptiveParams(wtp::IUftStraCtx* ctx, const TickContext& tc)
{
    if (_tick_count % _cfg.param_update_interval != 0) {
        return;
    }

    // Adaptive parameter update placeholder
    // This is a placeholder - actual implementation would record performance
    // and update parameters based on the manager's logic
}

//==========================================================================
// Reset
//==========================================================================

void StrategyCoordinator::resetSession()
{
    // P1-1: reset() replaces old resume() — resets to QUOTING+NORMAL
    if (_trading_state) {
        _trading_state->reset();
    }
    _quote_chain.toxicity().reset();
    _tick_count = 0;
    _last_mid.clear();
}

void StrategyCoordinator::resetDaily()
{
    resetSession();
    if (_risk_monitor) {
        _risk_monitor->resetDaily();
    }
}

} // namespace futu