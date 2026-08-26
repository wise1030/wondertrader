/*!
* \file UftFutuMmStrategy.cpp
* \brief GLFT+Alpha Market-Making Strategy Implementation (as UFT Strategy)
*
* 集成业务模块：
*   - FutuPortfolio: 持仓管理、Delta计算、对冲
*   - FutuQuoter: 多档位报价执行
*   - SpreadOptimizer: GLFT价差优化
*   - MicroAlphaEngine: Alpha预测引擎
*   - ToxicFlowDetector: VPIN毒性检测
*   - AutoCancelPolicy: 自动撤单策略
*   - FutuRiskMonitor: 风险监控
*   - Level2DataAdapter: Level2数据适配
*   - PerformanceAnalyzer: 绩效分析
*/
#include "UftFutuMmStrategy.h"
#include "FutuModuleAssembler.h"
#include "FutuRuntimeOps.h"
#include "../WtUftCore/UftStraContext.h" // R1: dynamic_cast 获取 EventNotifier
#include "../Includes/IUftStraCtx.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSMarcos.h"
#include "../WTSTools/WTSLogger.h"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/TimeUtils.hpp"

// 业务模块头文件
#include "FutuPortfolio.h"
#include "FutuQuoter.h"
#include "SpreadOptimizer.h"

#include "UnifiedOrderTracker.h"
#include "MarketDataContext.h"
#include "CloseoutExecutor.h"
#include "FutuRiskMonitor.h"
#include "ToxicFlowDetector.h"
#include "PerformanceAnalyzer.h"
#include "PerformanceMonitor.h"
#include "TscClock.h"
#include "RiskLiquidator.h"
#include "SpreadArbitrageManager.h"
#include "FutuComponentFactory.h"
#include "FutuConfigLoader.h"
#include "SelfTradePrevention.h"
#include "OrderRouter.h"
#include "StrategyCoordinator.h"
#include "AsyncArbitrageExecutor.h"
#include "BilateralQuoteStats.h"
#include "CorrelationManager.h"

// 综合信号组件头文件
#include "TickTransactionInferer.h"
#include "SelfTradeCalibrator.h"
#include "SignalAggregator.h" // 新增：信号聚合器
#include "FutuConfigValidator.h" // 配置校验

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <chrono>

//==========================================================================
// V8-R4/P0-3 + V8-R6/WS-F: 回调串行化两态编译开关 (默认 big 与历史基线一致)。
// 历史: P0-3 曾删除 FUTU_CALLBACK_LOCK=0 细粒度开关 (=0 存在 ≥6 处假无锁);
// R6 起在 WS-A(停机域收编)/WS-E(命令化单飞)/写点清单完备的前提下恢复两态,
// 切 none 前必须: 全量 TestUnits TSAN 构建 + _ec_5d 逐比特 A/B + 灰度 p99/p999
// 验收 (含 refreshQuotes 持锁时长埋点, 见去大锁方案节) —— 缺一不可。
//==========================================================================
#ifndef FUTU_CB_LOCK_BIG
#define FUTU_CB_LOCK_BIG 1
#endif
#if FUTU_CB_LOCK_BIG
#define FUTU_CB_LOCK_GUARD() std::lock_guard<std::recursive_mutex> _cb_lock(_cb_mtx)
#else
#define FUTU_CB_LOCK_GUARD()
#endif

namespace futu
{

//==========================================================================
// 辅助函数：从 WTSVariant 读取参数（带默认值）
//==========================================================================

namespace
{

// 读取 double 参数
double readDouble(WTSVariant* cfg, const char* key, double defVal)
{
    if (!cfg)
        return defVal;
    WTSVariant* node = cfg->get(key);
    return node ? node->asDouble() : defVal;
}

// 读取 uint32_t 参数
uint32_t readUInt32(WTSVariant* cfg, const char* key, uint32_t defVal)
{
    if (!cfg)
        return defVal;
    WTSVariant* node = cfg->get(key);
    return node ? node->asUInt32() : defVal;
}

// 读取 bool 参数
bool readBool(WTSVariant* cfg, const char* key, bool defVal)
{
    if (!cfg)
        return defVal;
    WTSVariant* node = cfg->get(key);
    return node ? node->asBoolean() : defVal;
}

// 读取 string 参数
std::string readString(WTSVariant* cfg, const char* key, const char* defVal)
{
    if (!cfg)
        return defVal;
    WTSVariant* node = cfg->get(key);
    return node ? node->asString() : defVal;
}

/**
* @brief 将 fullCode 转换为 stdCode 格式
*
* fullCode 格式: SHFE.ag2606 (交易所.合约代码)
* stdCode 格式:  SHFE.ag.2606 (交易所.品种.月份)
*
* 使用 CodeHelper::rawMonthCodeToStdCode 实现转换
*/
std::string fullCodeToStdCode(const std::string& fullCode)
{
    // 查找第一个点，分离交易所和合约代码
    size_t firstDot = fullCode.find('.');
    if (firstDot == std::string::npos)
        return fullCode; // 没有点，直接返回

    std::string exchg = fullCode.substr(0, firstDot);
    std::string code = fullCode.substr(firstDot + 1);

    // 使用 CodeHelper 进行转换
    return CodeHelper::rawMonthCodeToStdCode(code.c_str(), exchg.c_str());
}

} // anonymous namespace

//==========================================================================
// 构造/析构
//==========================================================================

UftFutuMmStrategy::UftFutuMmStrategy(const char* id)
    : UftStrategy(id), _trading_state() // TradingState default-constructs all false
      ,
      _tick_count(0), _param_update_interval(100)
      // v7.6: _channel_ready/_order_error_count/_quoting_paused_since/
      //       _exchange_time_ms/_portfolio_ctx_dirty 均为原子成员, 类内初始化
      ,
      _hot_mgr()
{}

UftFutuMmStrategy::~UftFutuMmStrategy() {}

//==========================================================================
// 初始化
//==========================================================================

bool UftFutuMmStrategy::init(WTSVariant* cfg)
{
    if (!cfg)
        return false;

    // 配置解析与校验已拆分至 FutuConfigLoader (架构重构 C1)
    return FutuConfigLoader::load(cfg, _config, _contract_infos, id());
}

//==========================================================================
// R1: 告警通道注入 + 套利 alert 转发 (策略层作为组合根)
//==========================================================================

void UftFutuMmStrategy::setEventNotifier(wtp::EventNotifier* notifier)
{
    _event_notifier = notifier;
    // RiskMonitor: 直达 (既有 22 处 broadcastAlert 立即生效)
    if (_risk_monitor)
        _risk_monitor->setEventNotifier(notifier);
    // ArbManager: 经回调 → 本策略 handleRiskAlert → EventNotifier
    // (解耦: ArbManager 不直接依赖 EventNotifier, 保持模块边界清晰)
    if (_spread_arb_manager) {
        _spread_arb_manager->setAlertCallback([this](const RiskAlert& alert) { this->handleRiskAlert(alert); });
    }
}

void UftFutuMmStrategy::handleRiskAlert(const RiskAlert& alert)
{
    // 2026-08-26: WARNING 级持续态告警 (相关性破裂等) 按 pair+type 60s 节流 --
    // 原实现每 tick 打一行, 2026-08-26 单日 3.4 万行; 附带 value/threshold 便于核对。
    // CRITICAL/EMERGENCY 不节流。EMERGENCY 处置逻辑 (halt+disable) 不受影响。
    bool throttled = false;
    if (alert.level == RiskAlert::Level::WARNING) {
        const int64_t now_ms = TimeUtils::getLocalTimeNow();
        const std::string key = fmt::format("{}#{}", alert.pair_id, static_cast<int>(alert.type));
        SpinLockGuard _g(_arb_alert_lock);
        int64_t& last = _arb_alert_last_ms[key];
        if (now_ms - last < 60000)
            throttled = true;
        else
            last = now_ms;
    }

    if (!throttled) {
        WTSLogger::warn("[ARB_RISK] pair={} level={} type={} val={:.4f}/thr={:.4f}: {}{}",
                        alert.pair_id,
                        static_cast<int>(alert.level),
                        static_cast<int>(alert.type),
                        alert.value,
                        alert.threshold,
                        alert.message,
                        (alert.level == RiskAlert::Level::WARNING) ? " (throttled 60s)" : "");
    }

    // V8-R4/A8: EMERGENCY 此前只日志+广播, 套利组合止损线是"假保险丝" --
    // 达阈后 MM 照常报价、arb 照常开新仓, 兜底只剩做市侧另一套日亏线。
    // 接入: IRREVERSIBLE halt (停 MM 报价, 收盘 closeout 保持 Fix4 豁免)
    // + disable arb (停新信号; 持仓由 closeout/残腿对冲处置)。
    // 线程: 回调来自 arb 线程 (live) -- haltTrading 内部为原子写, 与
    // zombie-halt 路径同类; disable 与 arb 主循环同线程。
    if (alert.level == RiskAlert::Level::EMERGENCY) {
        if (_risk_monitor && !_risk_monitor->isTradingHalted()) {
            _risk_monitor->haltTrading(RiskCategory::IRREVERSIBLE, -alert.value);
            WTSLogger::error("[ARB_RISK] EMERGENCY halt triggered: arb portfolio drawdown {:.2f} >= {:.2f}",
                             alert.value,
                             alert.threshold);
        }
        if (_spread_arb_manager && _spread_arb_manager->isEnabled())
            _spread_arb_manager->disable();
    }

    // 转发到 EventNotifier (运维侧订阅 nanomsg topic="ARB_RISK")
    // 节流命中的 WARNING 不外发, 避免运维通道同步被刷爆
    if (_event_notifier && !throttled) {
        std::string msg = fmt::format("[L{}] pair={} type={} val={:.2f}/thr={:.2f}: {}",
                                      static_cast<int>(alert.level),
                                      alert.pair_id,
                                      static_cast<int>(alert.type),
                                      alert.value,
                                      alert.threshold,
                                      alert.message);
        _event_notifier->notify("ARB_RISK", msg.c_str());
    }
}

//==========================================================================
// 业务模块初始化
//==========================================================================

void UftFutuMmStrategy::initBusinessModules(wtp::IUftStraCtx* ctx)
{
    // 5A-3: 模块装配外移至 FutuModuleAssembler (friend, 引用别名, 零逻辑改动)
    FutuModuleAssembler::assemble(*this, ctx);
}

//==========================================================================
// UFT 策略回调
//==========================================================================

void UftFutuMmStrategy::on_init(IUftStraCtx* ctx)
{
    // v7.4 P0-2: 实盘回调多线程 (框架核实: MdSpi/TdSpi/ticker 三线程),
    // 全部回调已由 _cb_mtx 串行化, 通知 TradingState 停用单线程 tid 断言
    TradingState::setExternalLocking(true);

    // 保存ctx指针
    _main_ctx = ctx;
    _is_backtest = _config.is_backtest;
    WTSLogger::info(
        "UftFutuMmStrategy[{}] mode: {} (isBacktest={})", id(), _is_backtest ? "BACKTEST" : "LIVE", _is_backtest);

    // R1: 从 ctx 获取 EventNotifier (WtUftRunner 创建 UftStraContext 时已注入 &_notifier)
    auto* uft_ctx = dynamic_cast<UftStraContext*>(ctx);
    if (uft_ctx && uft_ctx->getEventNotifier()) {
        setEventNotifier(uft_ctx->getEventNotifier());
        WTSLogger::info("UftFutuMmStrategy[{}] EventNotifier injected via ctx", id());
    }

    // 默认收盘时间
    _config.closeout.close_time = 150000;

    // 从基础数据管理模块获取合约参数（如果配置文件未指定）
    // 5A-3: 合约信息加载外移 (session 缓存/收盘时间推导/乘数tick回填)
    FutuModuleAssembler::loadContractInfos(*this, ctx);

    // 初始化业务模块（需要合约参数 + ctx 用于 BilateralStats Per-Quoter sessInfo 注入）
    initBusinessModules(ctx);

    //============================================================
    // 配置校验（在 initBusinessModules 之后，所有模块参数已加载）
    //============================================================
    {
        FutuConfigValidator::ValidationResult vr;

        // 信号权重校验
        if (!_signal_aggregators.empty()) {
            const auto& sig_cfg = _signal_aggregators.begin()->second->getConfig();
            FutuConfigValidator::validateSignalWeights(sig_cfg.ofi_weight,
                                                       sig_cfg.trade_weight,
                                                       sig_cfg.book_imbalance_weight,
                                                       sig_cfg.momentum_weight,
                                                       sig_cfg.lead_lag_weight,
                                                       vr);
        }

        // GLFT 参数范围校验
        if (!_spread_optimizers.empty()) {
            const auto& glft = _spread_optimizers.begin()->second->getParams();
            FutuConfigValidator::checkRange("base_spread", glft.base_spread, 0.5, 20.0, vr);
            FutuConfigValidator::checkRange("phi", glft.phi, 0.01, 2.0, vr);
            FutuConfigValidator::checkPositive("tick_size", glft.tick_size, vr);
            FutuConfigValidator::checkRange("delta_skew_threshold", glft.delta_skew_threshold, 0.0, 0.9, vr);
        }

        // Portfolio 参数校验
        FutuConfigValidator::checkPositive("portfolio_max_delta", _portfolio->getParams().portfolio_max_delta, vr);

        // 单合约 delta/position 语义边界校验 (2026-08-19):
        //   maxDelta = 策略库存软限 (skew/qty衰减/义务/穿越的归一化基准)
        //   maxPosition = 风控硬顶 (仅 halt 闸门)
        for (const auto& ci : _contract_infos) {
            if (ci.max_position > 0 && ci.max_delta <= 0) {
                vr.addWarning(fmt::format("{}: maxDelta 未配置(<=0) — 策略库存调控(skew/衰减/义务)失效, "
                                          "仅剩 maxPosition 硬停",
                                          ci.code));
            } else if (ci.max_position > 0 && ci.max_delta > ci.max_position) {
                vr.addWarning(fmt::format("{}: maxDelta({:.0f}) > maxPosition({:.0f}) — 策略软限高于风控硬顶, "
                                          "义务/穿越等调节到达前即被硬停, 软限无意义",
                                          ci.code,
                                          ci.max_delta,
                                          ci.max_position));
            }
        }

        // 输出结果
        for (const auto& err : vr.errors) {
            WTSLogger::error("UftFutuMmStrategy[{}] Config validation ERROR: {}", id(), err);
        }
        for (const auto& warn : vr.warnings) {
            WTSLogger::warn("UftFutuMmStrategy[{}] Config validation WARNING: {}", id(), warn);
        }
        if (vr.valid) {
            WTSLogger::info(
                "UftFutuMmStrategy[{}] Config validation passed (0 errors, {} warnings)", id(), vr.warningCount());
        } else {
            WTSLogger::error(
                "UftFutuMmStrategy[{}] Config validation FAILED ({} errors, {} warnings) — strategy may misbehave!",
                id(),
                vr.errorCount(),
                vr.warningCount());
        }
    }

    //============================================================
    // 注册热更新参数（运行时可修改，无需重启策略）
    // 仅包含直接影响报价价格计算的参数
    // 仓位管理/风控/对冲等参数需重启生效
    // 注意：必须在 initBusinessModules() 之后注册，
    //       以便从已初始化的模块读取实际参数值作为默认值
    //============================================================

    const auto& coord_mp = _coordinator->getConfig().modules;

    // 从第一个 SpreadOptimizer 读取 GLFTParams 作为默认值
    GLFTParams glft_defaults;
    if (!_spread_optimizers.empty()) {
        auto it = _spread_optimizers.begin();
        if (it->second)
            glft_defaults = it->second->getParams();
    }

    // 从第一个 SignalAggregator 读取权重作为默认值
    SignalAggregatorConfig sig_defaults;
    if (!_signal_aggregators.empty()) {
        auto it = _signal_aggregators.begin();
        if (it->second)
            sig_defaults = it->second->getConfig();
    }

    // 热参数注册已拆分至 FutuHotParamManager (架构重构 C2)
    // B2: 合约级 maxDelta 默认取 anchor 合约配置值 (热更新应用于全部合约)
    double contract_max_delta_default = 0.0;
    for (const auto& ci : _contract_infos) {
        if (ci.code == _config.anchor_code) {
            contract_max_delta_default = ci.max_delta;
            break;
        }
    }
    _hot_mgr.registerParams(ctx, _config, glft_defaults, sig_defaults, coord_mp.alpha_sensitivity,
                            contract_max_delta_default);

    WTSLogger::info("UftFutuMmStrategy[{}] hot-update params registered (defaults from coordinator.yaml)", id());

    // 启动时同步共享内存中已存在的热参数值到各模块
    // 避免重启后丢失上一次热更新结果（sync_param 会保留共享内存旧值，但模块仍按 config 初始化）
    {
        FutuHotParamManager::Targets t;
        t.config = &_config;
        t.quoters = &_quoters;
        t.spread_opts = &_spread_optimizers;
        t.aggregators = &_signal_aggregators;
        t.coordinator = _coordinator.get();
        t.portfolio = _portfolio.get();
        _hot_mgr.applyAll(t, id());
        WTSLogger::info("UftFutuMmStrategy[{}] initial hot params synced from shared memory", id());
    }

    // 加固(2026-08-24): 启动期热参漂移摘要 —— 对比 hotparams.yaml 与 config/coordinator 同名键。
    // 无条件调用(回测也打印): watcher 不跑、热参不生效, 差异键即回测/实盘行为分叉点。
    _hot_mgr.logDriftSummary("hotparams.yaml", id());

    // 启动 hotparams.yaml 文件监视器 (修改后自动写入共享内存并触发热更新)
    // 仅在实盘模式启用 (回测无共享内存)
    // V8-P0-1: watcher 只写共享内存+置脏标志, 参数应用由 on_tick 在
    // _cb_mtx 内 drain -- 不再把 Targets 交给 watcher 线程直接 applyAll
    if (!_is_backtest) {
        _hot_watcher.start(id(), "hotparams.yaml", &_hot_mgr, 1000);
    }

    // 输出初始化日志
    WTSLogger::info("UftFutuMmStrategy[{}] initialized: {} contracts, {} levels",
                    id(),
                    _contract_infos.size(),
                    _config.quoting.num_levels);
    WTSLogger::info("MaxDelta={} (soft)", _config.portfolio.max_delta);
    WTSLogger::info("Modules: spreadOpt={}, toxicity={}",
                    _config.modules.use_spread_optimizer,
                    _config.modules.use_toxicity_detector);

    // 设置跨期套利信号回调
    if (_spread_arb_manager) {
        _spread_arb_manager->setSignalCallback([this](const SpreadSignal& signal) {
            // 信号回调只记录，实际执行在 processSpreadArbitrage 中
            WTSLogger::info("SpreadArb signal: pair={}, type={}, confidence={}",
                            signal.pair_id,
                            (int)signal.type,
                            signal.confidence);
        });
    }

    // 订阅所有合约行情（使用 fullCode，与行情推送格式一致）
    for (const auto& ci : _contract_infos) {
        ctx->stra_sub_ticks(ci.code.c_str());
        WTSLogger::info("UftFutuMmStrategy[{}] subscribed: {}", id(), ci.code);
    }

    // MonitorBridge: WtMonSvr GUI 数据桥 (stradata 资金/持仓落盘, 默认关)
    {
        MonitorBridge::Config mbCfg;
        mbCfg.enabled = _config.monitor.enabled;
        mbCfg.flush_interval_ms = _config.monitor.flush_interval_ms;
        _mon_bridge.init(id(), _portfolio.get(), mbCfg);
    }
}

void UftFutuMmStrategy::on_session_begin(IUftStraCtx* ctx, uint32_t uTDate)
{
    FUTU_CB_LOCK_GUARD();
    // V8-R6/WS-E: 命令化 —— big 下 post 后同回调内 drain, 与旧内联逐比特一致。
    // none 下 Session 属 Md 域: 仅发布者即 Md 线程(回测单线程)时自 drain;
    // 实盘 ticker 线程只投递, 由下一 on_tick 检查点在 Md 属主线程执行
    // (session 切换后必有 tick 流入, 无静默期滞留)。
    postCommand(PendingCommand::Type::SessionBegin, uTDate);
#if FUTU_CB_LOCK_BIG
    drainPendingCommands(ctx, true);
#else
    if (std::this_thread::get_id() == _md_tid.load(std::memory_order_acquire))
        drainPendingCommands(ctx, true);
#endif
}

void UftFutuMmStrategy::on_session_end(IUftStraCtx* ctx, uint32_t uTDate)
{
    FUTU_CB_LOCK_GUARD();
    postCommand(PendingCommand::Type::SessionEnd, uTDate);
#if FUTU_CB_LOCK_BIG
    drainPendingCommands(ctx, true);
#else
    if (std::this_thread::get_id() == _md_tid.load(std::memory_order_acquire))
        drainPendingCommands(ctx, true);
#endif
}

//============================================================
// on_tick 子函数 (P2-1a: 从 on_tick 拆出)
//============================================================

void UftFutuMmStrategy::handleQuotingAutoResume()
{
    if (_trading_state.qphase != QuotingPhase::ERROR || _quoting_paused_since == 0)
        return;

    uint64_t paused_ms =
        (_exchange_time_ms > 0 ? _exchange_time_ms.load(std::memory_order_acquire) : TimeUtils::getLocalTimeNow()) -
        _quoting_paused_since;
    uint64_t wait_threshold = 10000; // 初始等待 10 秒
    if (_order_error_count > 0) {
        uint32_t shift = (_order_error_count < 5) ? _order_error_count.load(std::memory_order_acquire) : 5;
        uint64_t exp_wait = 10000ULL << shift;
        wait_threshold = (exp_wait > 60000ULL) ? 60000ULL : exp_wait;
    }

    if (paused_ms > wait_threshold) {
        // 试探性恢复: count 不衰减保留真实历史.
        // 退避到期后无条件试探翻 NORMAL, 让做市试发新单. 若再次失败 on_entrust
        // 失败回调会立即把 qphase 翻回 ERROR (count 已大, 走硬触发硬撤路径).
        if (_trading_state.tryResumeFrom(QuotingPhase::ERROR)) {
            _quoting_paused_since = 0;
            WTSLogger::info("UftFutuMmStrategy[{}] Quoting auto-resumed after {}ms (count={}, probing)",
                            id(),
                            paused_ms,
                            _order_error_count);
        }
    }
}

void UftFutuMmStrategy::handleMarketDataUpdate(const char* stdCode, WTSTickData* tick, double mid)
{
    if (mid > 0) {
        _portfolio->markToMarket(stdCode, mid);
        // V8-R4: _last_mid 写入删除 -- Coordinator 在 processTick 组合更新段
        // 以 tc.mid 写同一时点的 mid (单一属主, 时序等价)

        if (_price_stale) {
            _price_stale = false;
            WTSLogger::info("UftFutuMmStrategy[{}] Price recovered (first tick after channel ready)", id());
        }
    }

    if (_correlation_manager) {
        _correlation_manager->onTick(tick);

        if (_portfolio) {
            const std::string& anchor = _config.anchor_code;
            if (stdCode != anchor) {
                double beta = _correlation_manager->getHedgeRatio(stdCode, anchor);
                auto stats = _correlation_manager->getCorrelation(stdCode, anchor);

                // v7.6: 裸指针直写收编为 portfolio 锁内方法 (逻辑原样内聚)
                _portfolio->smoothUpdateHedgeRatio(stdCode, beta, stats.sample_count);
            }
        }
    }
}

void UftFutuMmStrategy::handleLeadLagPush(const char* stdCode, WTSTickData* tick, double mid)
{
    if (_config.anchor_code.empty() || mid <= 0)
        return;

    // LeadLag 调试: 确认 push 是否到达
    static uint64_t ll_dbg_counter = 0;
    if (ll_dbg_counter++ % 10000 == 0) {
        WTSLogger::debug("[LEADLAG_DBG] stdCode={} anchor={} mid={:.2f} aggregators={}",
                         stdCode,
                         _config.anchor_code,
                         mid,
                         _signal_aggregators.size());
    }

    if (stdCode != _config.anchor_code)
        return;

    uint64_t ts = tick->volume() > 0 ? static_cast<uint64_t>(tick->actiondate()) * 1000000000ULL +
                                           static_cast<uint64_t>(tick->actiontime())
                                     : 0;

    for (auto& [code, aggregator] : _signal_aggregators) {
        if (code != _config.anchor_code && aggregator) {
            aggregator->updateLeadContract(_config.anchor_code, mid, ts);
        }
    }
}

void UftFutuMmStrategy::drainTdSpiLogs()
{
    uint64_t dropped = _tdspi_logs_dropped.exchange(0, std::memory_order_relaxed);
    if (dropped > 0)
        WTSLogger::warn("TdSpi log queue full: {} entries dropped (deferred debug logs)", dropped);
    _tdspi_log_queue.popAll([](const TdSpiLogEvent& e) {
        if (e.level == 0)
            WTSLogger::debug("UftFutuMmStrategy TRADE(deferred): {} {} {:.0f}@{:.1f} | Delta: {:.0f} {}",
                             e.code.c_str(), e.action.c_str(), e.vol, e.price, e.delta, e.effect.c_str());
        else
            WTSLogger::info("UftFutuMmStrategy TRADE(deferred): {} {} {:.0f}@{:.1f} | Delta: {:.0f} {}",
                            e.code.c_str(), e.action.c_str(), e.vol, e.price, e.delta, e.effect.c_str());
    });
}

void UftFutuMmStrategy::handleCoordinatorTick(IUftStraCtx* ctx, const char* stdCode, WTSTickData* tick, uint64_t now_ms)
{
    drainTdSpiLogs();  // C11: drain deferred TdSpi logs (moved from fill path to tick path)
    if (!_coordinator || _price_stale) {
        // Fallback: no coordinator (or stale price), trigger fail-safe
        if (!_coordinator) {
            WTSLogger::error("UftFutuMmStrategy[{}] Coordinator is null, triggering FAIL-SAFE!", id());
            _trading_state.setQuotingPhase(QuotingPhase::RISK_HALTED);
            for (auto& [code, quoter] : _quoters)
                quoter->cancelAll(ctx);
        }
        return;
    }

    auto result = _coordinator->processTick(ctx, stdCode, tick, now_ms, _tsc_tick0);

    // 记录报价到绩效分析器
    if (result.quote_placed && _perf_analyzer) {
        double qmid = _coordinator ? _coordinator->getLastMid(stdCode) : 0.0;
        _perf_analyzer->recordQuote(stdCode,
                                    qmid,
                                    qmid,
                                    0,
                                    0,
                                    ctx->stra_get_date() * 1000000ULL + ctx->stra_get_time() * 100ULL +
                                        ctx->stra_get_secs());
    }

    // === Closeout 驱动 (已拆分至 CloseoutOrchestrator, 架构重构 C3) ===
    _closeout_orch.onTick(ctx, tick, result.closeout_executed);
}

//============================================================
// on_tick 主控 (P2-1a: 从 285 行 → ~35 行)
//============================================================

void UftFutuMmStrategy::on_tick(IUftStraCtx* ctx, const char* stdCode, WTSTickData* tick)
{
    FUTU_CB_LOCK_GUARD();
    // V8-R6/WS-E: 首个 tick 捕获 Md 属主 tid (回测=回放线程; 实盘=MdSpi),
    // 供 none 模式 Session 命令发布者自 drain 判定。
    if (_md_tid.load(std::memory_order_relaxed) == std::thread::id{})
        _md_tid.store(std::this_thread::get_id(), std::memory_order_release);
    drainPendingCommands(ctx, true); // V8-R6/WS-E Md 检查点: Session 域 (快速路径 ~1ns)

    // V8-P0-1: 热参数 drain -- watcher 线程只写共享内存+置脏, 此处(_cb_mtx 内)
    // 统一应用; 嵌套 recursive_mutex 安全, 复用 on_params_updated 的
    // Targets 组装+日志。通道未就绪也照常 drain (参数应用不依赖行情通道)。
    if (_hot_mgr.consumePendingApply())
        on_params_updated();

    if (!_channel_ready || !tick)
        return;

    // P0: tick 入口 rdtsc (perf monitor 启用时 ~6ns, 禁用时一次分支)
    _tsc_tick0 = _performance_monitor ? TscClock::now() : 0;

    // 1. 报价暂停恢复 (ERROR → NORMAL with exponential backoff)
    handleQuotingAutoResume();

    // 2. v7.1 主时钟: 全策略决策逻辑统一用 replay 时间 (tick actiondate/actiontime)。
    //   回测中墙钟节流(delta-rate滑窗/毒性cooloff/限速/恢复冷却)随机器速度漂移,
    //   导致订单序列不可复现 (同配置两次运行 PnL 差数倍)。
    //   实盘时 replay 时间=交易所时间, 语义一致。
    {
        uint32_t ad = tick->actiondate();
        uint32_t at = tick->actiontime();
        _exchange_time_ms = static_cast<uint64_t>(ad) * 86400000ULL +
                            static_cast<uint64_t>(at / 10000000) * 3600000ULL +
                            static_cast<uint64_t>((at / 100000) % 100) * 60000ULL +
                            static_cast<uint64_t>((at / 1000) % 100) * 1000ULL + (at % 1000);
        if (_coordinator)
            _coordinator->setExchangeTime(_exchange_time_ms);
        if (_order_router)
            _order_router->setNowMs(_exchange_time_ms);
        if (_spread_arb_manager)
            _spread_arb_manager->setNowMs(_exchange_time_ms);
        if (_async_arb)
            _async_arb->setReplayNowUs(_exchange_time_ms * 1000ULL); // V8-R4/A5
    }
    uint64_t now_ms = _exchange_time_ms;
    if (_risk_monitor)
        _risk_monitor->setCurrentTime(now_ms);

    // 3. 行情数据更新 (markToMarket + correlation + hedge_ratio)
    // C1: 单边盘口(锁板 AskPrice1=0 / 锁跌停 BidPrice1=0)时 (0+ask)/2 半价
    //     会污染 markToMarket -> 浮盈瞬间巨亏 -> DAILY_LOSS 误触发
    //     IRREVERSIBLE halt + 对手价 FAK 强平 (preCheck 拦不到策略层)。
    //     双边均 >0 才接受, 否则 mid=0 下游全部跳过, 保留上一有效值。
    const double bid0 = tick->bidprice(0);
    const double ask0 = tick->askprice(0);
    double mid = (bid0 > 0 && ask0 > 0) ? (bid0 + ask0) / 2.0 : 0.0;
    handleMarketDataUpdate(stdCode, tick, mid);

    // 3.5 PerformanceAnalyzer tick 更新 (真实 adverse selection 追踪)
    // V8-R3: 与 recordTrade 同基准 replay _exchange_time_ms (旧 actiontime 双域混用)
    if (_perf_analyzer && mid > 0)
        _perf_analyzer->onTickUpdate(stdCode, mid, _exchange_time_ms);

    // 4. LeadLag 跨合约推送
    handleLeadLagPush(stdCode, tick, mid);

    // 5. Coordinator 主处理 + closeout 驱动 (含 coordinator null fail-safe)
    handleCoordinatorTick(ctx, stdCode, tick, now_ms);

    // 6. 跨期价差套利 (与做市业务平级，独立处理 — 已拆分至 ArbExecutionBridge, C4)
    // v7.1: CLOSEOUT 阶段不喂 arb tick — 收盘平仓窗口内开新价差仓会让
    //       closeout 前功尽弃 (回测实证: 14:45 平仓后 arb 重建 ~50 手 delta 过夜,
    //       次日跳空触发日亏 IRREVERSIBLE halt)。夜盘 closeout 由
    //       exitToQuoting 恢复 phase 后自动恢复喂入。
    // v7.1: session 休息段同样不喂 arb (与报价暂停一致).
    if (_spread_arb_manager && _config.modules.use_spread_arbitrage && _trading_state.phase != MmPhase::CLOSEOUT &&
        !(_coordinator && _coordinator->isSectionBreakActive())) {
        _arb_bridge.onTick(ctx, stdCode, tick);
    }

    _tick_count++;

    // MonitorBridge: 节流落盘 (内部仅时间戳比较, 到点才写盘)
    _mon_bridge.maybeFlush(ctx);
}

void UftFutuMmStrategy::on_order_queue(IUftStraCtx* ctx, const char* stdCode, WTSOrdQueData* newOrdQue)
{
    FUTU_CB_LOCK_GUARD(); // v7.4 P0-2 回调串行化 (FUTU_CALLBACK_LOCK=0 时空操作)
    // Level2: 更新 MarketDataContext
    auto it = _market_data.find(stdCode);
    if (it != _market_data.end())
        it->second->onOrderQueue(newOrdQue);
}

void UftFutuMmStrategy::on_order_detail(IUftStraCtx* ctx, const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
    FUTU_CB_LOCK_GUARD(); // v7.4 P0-2 回调串行化 (FUTU_CALLBACK_LOCK=0 时空操作)
    // Level2: 更新 MarketDataContext
    auto it = _market_data.find(stdCode);
    if (it != _market_data.end())
        it->second->onOrderDetail(newOrdDtl);
}

void UftFutuMmStrategy::on_transaction(IUftStraCtx* ctx, const char* stdCode, WTSTransData* newTrans)
{
    FUTU_CB_LOCK_GUARD(); // v7.4 P0-2 回调串行化 (FUTU_CALLBACK_LOCK=0 时空操作)
    if (!newTrans)
        return;

    const auto& trans = newTrans->getTransStruct();
    double qty = static_cast<double>(trans.volume);
    double price = trans.price;
    uint64_t timestamp = trans.action_time;

    // V8-R6/P2-4: 删除死代码 —— last_mid/isBuy 此前计算后零消费(方向分类由
    // MarketDataContext::onTransaction 内部独立完成), 且 last_mid==0 时默认
    // isBuy=true 的系统性偏差曾污染下游口径的隐患随删除一并消除。

    // 更新 SpreadOptimizer 的成交数据 (P0-2.1: Legacy onFill removed)
    if (_config.modules.use_spread_optimizer) {
        // NO-OP: Fill stats should ideally be aggregated in SignalContext
    }

    if (_correlation_manager) {
        _correlation_manager->onTick(stdCode, price, timestamp);
    }

    // 移除不安全的直连调用：_spread_arb_manager->onTick(stdCode, price, 1.0, timestamp);
    // 因为套利主计算已由 _async_arb 在子线程专门负责，主线程的 on_transaction 强行调用会导致致命的 Map 并发读写冲突 (Core Dump)

    // 更新 MarketDataContext (不可或缺核心组件)
    auto md_it = _market_data.find(stdCode);
    if (md_it != _market_data.end())
        md_it->second->onTransaction(newTrans);

    // 更新 PerformanceMonitor
    if (_performance_monitor) {
        _performance_monitor->recordFillReceived();
    }
}

void UftFutuMmStrategy::on_trade(
    IUftStraCtx* ctx, uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double vol, double price)
{
    FUTU_CB_LOCK_GUARD();
    drainPendingCommands(ctx, false); // V8-R6/WS-E Td 检查点: Channel 域
    // 5A-3: 成交处理外移 (FutuRuntimeOps, friend+别名, 零逻辑改动)
    FutuRuntimeOps::processTradeFill(*this, ctx, localid, stdCode, isLong, offset, vol, price);
}

void UftFutuMmStrategy::on_order(IUftStraCtx* ctx,
                                 uint32_t localid,
                                 const char* stdCode,
                                 bool isLong,
                                 uint32_t offset,
                                 double totalQty,
                                 double leftQty,
                                 double price,
                                 bool isCanceled)
{
    FUTU_CB_LOCK_GUARD();
    drainPendingCommands(ctx, false); // V8-R6/WS-E Td 检查点: Channel 域
    // 5A-3: 外移 FutuRuntimeOps
    FutuRuntimeOps::onOrderEvent(*this, ctx, localid, stdCode, isLong, offset, totalQty, leftQty, price, isCanceled);
}

void UftFutuMmStrategy::on_position(
    IUftStraCtx* ctx, const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail)
{
    FUTU_CB_LOCK_GUARD(); // v7.4 P0-2 回调串行化 (FUTU_CALLBACK_LOCK=0 时空操作)
    // 框架回调：本地持仓更新（不是账户持仓）
    // 注意：框架的 UftStraContext::on_position 账户持仓回调被注释掉了
    // 这里收到的是本地持仓文件的缓存数据，参数含义：
    //   prevol: 本地持仓量（净头寸）
    //   preavail: 本地持仓量
    //   newvol: 0
    //   newavail: 0
    // 今仓/昨仓逻辑由框架 ActionPolicy 处理，策略层不区分

    double local_pos = prevol; // 本地持仓净头寸

    WTSLogger::debug("UftFutuMmStrategy[{}] Local position update: {} {}={}",
                     id(),
                     stdCode,
                     isLong ? "long" : "short",
                     std::abs(local_pos));

    // 使用策略本地持仓更新 Portfolio（而不是账户持仓）
    // 一个账户可能有多个策略，每个策略只管理自己的持仓
    double local_net = ctx->stra_get_local_position(stdCode);

    double current = _portfolio->getPosition(stdCode);
    if (std::abs(current - local_net) > 0.01) {
        WTSLogger::info(
            "UftFutuMmStrategy[{}] Position sync: {} portfolio={} -> local_net={}", id(), stdCode, current, local_net);
        _portfolio->onPositionUpdate(stdCode, local_net);

        // P0-2.3: Mark portfolio context as dirty for lazy update
        _portfolio_ctx_dirty = true;
    }
}

void UftFutuMmStrategy::on_channel_ready(IUftStraCtx* ctx)
{
    FUTU_CB_LOCK_GUARD();
    // V8-R6/WS-E: 命令化 —— Channel 属 Td 域, 发布者即 TdSpi 线程,
    // 两种模式均允许自 drain (行情静默期无滞留)。
    postCommand(PendingCommand::Type::ChannelReady, 0);
    drainPendingCommands(ctx, false);
}

void UftFutuMmStrategy::on_channel_lost(IUftStraCtx* ctx)
{
    FUTU_CB_LOCK_GUARD();
    postCommand(PendingCommand::Type::ChannelLost, 0);
    drainPendingCommands(ctx, false);
}

void UftFutuMmStrategy::finalizeOrder(uint32_t localid)
{
    // 5A-3: 外移 FutuRuntimeOps (M1/M2 幂等清理)
    FutuRuntimeOps::finalizeOrder(*this, localid);
}

//==========================================================================
// V8-R6/WS-E: 罕见重操作命令通道
//==========================================================================

void UftFutuMmStrategy::postCommand(PendingCommand::Type type, uint32_t uTDate)
{
    {
        std::lock_guard<std::mutex> _g(_cmd_mtx);
        _pending_cmds.push_back({type, uTDate});
    }
    _cmd_has_pending.store(true, std::memory_order_release);
}

void UftFutuMmStrategy::drainPendingCommands(wtp::IUftStraCtx* ctx, bool session_domain)
{
    auto isSessionCmd = [](PendingCommand::Type t) {
        return t == PendingCommand::Type::SessionBegin || t == PendingCommand::Type::SessionEnd;
    };
    // 快速路径: 无待处理命令时一次 acquire load 即返回 (~1ns, 热路径零负担)
    if (!_cmd_has_pending.load(std::memory_order_acquire))
        return;
    // 单飞 claim: 他线程执行中 => 本域新命令由其在循环内收走或其后的同域
    // 检查点兜底, 此处直接返回
    if (_cmd_executing.exchange(1, std::memory_order_acq_rel) != 0)
        return;
    // RAII: 命令执行抛异常也必须释放单飞标志, 否则后续 drain 永久失效
    struct ExecGuard
    {
        std::atomic<uint8_t>& f;
        ~ExecGuard() { f.store(0, std::memory_order_release); }
    } _exec_guard{_cmd_executing};

    // 按属主域分拣: 循环收走本域命令直至清空; 他域命令留队, 由其属主
    // 检查点消费 (Md 检查点=Session 域, Td 检查点/发布者=Channel 域)。
    for (;;) {
        std::vector<PendingCommand> batch;
        {
            std::lock_guard<std::mutex> _g(_cmd_mtx);
            std::vector<PendingCommand> remain;
            remain.reserve(_pending_cmds.size());
            for (const auto& c : _pending_cmds) {
                if (isSessionCmd(c.type) == session_domain)
                    batch.push_back(c);
                else
                    remain.push_back(c);
            }
            _pending_cmds.swap(remain);
        }
        if (batch.empty())
            break;
        for (const auto& cmd : batch) {
            switch (cmd.type) {
            case PendingCommand::Type::ChannelReady:
                FutuRuntimeOps::onChannelReady(*this, ctx);
                break;
            case PendingCommand::Type::ChannelLost:
                FutuRuntimeOps::onChannelLost(*this, ctx);
                break;
            case PendingCommand::Type::SessionBegin:
                FutuRuntimeOps::onSessionBegin(*this, ctx, cmd.uTDate);
                break;
            case PendingCommand::Type::SessionEnd:
                FutuRuntimeOps::onSessionEnd(*this, ctx, cmd.uTDate);
                break;
            }
        }
    }
    // 退出前重算标志: 执行期间到达的本域/他域命令都不丢失唤醒
    {
        std::lock_guard<std::mutex> _g(_cmd_mtx);
        _cmd_has_pending.store(!_pending_cmds.empty(), std::memory_order_release);
    }
}

void UftFutuMmStrategy::on_entrust(uint32_t localid, bool bSuccess, const char* message)
{
    FUTU_CB_LOCK_GUARD();
    // on_entrust 无 ctx 入参 (框架契约), 复用 on_init 缓存的主 ctx
    if (_main_ctx)
        drainPendingCommands(_main_ctx, false); // V8-R6/WS-E Td 检查点: Channel 域
    // 5A-3: 外移 FutuRuntimeOps
    FutuRuntimeOps::onEntrust(*this, _main_ctx, localid, bSuccess, message);
}

//==========================================================================
// 参数热更新回调
//==========================================================================

void UftFutuMmStrategy::on_params_updated()
{
    // v7.4 P0-2: 实盘回调多线程 (MdSpi/TdSpi/ticker), 外部锁串行化
    FUTU_CB_LOCK_GUARD();
    WTSLogger::info("UftFutuMmStrategy[{}] === PARAMS HOT UPDATE ===", id());

    // 热参数应用已拆分至 FutuHotParamManager (架构重构 C2)
    FutuHotParamManager::Targets t;
    t.config = &_config;
    t.quoters = &_quoters;
    t.spread_opts = &_spread_optimizers;
    t.aggregators = &_signal_aggregators;
    t.coordinator = _coordinator.get();
    t.portfolio = _portfolio.get();
    _hot_mgr.applyAll(t, id());

    WTSLogger::info("UftFutuMmStrategy[{}] === HOT UPDATE COMPLETE ===", id());
}

} // namespace futu

//==========================================================================
// 策略工厂导出
//==========================================================================

#include "../Includes/UftStrategyDefs.h"

class FutuStrategyFact : public IUftStrategyFact
{
public:
    FutuStrategyFact() {}
    virtual ~FutuStrategyFact() {}

    virtual const char* getName() override { return "FutuStraFact"; }

    virtual void enumStrategy(FuncEnumUftStrategyCallback cb) override { cb(getName(), "FutuMM", true); }

    virtual UftStrategy* createStrategy(const char* name, const char* id) override
    {
        if (strcmp(name, "FutuMM") == 0)
            return new futu::UftFutuMmStrategy(id);
        return nullptr;
    }

    virtual bool deleteStrategy(UftStrategy* stra) override
    {
        delete stra;
        return true;
    }
};

extern "C" {
EXPORT_FLAG IUftStrategyFact* createStrategyFact()
{
    return new FutuStrategyFact();
}

EXPORT_FLAG void deleteStrategyFact(IUftStrategyFact*& fact)
{
    if (fact) {
        delete fact;
        fact = nullptr;
    }
}
}
