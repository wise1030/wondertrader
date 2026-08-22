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
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace futu
{

namespace
{
/// stdCode ("SHFE.ao.ao2609" 三段式) -> fullCode ("SHFE.ao2609" 两段式).
/// live TraderAdapter::cancelAll 用 getFullCode() 匹配入参 (框架坑, 见 AGENTS.md
/// 已知外部限制); 已是两段式或无点的输入原样返回。
std::string stdCodeToFullCode(const std::string& stdCode)
{
    auto p1 = stdCode.find('.');
    auto p2 = stdCode.rfind('.');
    if (p1 == std::string::npos || p2 == p1)
        return stdCode;
    return stdCode.substr(0, p1 + 1) + stdCode.substr(p2 + 1);
}

struct BilateralSeed
{
    uint64_t bil = 0;
    uint64_t ses = 0;
    uint64_t smp = 0;
    uint32_t sw = 0;
    double avg = 0.0;
    uint64_t inv[5] = {0, 0, 0, 0, 0};
    bool valid = false;
};

std::string makeBilateralFilePath(const std::string& dir, uint32_t tdate)
{
    return dir + "/bilateral_stats_" + std::to_string(tdate) + ".log";
}

void ensureBilateralDir(const std::string& dir)
{
    if (dir.empty())
        return;
    ::mkdir(dir.c_str(), 0755);
}

void appendBilateralLine(const std::string& dir, uint32_t tdate, const std::string& line)
{
    if (line.empty() || tdate == 0)
        return;

    ensureBilateralDir(dir);
    std::string path = makeBilateralFilePath(dir, tdate);
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;

    std::string out = line;
    out.push_back('\n');
    ssize_t n = ::write(fd, out.data(), out.size());
    (void)n;
    ::close(fd);
}

std::unordered_map<std::string, std::string> loadLastBilateralLines(const std::string& dir, uint32_t tdate)
{
    std::unordered_map<std::string, std::string> result;
    if (tdate == 0)
        return result;

    std::ifstream in(makeBilateralFilePath(dir, tdate));
    if (!in.is_open())
        return result;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        auto code_pos = line.find(" code=");
        if (code_pos == std::string::npos)
            continue;
        size_t start = code_pos + 6;
        size_t end = line.find(' ', start);
        if (end == std::string::npos)
            end = line.size();
        result[line.substr(start, end - start)] = line;
    }
    return result;
}

BilateralSeed parseBilateralLine(const std::string& line)
{
    BilateralSeed seed;
    unsigned long long bil = 0, ses = 0, smp = 0;
    unsigned long long inv0 = 0, inv1 = 0, inv2 = 0, inv3 = 0, inv4 = 0;
    unsigned int sw = 0;
    double avg = 0.0;
    int n = std::sscanf(line.c_str(),
                        "ts=%*u:%*u type=%*s code=%*s bil=%llu ses=%llu ratio=%*f avg=%lf smp=%llu sw=%u inv=%llu,%llu,%llu,%llu,%llu",
                        &bil,
                        &ses,
                        &avg,
                        &smp,
                        &sw,
                        &inv0,
                        &inv1,
                        &inv2,
                        &inv3,
                        &inv4);
    seed.bil = bil;
    seed.ses = ses;
    seed.smp = smp;
    seed.sw = sw;
    seed.avg = avg;
    seed.inv[0] = inv0;
    seed.inv[1] = inv1;
    seed.inv[2] = inv2;
    seed.inv[3] = inv3;
    seed.inv[4] = inv4;
    seed.valid = (n == 10);
    return seed;
}
} // namespace

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
    auto readString = [](wtp::WTSVariant* v, const char* key, const char* defVal) -> std::string {
        if (!v)
            return defVal ? defVal : "";
        wtp::WTSVariant* node = v->get(key);
        return node ? node->asCString() : (defVal ? defVal : "");
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
        // spreadOptimizer 是策略核心，恒启用（不再提供 enabled 开关）
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
            _cfg.modules.cancel_retry_interval_ms =
                readUInt32(autoCancel, "cancelRetryIntervalMs", _cfg.modules.cancel_retry_interval_ms);
            _cfg.modules.cancel_max_retries =
                readUInt32(autoCancel, "cancelMaxRetries", _cfg.modules.cancel_max_retries);
        }

        // SelfTradePrevention: 已迁移到 StpConfig::fromVariant


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

    // v7.9 session 休息段参数 (秒级; 新键 sectionBreakSecondsBefore 优先,
    // 旧键 sectionBreakMinutesBefore 按 min*60 兼容转换)
    if (cfg->has("sectionBreakSecondsBefore")) {
        _cfg.section_break_seconds_before = readUInt32(cfg, "sectionBreakSecondsBefore", _cfg.section_break_seconds_before);
    } else if (cfg->has("sectionBreakMinutesBefore")) {
        _cfg.section_break_seconds_before = readUInt32(cfg, "sectionBreakMinutesBefore", 0) * 60;
    }

    // 双边统计输出链路
    _cfg.bilateral_stats_log_interval_sec =
        readUInt32(cfg, "bilateralStatsLogIntervalSec", _cfg.bilateral_stats_log_interval_sec);
    _cfg.bilateral_stats_log_dir =
        readString(cfg, "bilateralStatsLogDir", _cfg.bilateral_stats_log_dir.c_str());

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
    pcfg.section_break_seconds_before = _cfg.section_break_seconds_before;
    pcfg.close_time = _cfg.close_time;
    pcfg.closeout_minutes_before = _cfg.closeout_minutes_before;
    pcfg.night_close_time = _cfg.night_close_time;
    pcfg.night_minutes_before = _cfg.night_minutes_before;
    _phase_mgr.configure(pcfg);
}

void StrategyCoordinator::setTradingState(TradingState* state)
{
    _trading_state = state;
    // P1.3 Step1: wire CloseoutTrigger. trading_state 是 coordinator 最后设置的依赖
    // (FutuModuleAssembler 顺序: setConfig/setQuoters/setPortfolio/setRiskMonitor 均先于 setTradingState)
    _closeout_trigger.setDeps({_risk_monitor, _trading_state, _portfolio, &_cfg,
        [this](wtp::IUftStraCtx* ctx) {
            if (_quoters) {
                for (auto& [code, q] : *_quoters) { if (q) q->cancelAll(ctx); }
            }
        },
        [this](wtp::IUftStraCtx* ctx, uint32_t hhmm, uint32_t secs) {
            flushBilateralStats(ctx, hhmm, secs);
        }});
    _risk_coord.setDeps({_portfolio, _order_router, _risk_monitor, _trading_state, _arb_executor,
        &_quote_chain, _self_trade_calibrator, &_cfg,
        [this](wtp::IUftStraCtx* ctx) {
            if (_quoters) {
                for (auto& [code, q] : *_quoters) { q->cancelAll(ctx); }
            }
        }});  // P1.3 Step2a+2b: wire RiskCoordinator
}

void StrategyCoordinator::wireDeps(const CoordinatorDeps& deps)
{
    // B7: Set all dependency pointers in one call
    _portfolio = deps.portfolio;
    _order_tracker = deps.order_tracker;
    _risk_monitor = deps.risk_monitor;
    _order_router = deps.order_router;
    _trading_state = deps.trading_state;
    _quoters = deps.quoters;
    _spread_opts = deps.spread_opts;
    _market_data = deps.market_data;
    _signal_aggregators = deps.signal_aggregators;
    _toxicity = deps.toxicity;
    _perf_monitor = deps.perf_monitor;
    _self_trade_calibrator = deps.self_trade_calibrator;
    _correlation_manager = deps.correlation_manager;
    _arb_executor = deps.arb_executor;
    _arb_manager = deps.arb_manager;

    // Wire sub-component deps (was in setTradingState - must be after all members set)
    _closeout_trigger.setDeps({_risk_monitor, _trading_state, _portfolio, &_cfg,
        [this](wtp::IUftStraCtx* ctx) {
            if (_quoters) {
                for (auto& [code, q] : *_quoters) { if (q) q->cancelAll(ctx); }
            }
        },
        [this](wtp::IUftStraCtx* ctx, uint32_t hhmm, uint32_t secs) {
            flushBilateralStats(ctx, hhmm, secs);
        }});
    _risk_coord.setDeps({_portfolio, _order_router, _risk_monitor, _trading_state, _arb_executor,
        &_quote_chain, _self_trade_calibrator, &_cfg,
        [this](wtp::IUftStraCtx* ctx) {
            if (_quoters) {
                for (auto& [code, q] : *_quoters) { q->cancelAll(ctx); }
            }
        }});

    // C12: pre-populate atomic _last_mid from _quoters
    initLastMid();
}

bool StrategyCoordinator::validateDeps() const
{
    uint32_t errors = 0;
    auto require = [this, &errors](const void* p, const char* name) {
        if (!p) { WTSLogger::error("[COORDINATOR] B7: required dependency missing: {}", name); errors++; }
    };
    require(_portfolio, "portfolio");
    require(_order_tracker, "order_tracker");
    require(_risk_monitor, "risk_monitor");
    require(_order_router, "order_router");
    require(_trading_state, "trading_state");
    if (_cfg.use_market_making) {
        require(_quoters, "quoters (MM enabled)");
        require(_spread_opts, "spread_opts (MM enabled)");
        require(_market_data, "market_data (MM enabled)");
        require(_signal_aggregators, "signal_aggregators (MM enabled)");
    }
    if (errors > 0)
        WTSLogger::error("[COORDINATOR] B7: {} required dependencies MISSING", errors);
    else
        WTSLogger::info("[COORDINATOR] B7: dependency check passed (0 missing)");
    return errors == 0;
}

void StrategyCoordinator::initLastMid()
{
    if (!_quoters) return;
    for (auto& [code, quoter] : *_quoters) {
        _last_mid[code] = std::make_unique<MidSlot>();
    }
    WTSLogger::info("[COORDINATOR] C12: _last_mid pre-populated with {} contracts (atomic MidSlot)", _last_mid.size());
}

void StrategyCoordinator::initialize()
{
    WTSLogger::info("StrategyCoordinator: initialized (perf={})", _cfg.perf_enabled);
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
    if (_closeout_trigger.process(ctx, tc)) {
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
                _risk_coord.checkRisk(ctx, tc, _toxicity && _quote_chain.toxicity().inCooloff(tc.timestamp));
            }
        }
        return result;
    }

    if (_cfg.use_market_making && _cfg.bilateral_stats_log_interval_sec > 0)
        logBilateralStatsPeriodic(ctx, tc);

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

    _event_dispatcher.dispatch(CoordinatorEvent::TickReceived);  // C10: fire at processTick entry

    // ===== Market Making Pipeline =====
    if (_cfg.use_market_making) {
        // Stage 3: Update signals (MM only)
        updateSignals(ctx, tc, tick);

        // Stage 4: Check risk
        if (!_risk_coord.checkRisk(ctx, tc, _toxicity && _quote_chain.toxicity().inCooloff(tc.timestamp))) {
            // BLOCK_SIDE / HALT 时仍执行 taker 减仓 - 减仓是恢复正常的关键手段.
            // 旧逻辑 HALT 时跳过 checkTakerReduce 是反的: 恰恰 HALT 时最需要强平减仓
            // (13:50 HALT 后 TAKER_REDUCE 不复燃, 持仓任由 requote 放大). requote 路径
            // 已独立拦截 HALT(见 requoteAfterFill), 此处只管主动减仓.
            _risk_coord.checkTakerReduce(ctx, _last_exchange_time_ms);
            result.processed = true;
            return result;
        }

        // Stage 5: Process auto-cancel (先撤旧单,再报新单)
        result.order_canceled = processAutoCancel(ctx, tc);

        // Stage 5.5: v7.1 taker 紧急减仓 (穿仓主动吃单; 报价不停, 与之并行)
        _risk_coord.checkTakerReduce(ctx, _last_exchange_time_ms);

        // Stage 6: Process quoting (MM core)
        result.quote_placed = processQuoting(ctx, tc, tick);
    }

    // ===== Arbitrage Pipeline =====
    // Note: Arbitrage processing is handled by UftFutuMmStrategy::processSpreadArbitrage
    // when use_spread_arbitrage is enabled

    // Stage 7.5: Position reduction removed — skew+clamp handles inventory reduction via quote offset
    // (attemptPositionReduction used 3-tick cross-spread which was too costly;
    //  enhanced skew with clamp+scale now drives ask to mid for natural reduction)

    // (V8-R3: Stage 8 adaptiveParam 空占位已删除)

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

        // P0-4: 每秒更新一次性能统计 + 阈值告警 (V8-R3: warn/critical 接线)
        if (tc.timestamp - _last_perf_ms >= 1000) {
            _perf_monitor->updatePerSecondCounters();
            _perf_monitor->checkThresholds(tc.timestamp);
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
    if (_cfg.section_break_seconds_before == 0)
        return false;

    // 5A-1: 窗口判定统一走 SessionPhaseManager; sess 指针取回缓存进 TickContext
    //   (F7: preCheck 复用; v7.7 A4: tc 改非 const 引用, 消除 const_cast)
    wtp::WTSSessionInfo* sess = _phase_mgr.getSession(tc.code);
    tc.session = sess;
    if (!sess)
        return false;

    // 当前分钟 (stra_get_time 返回 HHMM(4位), 兼容 HHMMSS)
    uint32_t cur_hhmm = (tc.time_hms >= 10000) ? tc.time_hms / 100 : tc.time_hms;

    // v7.9: 秒级窗口 — 当前秒内毫秒 stra_get_secs (ssmmm/1000 = 分钟内秒)
    uint32_t secs_in_min = ctx->stra_get_secs() / 1000;
    bool in_break = _phase_mgr.inSectionBreak(tc.code, tc.time_hms, secs_in_min);

    if (in_break) {
        if (!_section_break_active) {
            _section_break_active = true;
            WTSLogger::info("[SECTION_BREAK] {} entering break window at {} ({}s before section end), cancel all + "
                            "pause quoting/arb",
                            tc.code,
                            cur_hhmm,
                            _cfg.section_break_seconds_before);
            // 撤全部做市报价
            if (_quoters) {
                for (auto& [code, quoter] : *_quoters) {
                    if (quoter)
                        quoter->cancelAll(ctx);
                }
            }
            flushBilateralStats(ctx, cur_hhmm, secs_in_min);
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
    tc.last_px = tick->price(); // L0 触板判定用最新成交价 (无成交时段=0, 由 LimitPricePolicy 守卫)

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
        // 锁板单边盘口: 报价循环跳过, L0 触板撤单不会执行 -> 存量单残留。
        // 已知边角: 开盘/跳空即锁板时无前置触板 tick, 残留单挂板价队列,
        // 开板回落时可能被逆选择成交。此处仅告警 (严格撤单需 Quoter 级 cancelAll 入口)。
        // 注: tc.upper/lower_limit 与 tc.tick_size 在本分支之后才填充,
        //     直接读 tick, 且用精确比较 (成交价不可能越过板价)。
        double ul = tick->upperlimit(), ll = tick->lowerlimit();
        if (tc.last_px > 0 && ((ul > 0 && tc.last_px >= ul) || (ll > 0 && tc.last_px <= ll))) {
            if ((++_limit_skip_cnt & 0xFF) == 1) { // 每 256 次打一条,避免日志洪水
                WTSLogger::warn("StrategyCoordinator: {} one-sided book at limit (last={} ul={} ll={}), "
                                "quote cycle skipped, residual orders may remain (cnt={})",
                                tc.code, tc.last_px, ul, ll, _limit_skip_cnt);
            }
        }
        return false;
    }

    tc.mid = (tc.bid_px + tc.ask_px) / 2.0;

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
        _portfolio->setShadowFromEngine(tc.code,
                                        ctx->stra_get_local_position(tc.code.c_str()),
                                        ctx->stra_get_local_closeprofit(tc.code.c_str()),
                                        ctx->stra_get_local_posprofit(tc.code.c_str()));
        auto mid_it = _last_mid.find(tc.code);
        if (mid_it != _last_mid.end() && mid_it->second)
            mid_it->second->v.store(tc.mid, std::memory_order_relaxed);

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
        // V8-T1: ofi_component 在 slot 中已算好 (SignalAggregator.h lambda),
        // 此前漏填恒 0 -> PredictiveToxicity 的 toxic_side 永远为 0,
        // 毒性单边抑制从不生效 (恒走双边抑制分支)
        alpha_res.ofi_component = sig_ctx.alpha.ofi_component;
        alpha_res.is_strong_signal = sig_ctx.alpha.is_strong_signal;
        alpha_res.timestamp = sig_ctx.timestamp;

        TradeImbalanceResult trade_res;
        trade_res.net_flow = sig_ctx.trade_flow.net_flow;
        trade_res.imbalance_ratio = sig_ctx.trade_flow.net_flow_normalized;
        // V8-T7: large_trade_ratio 此前漏填恒 0, trade_toxicity 被 0.5 因子压半
        // (0.5+0.5×0 -> 0.5), 大单主导的毒性流被低估
        trade_res.large_trade_ratio = sig_ctx.trade_flow.large_trade_ratio;

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
            // V8-A6: 经显式入口写入 (单写者恢复), 不再反向裸写聚合器内部状态
            aggregator->updateToxicity(tox.toxic_score, tox.toxic_side, true);
        } else {
            // toxic_detected每tick重算，不复位锁存
            // 与should_pause相同的锁存: 只设true不复位false
            // 导致toxic_detected一旦被设就永久锁死
            aggregator->updateToxicity(tox.toxic_score, tox.toxic_side, false);
        }
    }
}

//==========================================================================
// Stage 4: Check Risk Limits
//==========================================================================

//==========================================================================
// Stage 5: Process Quoting
//==========================================================================

bool StrategyCoordinator::processQuoting(wtp::IUftStraCtx* ctx, TickContext& tc, wtp::WTSTickData* tick)
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
    // 2.5 v3/v7.1 软风控前置: 统一 delta 利用率口径 (2026-08-19 语义边界原则)
    //   (delta+同向pending×hr)/contract_max_delta — skew 归一化注入 + quoter qty衰减/义务 共用
    //   maxPosition 仅用于风控硬闸门 (checkHardPositionRisk 内 halt_quoting)
    //==========================================================================
    PreTradeDecision decision;
    if (_risk_monitor) {
        // A3: 优先复用 TickContext.cs (preCheck 入口已快照), 消除重复递归锁+拷贝
        decision = tc.cs_valid
            ? _risk_monitor->checkPreTradePosition(tc.cs, _order_tracker, tc.timestamp)
            : _risk_monitor->checkPreTradePosition(tc.code, _portfolio, _order_tracker, tc.timestamp);
        // v7.8: rate-limit HALT_QUOTING logs to enter/exit transitions only
        if (cs && cs->max_position > 0) {
            bool& last_halted = _halt_quoting_state[tc.code];
            if (decision.risk.halt_quoting && !last_halted) {
                WTSLogger::error("[RISK] {} HALT_QUOTING: net position {:.0f} exceeds maxPosition {:.0f} "
                                 "-> ALL quoting paused for this contract (risk control)",
                                 tc.code,
                                 cs->position,
                                 cs->max_position);
            } else if (!decision.risk.halt_quoting && last_halted) {
                WTSLogger::info("[RISK] {} HALT_QUOTING lifted: net position {:.0f} within maxPosition {:.0f} "
                                "-> quoting resumed",
                                tc.code,
                                cs->position,
                                cs->max_position);
            }
            last_halted = decision.risk.halt_quoting;
        }

        // v7.1: 带符号 delta 利用率注入 skew (正=多 负=空, 取较大侧)
        if (cs && cs->contract_max_delta > 0) {
            p_ctx.contract_delta_util = (decision.strategy.long_delta_util >= decision.strategy.short_delta_util)
                                            ? decision.strategy.long_delta_util
                                            : -decision.strategy.short_delta_util;
            p_ctx.contract_delta_util_valid = true;
        }
    }

    //==========================================================================
    // 3. 使用 SpreadOptimizer 计算动态 Skew 和价差倍数
    //==========================================================================
    double skew = 0.0;
    double spread_mult = 1.0;
    // spreadOptimizer 恒启用；仅在信号上下文缺失(sig_ctx==null)时退化为对称报价。
    // 退化价差使用配置 base_spread（来自恒存在的 SpreadOptimizer），不再硬编码 2.0。
    double fallback_spread = tc.spread_opt ? tc.spread_opt->getParams().base_spread : 2.0;
    double l0_bid = tc.mid - fallback_spread * tc.tick_size;
    double l0_ask = tc.mid + fallback_spread * tc.tick_size;

    if (tc.spread_opt && sig_ctx) {
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
    pctx.last_price = tc.last_px;
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
    if (decision.risk.pending_drain_bid || decision.risk.pending_drain_ask) {
        if (decision.risk.pending_drain_bid) {
            tc.quoter->cancelSide(ctx, true);
            allow_bid = false;
        }
        if (decision.risk.pending_drain_ask) {
            tc.quoter->cancelSide(ctx, false);
            allow_ask = false;
        }
        WTSLogger::debug("[DRAIN] {} bid_drain={} ask_drain={} -> cancel+skip drained side",
                         tc.code,
                         decision.risk.pending_drain_bid,
                         decision.risk.pending_drain_ask);
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
        cq.decision = decision;
        cq.upper_limit = tick->upperlimit();
        cq.lower_limit = tick->lowerlimit();
        cq.best_bid = tick->bidprice(0);
        cq.best_ask = tick->askprice(0);
        cq.timestamp = tc.timestamp;
        cq.valid = true;
    }

    // V8-P0-2: MM 报单计入 ORDER_RATE 频控 -- 此前 recordOrder 仅 taker/arb
    // 调用, 最高频的 MM 报单路径零计数, 频控对其失明; refreshQuotes 返回值
    // 为本轮实际挂单数 (handleObligation/Flexible/Bilateral 全部汇聚于此)
    {
        uint32_t orders_placed = tc.quoter->refreshQuotes(ctx, FutuQuoter::QuoteRequest{
            tc.mid, l0_bid, l0_ask, spread_mult, allow_bid, allow_ask,
            tc.timestamp, tick->upperlimit(), tick->lowerlimit(),
            tick->bidprice(0), tick->askprice(0),
            decision.risk, decision.strategy
        });
        if (orders_placed > 0 && _risk_monitor)
            _risk_monitor->recordOrders(orders_placed);
    }

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

    const auto& actions = _order_tracker->checkAutoCancel(
        tc.code, tc.timestamp, tc.mid, tick_size, false);

    //==========================================================================
    // B+: zombie 升级处置 — 告警 + 该合约 halt 闩锁 + stra_cancel_all(fullCode) 兜底.
    // 注意框架坑: live TraderAdapter::cancelAll 用 getFullCode()("SHFE.ao2609" 两段式)
    // 匹配入参, 传 stdCode("SHFE.ao.ao2609" 三段式)永不匹配且静默全不撤 — 必须转
    // fullCode (回测 mocker 用 stdCode 匹配, 行为不一致, 见 AGENTS.md 已知外部限制).
    //==========================================================================
    const auto& zcodes = _order_tracker->getZombieEscalations();
    for (const auto& zcode : zcodes) {
        std::string fc = stdCodeToFullCode(zcode);
        WTSLogger::error("[RISK] {} ZOMBIE: cancel retries exhausted -> halt contract quoting + engine cancelAll({})",
                         zcode,
                         fc);
        if (_risk_monitor)
            _risk_monitor->setZombieHalt(zcode);
        orderApiCall([&] { return ctx->stra_cancel_all(fc.c_str()); });
    }

    // B+ 修复(P2-3): zombie 闩锁自动恢复 -- 兜底 cancelAll 杀掉 zombie 且回报清账后
    // (存活 zombie 合约集合不再含该合约, tracker 每次 checkAutoCancel 刷新),
    // 释放该合约的 halt 闩锁; 无断连场景 (流控吞单) 无需等通道重连/重启。
    if (_risk_monitor)
        _risk_monitor->retainZombieHalts(_order_tracker->getAliveZombieContracts());

    if (!actions.empty()) {
        for (const auto& action : actions) {
            // B+: STALE 等动作 tracker 在发出时已标记 pendingCancel(含撤单时刻);
            // TIMEOUT 重试动作本就在 pendingCancel 态 — 均直接发撤单即可。
            orderApiCall([&] { return ctx->stra_cancel(action.order_id); });
        }
        return true;
    }

    return !zcodes.empty();
}

//==========================================================================
// Stage 4.5: v7.1 Taker 紧急减仓
//   合约 util ≥ taker_reduce_threshold (默认1.3) 时, FAK 对手价主动平掉
//   (|pos| - target×maxPos) 超出部分。应对"大量成交突然穿仓"场景:
//   被动减仓(skew穿越)回归太慢时主动吃单, 报价永不停(做市义务不受影响)。
//==========================================================================

//==========================================================================
// v7.1: 成交后立即重挂 (做市义务恢复)
//   单边成交把该侧挂单深度侵蚀到 min_valid_qty 以下 → 双边义务不满足 →
//   立即撤剩余单 + 按最近 tick 参数重新挂单, 不再等下一个 tick。
//   obligation 路径 (obligation level) 本身即"先撤残留再双边下单",
//   天然满足"撤剩余单重新挂单"语义。
//==========================================================================

bool StrategyCoordinator::requoteAfterFill(wtp::IUftStraCtx* ctx, const std::string& code, uint64_t now_ms, bool from_fill)
{
    // from_fill=false: B+ 撤单终态回报触发的事件驱动补挂 — 不是成交, 不发 FillReceived
    if (from_fill)
        _event_dispatcher.dispatch(CoordinatorEvent::FillReceived); // C10: fire at fill entry
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
    if (mid_it != _last_mid.end() && mid_it->second) {
        double cached = mid_it->second->v.load(std::memory_order_relaxed);
        if (cached > 0)
            latest_mid = cached;
    }
    double mid_delta = latest_mid - q.mid;
    double new_l0_bid = q.l0_bid + mid_delta;
    double new_l0_ask = q.l0_ask + mid_delta;

    // (2) 重跑 pre-trade 风控: 成交后持仓已变, util/hard_block/obligation 必须刷新(数量据此重算)
    PreTradeDecision decision = q.decision;
    bool allow_bid = q.allow_bid;
    bool allow_ask = q.allow_ask;
    if (_risk_monitor && _portfolio && _order_tracker) {
        decision = _risk_monitor->checkPreTradePosition(code, _portfolio, _order_tracker, now_ms);
        // pending_drain 覆盖 allow (与 processQuoting 的 drain 逻辑一致)
        if (decision.risk.pending_drain_bid)
            allow_bid = false;
        if (decision.risk.pending_drain_ask)
            allow_ask = false;
    }

    // 风控: 同侧连续成交熔断暂停期不重挂（防止 fill→requote→fill 循环）。
    // 暂停期过后 verdict 自动失效, 报价随下一 tick / 后续成交恢复。
    if (decision.risk.side_pause_bid || decision.risk.side_pause_ask) {
        WTSLogger::warn("[RISK] {} requoteAfterFill skipped: side fill breaker pause active (bid={} ask={})",
                        code,
                        decision.risk.side_pause_bid,
                        decision.risk.side_pause_ask);
        return false;
    }

    // (3) 作用 retreat: 与新算报价比较, 取更保守价
    //     买单成交 -> bid <= 成交价-retreat_ticks; 卖单成交 -> ask >= 成交价+retreat_ticks
    //     (getFillRetreat 返回价已 on-tick: 内部 retreat_ticks*tick_size)
    bool retreat_bid_active = false;
    bool retreat_ask_active = false;
    bool retreat_bid_applied = false;
    bool retreat_ask_applied = false;
    if (_self_trade_calibrator) {
        FillRetreat retreat = _self_trade_calibrator->getFillRetreat(code, now_ms);
        retreat_bid_active = retreat.bid_retreat_active;
        retreat_ask_active = retreat.ask_retreat_active;
        if (retreat.bid_retreat_active && new_l0_bid > retreat.bid_retreat_price) {
            new_l0_bid = retreat.bid_retreat_price;
            retreat_bid_applied = true;
        }
        if (retreat.ask_retreat_active && new_l0_ask < retreat.ask_retreat_price) {
            new_l0_ask = retreat.ask_retreat_price;
            retreat_ask_applied = true;
        }
    }

    WTSLogger::debug("[REQUOTE] {} fill eroded obligation depth (bid_valid={} ask_valid={}), "
                     "re-quoting: fresh mid={:.2f} (cached={:.2f}) delta={:.2f} -> bid={:.2f} ask={:.2f}, "
                     "retreat bidActive={}/bidApplied={} askActive={}/askApplied={}, "
                     "util L={:.2f}/S={:.2f} hardB={}/hardA={}",
                     code,
                     snap.has_valid_bid,
                     snap.has_valid_ask,
                     latest_mid,
                     q.mid,
                     mid_delta,
                     new_l0_bid,
                     new_l0_ask,
                     retreat_bid_active,
                     retreat_bid_applied,
                     retreat_ask_active,
                     retreat_ask_applied,
                     decision.strategy.long_delta_util,
                     decision.strategy.short_delta_util,
                     decision.strategy.block_add_long,
                     decision.strategy.block_add_short);

    // V8-P0-2: requoteAfterFill 补挂单同样计入频控 (实盘跑 TdSpi 事件驱动,
    // 批量接口内部自旋锁保证多线程计数安全)
    {
        uint32_t orders_placed = quoter->refreshQuotes(ctx, FutuQuoter::QuoteRequest{
            latest_mid, new_l0_bid, new_l0_ask, q.spread_mult, allow_bid, allow_ask,
            q.timestamp, q.upper_limit, q.lower_limit, q.best_bid, q.best_ask,
            decision.risk, decision.strategy
        });
        if (orders_placed > 0 && _risk_monitor)
            _risk_monitor->recordOrders(orders_placed);
    }
    return true;
}

//==========================================================================
// Stage 7.5: Position Reduction — REMOVED
// Replaced by enhanced skew (clamp + inventory_skew_gain, delta 口径) which drives
// ask to mid for natural passive reduction, avoiding 3-tick cross-spread cost.
//==========================================================================

//==========================================================================
// Stage 8: Update Adaptive Parameters
//==========================================================================


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
    for (auto& [code, slot] : _last_mid) { if (slot) slot->v.store(0.0, std::memory_order_relaxed); }
}

void StrategyCoordinator::resetDaily()
{
    resetSession();
    if (_risk_monitor) {
        _risk_monitor->resetDaily();
    }
}

uint32_t StrategyCoordinator::tradingDateOf(uint32_t wallDate, uint32_t hhmm)
{
    if (wallDate == 0)
        return 0;
    // 夜盘 21:00-23:59 归属次一交易日；00:00 后 wall 日期已天然等于交易日期
    return (hhmm >= 2100) ? TimeUtils::getNextDate(wallDate) : wallDate;
}

void StrategyCoordinator::flushBilateralStats(wtp::IUftStraCtx* ctx, uint32_t hhmm, uint32_t secs)
{
    if (!ctx || !_quoters)
        return;

    // 白盘/夜盘统一 flush：夜盘 closeout(00:55) 与 section-break(00:59:50) 双行
    // 增量≈0 互为确认，无过滤需求
    uint32_t tdate = tradingDateOf(ctx->stra_get_date(), hhmm);
    if (tdate == 0)
        return;

    for (auto& [code, quoter] : *_quoters) {
        if (!quoter)
            continue;
        auto& stats = quoter->getBilateralStats();
        if (!stats.hasSessionInfo())
            continue;

        std::string line = stats.flushSection(hhmm, secs);
        if (!line.empty()) {
            WTSLogger::info("[BILATERAL_STATS] {}", line);
            appendBilateralLine(_cfg.bilateral_stats_log_dir, tdate, line);
        }
    }
}

void StrategyCoordinator::logBilateralStatsPeriodic(wtp::IUftStraCtx* ctx, const TickContext& tc)
{
    if (!ctx || !_quoters || _cfg.bilateral_stats_log_interval_sec == 0)
        return;

    auto qit = _quoters->find(tc.code);
    if (qit == _quoters->end() || !qit->second)
        return;

    auto& stats = qit->second->getBilateralStats();
    if (!stats.hasSessionInfo())
        return;

    uint64_t now_ms = tc.timestamp > 0 ? tc.timestamp : TimeUtils::getLocalTimeNow();
    uint64_t& last = _last_bilateral_log_ms[tc.code];
    uint64_t interval_ms = static_cast<uint64_t>(_cfg.bilateral_stats_log_interval_sec) * 1000;
    if (last > 0 && now_ms > last && now_ms - last < interval_ms)
        return;

    uint32_t cur_hhmm = (tc.time_hms >= 10000) ? tc.time_hms / 100 : tc.time_hms;
    uint32_t secs_in_min = ctx->stra_get_secs() / 1000;
    std::string line = stats.formatLiveString(cur_hhmm, secs_in_min);
    if (line.empty())
        return;

    last = now_ms;
    WTSLogger::info("[BILATERAL_STATS] {}", line);
    appendBilateralLine(_cfg.bilateral_stats_log_dir, tradingDateOf(tc.date, cur_hhmm), line);
}

void StrategyCoordinator::seedBilateralStatsFromFile(uint32_t tdate)
{
    if (!_quoters || tdate == 0)
        return;

    auto last_lines = loadLastBilateralLines(_cfg.bilateral_stats_log_dir, tdate);
    if (last_lines.empty())
        return;

    for (auto& [code, quoter] : *_quoters) {
        if (!quoter)
            continue;
        auto it = last_lines.find(code);
        if (it == last_lines.end())
            continue;

        BilateralSeed seed = parseBilateralLine(it->second);
        if (!seed.valid)
            continue;
        quoter->getBilateralStats().seedFrom(seed.bil, seed.ses, seed.avg, seed.smp, seed.sw, seed.inv);
    }
}

} // namespace futu
