/*!
 * \file FutuModuleAssembler.cpp
 * \brief 5A-3 (v7.5): 业务模块装配器 — initBusinessModules 外移
 *
 * 策略壳瘦身: 模块创建/配置/依赖注入 (~650行) 从 UftFutuMmStrategy 剥离。
 * friend + 引用别名方案: 函数体与原实现逐行一致, 别名绑定到策略成员,
 * 零逻辑改动; id() -> s.id(), setEventNotifier -> s.setEventNotifier。
 */
#include "FutuModuleAssembler.h"
#include "UftFutuMmStrategy.h"
#include "../Includes/IUftStraCtx.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../WTSTools/WTSLogger.h"

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
#include "SpreadArbitrageManager.h"
#include "FutuComponentFactory.h"
#include "SelfTradePrevention.h"
#include "OrderRouter.h"
#include "StrategyCoordinator.h"
#include "AsyncArbitrageExecutor.h"
#include "CorrelationManager.h"
#include "SelfTradeCalibrator.h"
#include "SignalAggregator.h"
#include "TradingState.h"
#include "../Share/CodeHelper.hpp"
#include "../Includes/WTSContractInfo.hpp"

namespace
{

/// fullCode (SHFE.ag2606) -> stdCode (SHFE.ag.2606)
std::string fullCodeToStdCode(const std::string& fullCode)
{
    size_t firstDot = fullCode.find('.');
    if (firstDot == std::string::npos)
        return fullCode;
    std::string exchg = fullCode.substr(0, firstDot);
    std::string code = fullCode.substr(firstDot + 1);
    return CodeHelper::rawMonthCodeToStdCode(code.c_str(), exchg.c_str());
}

} // anonymous namespace

namespace futu
{

void FutuModuleAssembler::assemble(UftFutuMmStrategy& s, wtp::IUftStraCtx* ctx)
{
    // 引用别名: 绑定策略成员, 保持原 initBusinessModules 函数体不变
    auto& _config = s._config;
    auto& _coordinator = s._coordinator;
    auto& _contract_infos = s._contract_infos;
    auto& _portfolio = s._portfolio;
    auto& _correlation_manager = s._correlation_manager;
    auto& _quoters = s._quoters;
    auto& _spread_optimizers = s._spread_optimizers;
    auto& _order_tracker = s._order_tracker;
    auto& _session_cache = s._session_cache;
    auto& _market_data = s._market_data;
    auto& _signal_aggregators = s._signal_aggregators;
    auto& _order_router = s._order_router;
    auto& _closeout_executor = s._closeout_executor;
    auto& _risk_monitor = s._risk_monitor;
    auto& _closeout_orch = s._closeout_orch;
    auto& _trading_state = s._trading_state;
    auto& _toxicity_detector = s._toxicity_detector;
    auto& _self_trade_calibrator = s._self_trade_calibrator;
    auto& _perf_analyzer = s._perf_analyzer;
    auto& _performance_monitor = s._performance_monitor;
    auto& _spread_arb_manager = s._spread_arb_manager;
    auto& _stp = s._stp;
    auto& _async_arb = s._async_arb;
    auto& _arb_bridge = s._arb_bridge;
    auto& _event_notifier = s._event_notifier;
    auto& _tick_count = s._tick_count;

    //------------------------------------------------------------
    // 0. 初始化 StrategyCoordinator 获取模块开关及配置
    //------------------------------------------------------------
    _coordinator = std::make_unique<StrategyCoordinator>();

    // Load from coordinator_config
    std::string coord_cfg_path = _config.coordinator_config.empty() ? "coordinator.yaml" : _config.coordinator_config;
    _coordinator->loadConfig(coord_cfg_path);

    // Apply strategy-level settings (final override)
    {
        const auto& mp = _coordinator->getConfig().modules;
        CoordinatorConfig coord_cfg = _coordinator->getConfig();
        coord_cfg.closeout_minutes_before = _config.closeout.minutes_before;
        coord_cfg.close_time = _config.closeout.close_time;
        coord_cfg.closeout_flatten_position = _config.closeout.flatten_position;
        coord_cfg.night_close_time = _config.closeout.night_close_time;
        coord_cfg.night_minutes_before = _config.closeout.night_minutes_before;
        coord_cfg.perf_monitor_latency_threshold = _config.perf.monitor_latency_threshold;
        coord_cfg.perf_enabled = _config.perf.enabled;
        coord_cfg.perf_log_interval = _config.perf.log_interval;
        coord_cfg.perf_warn_threshold_ns = _config.perf.warn_threshold_ns;
        coord_cfg.perf_critical_threshold_ns = _config.perf.critical_threshold_ns;
        _coordinator->setConfig(coord_cfg);
    }

    // 从 Coordinator 获取模块开关（来自 coordinator.yaml）
    const auto& coord_cfg = _coordinator->getConfig();

    // 模块开关（从 coordinator.yaml 读取，而非 config.yaml）
    _config.modules.use_toxicity_detector = coord_cfg.use_toxicity_detector;
    _config.modules.use_spread_optimizer = coord_cfg.use_spread_optimizer;
    _config.modules.use_adaptive_param = coord_cfg.use_adaptive_params;

    //==========================================================================
    // 模块开关统一解析 (单一权威来源):
    //   coordinator.yaml 根级是以下 7 个开关的唯一位置, 不再 fallback 到
    //   modules 节点或 config.yaml — 避免多源错配.
    //     useMarketMaking / useSpreadArbitrage / useAsyncArbThread
    //     usePerformanceMonitor / usePerformanceAnalyzer
    //     use_signal_aggregator / useHedging
    //   3 个模块级开关(toxicityDetector/spreadOptimizer/adaptiveParam) 只读
    //   modules.<name>.enabled, 由 StrategyCoordinator::loadConfigFromVariant 解析.
    //   下方 coordBool 找不到键时使用编译期默认(见 Modules 构造函数).
    //==========================================================================
    {
        wtp::WTSVariant* raw = coord_cfg._raw_variant;
        auto coordBool = [&](const char* key, bool compile_time_default) -> bool {
            if (raw) {
                wtp::WTSVariant* node = raw->get(key);
                if (node)
                    return node->asBoolean();
            }
            return compile_time_default;
        };
        _config.modules.use_market_making = coordBool("useMarketMaking", true);
        _config.modules.use_spread_arbitrage = coordBool("useSpreadArbitrage", false);
        _config.modules.use_async_arb_thread = coordBool("useAsyncArbThread", true);
        _config.modules.use_performance_monitor = coordBool("usePerformanceMonitor", false);
        _config.modules.use_performance_analyzer = coordBool("usePerformanceAnalyzer", false);
        // STP 唯一权威: coordinator.yaml modules.selfTradePrevention
        {
            wtp::WTSVariant* modules = raw ? raw->get("modules") : nullptr;
            wtp::WTSVariant* stp_v = modules ? modules->get("selfTradePrevention") : nullptr;
            StpConfig stp_cfg;
            if (stp_v)
                stp_cfg = StpConfig::fromVariant(stp_v);
            _config.modules.use_self_trade_prevention = stp_cfg.enabled;
            _config.modules.stp_min_price_gap = stp_cfg.min_price_gap;
        }
    }

    WTSLogger::info("Strategy mode: MM={}, Arb={}",
                    _config.modules.use_market_making ? "ON" : "OFF",
                    _config.modules.use_spread_arbitrage ? "ON" : "OFF");

    //------------------------------------------------------------
    // 1. FutuPortfolio（持仓管理）- 始终需要
    //------------------------------------------------------------
    _portfolio = std::make_unique<FutuPortfolio>();

    PortfolioParams portfolio_params;
    portfolio_params.portfolio_max_delta = _config.portfolio.max_delta;
    portfolio_params.hedge_ratio = _config.portfolio.hedge_ratio;
    portfolio_params.max_exposure = _config.risk.max_exposure;
    // max_loss 语义为正容忍度 (RiskMonitor: pnl < -max_loss 触发)。
    // 配置历史上正负约定混用(默认 -200000, README 示例 50000), 统一取绝对值防御.
    portfolio_params.max_loss = std::abs(_config.risk.max_daily_loss);
    _portfolio->setParams(portfolio_params);
    _portfolio->setAnchorContract(_config.anchor_code);

    WTSLogger::info("FutuPortfolio: maxDelta={} (soft), hedgeRatio={}, maxExposure={}, maxLoss={}",
                    portfolio_params.portfolio_max_delta,
                    portfolio_params.hedge_ratio,
                    portfolio_params.max_exposure,
                    portfolio_params.max_loss);

    // 添加合约到 Portfolio（包含单合约限制）
    for (const auto& ci : _contract_infos) {
        double max_pos = (ci.max_position > 0) ? ci.max_position : 0;
        double contract_max_del = (ci.max_delta > 0) ? ci.max_delta : 0;
        _portfolio->addContract(
            ci.code, ci.multiplier, ci.tick_size, 1.0, max_pos, contract_max_del, ci.target_position);
    }

    //------------------------------------------------------------
    // 2. CorrelationManager (相关性与组合极度套利)
    //------------------------------------------------------------
    if (_config.modules.use_market_making || _config.modules.use_spread_arbitrage) {
        _correlation_manager = std::make_unique<CorrelationManager>();

        const auto& mp = coord_cfg.modules;
        wtp::WTSVariant* root = coord_cfg._raw_variant;
        wtp::WTSVariant* modules_v = root ? root->get("modules") : nullptr;

        CorrelationConfig corr_cfg;
        if (modules_v) {
            wtp::WTSVariant* corr_v = modules_v->get("correlationManager");
            if (corr_v)
                corr_cfg = CorrelationConfig::fromVariant(corr_v);
        }
        _correlation_manager->setConfig(corr_cfg);

        for (size_t i = 0; i < _contract_infos.size(); ++i) {
            _correlation_manager->addContract(_contract_infos[i].code, _contract_infos[i].multiplier);
            for (size_t j = i + 1; j < _contract_infos.size(); ++j) {
                _correlation_manager->addRelation(_contract_infos[i].code, _contract_infos[j].code);
            }
        }
        WTSLogger::info("CorrelationManager: initialized with windowSize={}", corr_cfg.window_size);
    }

    //------------------------------------------------------------
    // 2. FutuQuoter（报价引擎）- 每合约一个 (仅做市)
    //------------------------------------------------------------
    if (_config.modules.use_market_making) {
        for (const auto& ci : _contract_infos) {
            auto quoter = std::make_unique<FutuQuoter>();

            QuoterConfig qcfg;
            qcfg.code = ci.code;
            qcfg.num_levels = _config.quoting.num_levels;
            qcfg.base_spread = _config.quoting.base_spread;
            qcfg.level_step = _config.quoting.level_step;
            qcfg.base_qty = _config.quoting.base_qty;
            qcfg.level_qty_multiplier = _config.quoting.level_qty_multiplier;
            qcfg.tick_size = ci.tick_size;
            qcfg.sticky_threshold = _config.quoting.sticky_threshold;
            qcfg.improve_retreat_ratio = _config.quoting.improve_retreat_ratio;
            qcfg.max_price_deviation = _config.quoting.max_price_deviation;

            // 价格保护参数
            qcfg.price_protection = _config.quoting.price_protection;
            qcfg.protect_ticks = _config.quoting.protect_ticks;

            // 双边报价参数
            qcfg.use_bilateral_quote = _config.quoting.use_bilateral_quote;
            qcfg.min_valid_qty = _config.quoting.base_qty; // 有效挂单最小数量 = 基础挂单量

            // v3 软风控参数透传
            qcfg.qty_decay_factor = _config.quoting.qty_decay_factor;
            qcfg.obligation_min_qty = _config.quoting.obligation_min_qty;
            qcfg.obligation_max_spread_ticks = _config.quoting.obligation_max_spread_ticks;
            qcfg.obligation_level = _config.quoting.obligation_level;
            qcfg.scout_qty = _config.quoting.scout_qty;

            quoter->init(qcfg);
            // Note: UnifiedOrderTracker will be set after it's created (in section 5)
            _quoters[ci.code] = std::move(quoter);
        }

        WTSLogger::info("FutuQuoter: {} quoters initialized, {} levels, baseSpread={}",
                        _quoters.size(),
                        _config.quoting.num_levels,
                        _config.quoting.base_spread);
    } else {
        WTSLogger::info("FutuQuoter: skipped (market making disabled)");
    }

    //------------------------------------------------------------
    // 3. SpreadOptimizer（价差优化器）- 每合约一个 (仅做市)
    //------------------------------------------------------------
    if (_config.modules.use_market_making && _config.modules.use_spread_optimizer) {
        _coordinator->setPortfolioMaxDelta(_config.portfolio.max_delta);
        const CoordinatorConfig& updated_cfg = _coordinator->getConfig();
        const auto& mp = updated_cfg.modules;

        for (const auto& ci : _contract_infos) {
            auto optimizer = FutuComponentFactory::createSpreadOptimizer(
                updated_cfg, ci.code, _config.quoting.base_spread, ci.tick_size);

            _spread_optimizers[ci.code] = std::move(optimizer);
        }

        WTSLogger::info("SpreadOptimizer: {} optimizers, baseSpread={}, maxDelta={} (soft)",
                        _spread_optimizers.size(),
                        _config.quoting.base_spread,
                        _config.portfolio.max_delta);
    }

    //------------------------------------------------------------
    // 4. UnifiedOrderTracker + AutoCancelPolicy（统一订单跟踪）
    //------------------------------------------------------------
    _order_tracker = std::make_unique<UnifiedOrderTracker>();

    {
        const auto& mp = coord_cfg.modules;

        UnifiedTrackerConfig tracker_cfg;
        tracker_cfg.max_orders = _config.order_control.max_orders;
        tracker_cfg.max_age_ms = mp.auto_cancel_max_age_ms;
        tracker_cfg.price_deviation = mp.auto_cancel_price_deviation;
        tracker_cfg.sticky_threshold = _config.quoting.sticky_threshold;
        // STP 唯一权威: coordinator.yaml modules.selfTradePrevention。
        // arb 启用时强制开启, 防止 arb 对手价单打到自己 MM 盘口。
        // Reason: arb sends marketable orders via OrderRouter that can cross own MM quotes.
        // Without STP, arb would self-trade with its own MM book.
        bool stp_effective = _config.modules.use_self_trade_prevention || _config.modules.use_spread_arbitrage;
        if (_config.modules.use_spread_arbitrage && !_config.modules.use_self_trade_prevention) {
            WTSLogger::info("STP forced ON because use_spread_arbitrage=true (prevents arb→MM self-trade)");
        }
        tracker_cfg.stp_enabled = stp_effective;
        tracker_cfg.stp_min_price_gap = _config.modules.stp_min_price_gap;
        _order_tracker->setConfig(tracker_cfg);
    }

    // 为所有 FutuQuoter 设置共享 tracker
    for (auto& [code, quoter] : _quoters) {
        quoter->setOrderTracker(_order_tracker.get());
    }

    // R3 v2: BilateralQuoteStats 已下放到 Per-Quoter 值成员
    //   - 每个 quoter 持值成员，setConfig(min_valid_qty/obligation_max_spread_ticks) 来自 quoter 自身 cfg
    //   - sessInfo 从 _session_cache 取（on_init 中已用三段式查询并缓存）
    //   - 不再重复调 stra_get_comminfo（两段式 code 查不到品种信息）
    uint32_t stats_ok = 0;
    uint32_t stats_fail = 0;
    for (auto& [code, quoter] : _quoters) {
        WTSSessionInfo* sessInfo = nullptr;
        auto scIt = _session_cache.find(code);
        if (scIt != _session_cache.end())
            sessInfo = scIt->second.sessInfo;
        if (quoter->initBilateralStats(sessInfo))
            stats_ok++;
        else
            stats_fail++;
    }
    WTSLogger::info(
        "BilateralQuoteStats: Per-Quoter init done, ok={} fail={} (total={})", stats_ok, stats_fail, _quoters.size());

    WTSLogger::info(
        "UnifiedOrderTracker: initialized (shared by {} FutuQuoters + AutoCancelPolicy + SelfTradePrevention)",
        _quoters.size());

    //------------------------------------------------------------
    // 5.5 注册模块至 StrategyCoordinator
    //------------------------------------------------------------

    // Register modules with coordinator
    StrategyCoordinator::CoordinatorDeps deps;  // B7: consolidated dep injection (replaces 15 setters)
    deps.order_tracker = _order_tracker.get();
    deps.quoters = &_quoters;
    deps.spread_opts = &_spread_optimizers;
    deps.market_data = &_market_data;
    deps.signal_aggregators = &_signal_aggregators;

    // Register modules created before coordinator
    deps.portfolio = _portfolio.get();
    deps.correlation_manager = _correlation_manager.get();

    //------------------------------------------------------------
    // 5.6 OrderRouter（统一下单路由器 — 套利/对冲/平仓）
    //------------------------------------------------------------
    _order_router = std::make_unique<OrderRouter>();
    _order_router->setOrderTracker(_order_tracker.get());

    // 设置限速：套利30单/秒，对冲10单/秒，平仓不限速
    _order_router->setRateLimit(Source::ARBITRAGE, 30, 1000);
    _order_router->setRateLimit(Source::HEDGING, 30, 1000);
    _order_router->setRateLimit(Source::CLOSEOUT, 0, 1000); // 0 = 不限速

    deps.order_router = _order_router.get();
    WTSLogger::info("OrderRouter: initialized (arb=30/s, hedge=30/s, closeout=unlimited)");

    //------------------------------------------------------------
    // 5.7 CloseoutExecutor（渐进式收盘对冲执行器）
    //------------------------------------------------------------
    _closeout_executor = std::make_unique<CloseoutExecutor>();
    _closeout_executor->setOrderRouter(_order_router.get());
    _closeout_executor->setOrderTracker(_order_tracker.get());
    _closeout_executor->setPortfolio(_portfolio.get());
    {
        CloseoutExecConfig exec_cfg;
        exec_cfg.drain_timeout_ms = _config.closeout.drain_timeout_ms;
        exec_cfg.depth_ratio_passive = _config.closeout.depth_ratio_passive;
        exec_cfg.depth_ratio_mid = _config.closeout.depth_ratio_mid;
        exec_cfg.depth_ratio_aggr = _config.closeout.depth_ratio_aggr;
        exec_cfg.sweep_threshold_ms = _config.closeout.sweep_threshold_ms;
        exec_cfg.sweep_ticks = _config.closeout.sweep_ticks;
        exec_cfg.use_fak = _config.closeout.use_fak;
        _closeout_executor->setConfig(exec_cfg);
    }
    WTSLogger::info("CloseoutExecutor: initialized (drain={}ms, sweep_ticks={}, fak={})",
                    _config.closeout.drain_timeout_ms,
                    _config.closeout.sweep_ticks,
                    _config.closeout.use_fak);

    // 设置交易时段信息（用于休市检查）
    for (const auto& [code, cache] : _session_cache) {
        _coordinator->setSessionInfo(code, cache.sessInfo);
    }

    // Initialize coordinator internal components
    _coordinator->initialize();

    WTSLogger::info("StrategyCoordinator: initialized (modules registered)");

    //------------------------------------------------------------
    // 6. FutuRiskMonitor（风险监控）
    //------------------------------------------------------------
    _risk_monitor = std::make_unique<FutuRiskMonitor>();

    // 频率/速率/仓位/delta 阈值: 单一来源, 直接整体拷贝 (消除逐字段手工搬运)
    RateLimits rate_limits = _config.risk.rate_limits;
    _risk_monitor->setRateLimits(rate_limits);
    _risk_monitor->setMaxPendingPerSide(_config.order_control.max_pending_per_side);

    // 设置恢复配置
    RecoveryConfig recovery_cfg;
    recovery_cfg.cooldown_ms = _config.risk.cooldown_ms;
    recovery_cfg.check_interval_ms = _config.risk.check_interval_ms;
    recovery_cfg.recovery_threshold = _config.risk.recovery_threshold;
    recovery_cfg.max_recovery_count = _config.risk.max_recovery_count;
    recovery_cfg.pnl_recovery_ratio = _config.risk.pnl_recovery_ratio;
    recovery_cfg.max_loss_for_recovery = _config.risk.max_loss_for_recovery;
    recovery_cfg.auto_clear_irreversible_on_reset = _config.risk.auto_clear_irreversible_on_reset;
    _risk_monitor->setRecoveryConfig(recovery_cfg);

    CloseoutConfig closeout_cfg;
    closeout_cfg.minutes_before = _config.closeout.minutes_before;
    closeout_cfg.max_retries = _config.closeout.max_retries;
    closeout_cfg.retry_interval_ms = _config.closeout.retry_interval_ms;
    closeout_cfg.night_close_time = _config.closeout.night_close_time;
    closeout_cfg.night_minutes_before = _config.closeout.night_minutes_before;
    _risk_monitor->setCloseoutConfig(closeout_cfg);

    WTSLogger::info("FutuRiskMonitor: maxOrdersPerSec={}, maxCancelsPerSec={}, cooldownMs={}, recoveryThreshold={}, "
                    "maxDeltaChangePerSec={}",
                    _config.risk.rate_limits.max_orders_per_sec,
                    _config.risk.rate_limits.max_cancels_per_sec,
                    _config.risk.cooldown_ms,
                    _config.risk.recovery_threshold,
                    _config.risk.rate_limits.max_delta_change_per_sec);

    // Register with coordinator
    if (_coordinator) {
        deps.risk_monitor = _risk_monitor.get();
    }

    // CloseoutOrchestrator 依赖注入 (架构重构 C3) — 所有依赖模块至此均已创建
    {
        CloseoutOrchestrator::Deps orch_deps;
        orch_deps.executor = _closeout_executor.get();
        orch_deps.risk_monitor = _risk_monitor.get();
        orch_deps.portfolio = _portfolio.get();
        orch_deps.trading_state = &_trading_state;
        orch_deps.order_router = _order_router.get();
        orch_deps.quoters = &_quoters;
        orch_deps.anchor_code = &_config.anchor_code;
        orch_deps.close_time = _config.closeout.close_time;
        orch_deps.flatten_position = _config.closeout.flatten_position;
        orch_deps.strategy_id = s.id();
        _closeout_orch.setDeps(orch_deps);
    }

    //------------------------------------------------------------
    // 7. MarketDataContext（核心行情上下文）
    //------------------------------------------------------------
    for (const auto& ci : _contract_infos) {
        _market_data.emplace(ci.code, FutuComponentFactory::createMarketDataContext(coord_cfg));
    }
    WTSLogger::info("MarketDataContext: mandatory core enabled");

    //------------------------------------------------------------
    // 7.1 SignalAggregator（信号聚合器）- 新信号架构
    //------------------------------------------------------------
    if (_config.modules.use_market_making && coord_cfg.use_signal_aggregator) {
        SignalAggregatorConfig sig_cfg;
        wtp::WTSVariant* root = coord_cfg._raw_variant;
        wtp::WTSVariant* modules_v = root ? root->get("modules") : nullptr;
        if (modules_v) {
            wtp::WTSVariant* sig_v = modules_v->get("signalAggregator");
            if (sig_v)
                sig_cfg = SignalAggregatorConfig::fromVariant(sig_v);
        }

        // model.type 校验失败 -> 跳过 SignalAggregator 初始化
        if (!sig_cfg.valid) {
            WTSLogger::error("SignalAggregator: config invalid (unsupported model type), skipping initialization");
            return;
        }

        for (const auto& ci : _contract_infos) {
            auto aggregator = std::make_unique<SignalAggregator>(sig_cfg);

            // Configure LeadLag: anchor contract is the lead for all non-anchor contracts
            if (sig_cfg.use_lead_lag && !_config.anchor_code.empty()) {
                if (ci.code != _config.anchor_code) {
                    // Non-anchor contract: anchor is its lead
                    aggregator->addLeadContract(_config.anchor_code, 1.0);
                }
                // Anchor contract itself doesn't need a lead (it IS the lead)
            }

            _signal_aggregators[ci.code] = std::move(aggregator);
        }

        WTSLogger::info("SignalAggregator: {} aggregators initialized "
                        "(ofi={:.2f}, trade={:.2f}, book={:.2f}, mom={:.2f}, lead_lag={:.2f})",
                        _signal_aggregators.size(),
                        sig_cfg.ofi_weight,
                        sig_cfg.trade_weight,
                        sig_cfg.book_imbalance_weight,
                        sig_cfg.momentum_weight,
                        sig_cfg.lead_lag_weight);
    } else if (_config.modules.use_market_making) {
        WTSLogger::info("SignalAggregator: skipped (use_signal_aggregator=false, using legacy architecture)");
    }

    //------------------------------------------------------------
    // 8. MicroAlphaEngine（已移除）
    // 新架构 (SignalAggregator) 已包含所有 Alpha 信号计算
    // MicroAlphaEngine 不再需要，已完全移除
    //------------------------------------------------------------
    WTSLogger::info("MicroAlphaEngine: DISABLED (SignalAggregator handles all alpha signals)");

    //------------------------------------------------------------
    // 9. ToxicFlowDetector（毒性流动检测器）(仅做市)
    //------------------------------------------------------------
    if (_config.modules.use_market_making && _config.modules.use_toxicity_detector) {
        _toxicity_detector = FutuComponentFactory::createToxicFlowDetector(coord_cfg);

        // Register with coordinator
        if (_coordinator) {
            deps.toxicity = _toxicity_detector.get();
        }

        WTSLogger::info("ToxicFlowDetector: created");
    } else {
        WTSLogger::info("ToxicFlowDetector: disabled");
    }

    //------------------------------------------------------------
    // 9.5 SelfTradeCalibrator（统一管理自身成交，供毒性检测和综合信号使用）
    //------------------------------------------------------------
    {
        _self_trade_calibrator = FutuComponentFactory::createSelfTradeCalibrator(coord_cfg);
    }

    // 校准器 tick_size 从合约基础信息统一获取（不再单独在策略层配置）
    for (const auto& ci : _contract_infos) {
        if (ci.code == _config.anchor_code) {
            _self_trade_calibrator->setTickSize(ci.tick_size);
            break;
        }
    }

    // 将校准器设置到毒性检测器（统一使用 SelfTradeCalibrator 管理 Fill 记录）
    if (_toxicity_detector) {
        _toxicity_detector->setSelfTradeCalibrator(_self_trade_calibrator.get());
    }

    // Register with coordinator
    if (_coordinator) {
        deps.self_trade_calibrator = _self_trade_calibrator.get();
    }

    {
        WTSLogger::info("SelfTradeCalibrator: created");
    }

    //------------------------------------------------------------
    // 11. PerformanceAnalyzer（绩效分析器）
    //------------------------------------------------------------
    if (_config.modules.use_performance_analyzer) {
        _perf_analyzer = FutuComponentFactory::createPerformanceAnalyzer(coord_cfg);
        WTSLogger::info("PerformanceMonitor: latencyThreshold={}ns", _config.perf.monitor_latency_threshold);
    } else {
        WTSLogger::info("PerformanceAnalyzer: disabled");
    }

    //------------------------------------------------------------
    // 12. PerformanceMonitor（性能监控）
    //------------------------------------------------------------
    if (_config.modules.use_performance_monitor) {
        _performance_monitor = FutuComponentFactory::createPerformanceMonitor(coord_cfg);
        // 接线到 Coordinator — 旧代码漏接, 协调器内 recordTickToQuote 因空指针永不执行
        deps.perf_monitor = _performance_monitor.get();
        TscClock::calibrate(); // P0: rdtsc→ns 系数一次性校准 (10ms)
        WTSLogger::info("PerformanceMonitor: latencyThreshold={}ns", _config.perf.monitor_latency_threshold);
    } else {
        WTSLogger::info("PerformanceMonitor: disabled");
    }

    //------------------------------------------------------------
    // 14. SpreadArbitrageManager（跨期价差套利管理器）
    // 独立配置文件: 从 config 中读取 spread_arbitrage_config
    //------------------------------------------------------------
    if (_config.modules.use_spread_arbitrage) {
        _spread_arb_manager = std::make_unique<SpreadArbitrageManager>();

        // 加载独立配置文件
        std::string arb_cfg_path =
            _config.spread_arbitrage_config.empty() ? "spread_arbitrage.yaml" : _config.spread_arbitrage_config;
        if (_spread_arb_manager->loadConfig(arb_cfg_path)) {
            WTSLogger::info("SpreadArbitrageManager: loaded config from {}", arb_cfg_path);
        } else {
            // 加载失败，使用默认配置
            SpreadArbitrageConfig arb_cfg;
            arb_cfg.enabled = true;
            arb_cfg.enhance_market_making = true;
            arb_cfg.max_total_position = 20.0;
            _spread_arb_manager->setConfig(arb_cfg);

            WTSLogger::warn("SpreadArbitrageManager: using default config (file load failed from {})", arb_cfg_path);
        }

        // Scheme B-3: inject Portfolio SSOT for portfolio-derived spread monitoring.
        // Must be set before any generateSignal call. Portfolio outlives SpreadArbMgr.
        _spread_arb_manager->setPortfolio(_portfolio.get());
        _spread_arb_manager->setInFlightTimeoutMs(120000ULL); // 120 seconds
        WTSLogger::info("SpreadArbitrageManager: B-3 gate enabled (Portfolio SSOT, in_flight_timeout={}ms)", 120000ULL);
    } else {
        WTSLogger::info("SpreadArbitrageManager: disabled");
    }
    //------------------------------------------------------------
    // 15. SelfTradePrevention（自成交防护模块）
    //------------------------------------------------------------
    if (_config.modules.use_self_trade_prevention || _config.modules.use_spread_arbitrage) {
        _stp = FutuComponentFactory::createSelfTradePrevention(coord_cfg, _order_tracker.get());
        WTSLogger::info("SelfTradePrevention: enabled, strategy=CANCEL_MM, using UnifiedOrderTracker");
    }

    //------------------------------------------------------------
    // 16. AsyncArbitrageExecutor（异步套利执行器）
    //------------------------------------------------------------
    if (_config.modules.use_spread_arbitrage) {
        _async_arb = FutuComponentFactory::createAsyncArbitrageExecutor(coord_cfg);
        _async_arb->setArbitrageManager(_spread_arb_manager.get());
        _async_arb->setSelfTradePrevention(_stp.get());
        // A10: 接线利润门槛 (此前 setMinProfitThreshold 无调用者, 价格调整成本检查恒放行)
        _async_arb->setMinProfitThreshold(_spread_arb_manager->getConfig().min_profit_threshold_ticks);
        if (_coordinator) {
            deps.arb_executor = _async_arb.get();
            deps.arb_manager = _spread_arb_manager.get(); // B2: 平仓 intent 查询通道
        }
        // B5: 过冲保险丝 — Portfolio sign-flip → arb_manager->onOvershootDetected
        if (_portfolio) {
            _portfolio->setArbManager(_spread_arb_manager.get());
        }

        // 设置每个合约的 tick size（用于套利订单价格调整）
        for (const auto& ci : _contract_infos) {
            if (ci.tick_size > 0) {
                _async_arb->updateTickSize(ci.code, ci.tick_size);
            }
        }

        WTSLogger::info("AsyncArbitrageExecutor: enabled, signalInterval=5000us, ticksPerSignal=5");

        // ArbExecutionBridge 依赖注入 (架构重构 C4)
        {
            ArbExecutionBridge::Deps bridge_deps;
            bridge_deps.async_arb = _async_arb.get();
            bridge_deps.arb_manager = _spread_arb_manager.get();
            bridge_deps.order_router = _order_router.get();
            bridge_deps.order_tracker = _order_tracker.get();
            bridge_deps.stp = _stp.get();
            bridge_deps.portfolio = _portfolio.get();
            bridge_deps.risk_monitor = _risk_monitor.get();
            bridge_deps.contract_infos = &_contract_infos;
            bridge_deps.use_spread_arbitrage = _config.modules.use_spread_arbitrage;
            bridge_deps.strategy_id = s.id();
            _arb_bridge.setDeps(bridge_deps);
        }
    } else {
        WTSLogger::info("AsyncArbitrageExecutor: disabled");
    }

    // 共享TradingState（必须在arb if/else块之外，否则Arb=OFF时指针为nullptr导致segfault）
    // B7: trading_state must be set before wireDeps (was setTradingState call)
    deps.trading_state = &_trading_state;
    if (_coordinator) {
        _coordinator->wireDeps(deps);      // B7: consolidated dep wiring (replaces 15 setters + setTradingState)
        _coordinator->validateDeps();      // B7: fail-fast validation
    }

    // R1: 如果 WtUftRunner 在 init 之前已注入 _event_notifier,
    // 此时所有下游(_risk_monitor / _spread_arb_manager)均已创建, 完成注入
    if (_event_notifier) {
        s.setEventNotifier(_event_notifier);
    }

    //------------------------------------------------------------
    // 初始化计数器
    //------------------------------------------------------------
    _tick_count = 0;
    // _param_update_interval 从配置读取

    // 从配置更新下单错误处理参数
    // order_error_threshold used directly from _config.order_control

    //------------------------------------------------------------
    // v7.7 A3: 依赖完备性校验 (启动期 fail-fast)
    //   20+ 裸指针 setter 注入, 生命周期靠口头约定; 此处收口断言
    //   核心依赖非空, 把运行期空指针崩溃前移为启动期明确报错。
    //------------------------------------------------------------
    {
        uint32_t dep_errors = 0;
        auto require = [&](const void* p, const char* name) {
            if (!p) {
                WTSLogger::error("[ASSEMBLY] REQUIRED dependency missing: {}", name);
                dep_errors++;
            }
        };
        require(_coordinator.get(), "coordinator");
        require(_portfolio.get(), "portfolio");
        require(_order_tracker.get(), "order_tracker");
        require(_order_router.get(), "order_router");
        require(_risk_monitor.get(), "risk_monitor");
        require(_closeout_executor.get(), "closeout_executor");
        if (_config.modules.use_market_making) {
            if (_quoters.empty()) {
                WTSLogger::error("[ASSEMBLY] REQUIRED: quoters empty (MM enabled)");
                dep_errors++;
            }
            require(_correlation_manager.get(), "correlation_manager (MM enabled)");
        }
        if (_config.modules.use_spread_arbitrage) {
            require(_spread_arb_manager.get(), "spread_arb_manager (arb enabled)");
            require(_async_arb.get(), "async_arb (arb enabled)");
            require(_stp.get(), "stp (arb enabled)");
        }
        if (dep_errors > 0)
            WTSLogger::error(
                "[ASSEMBLY] {} required dependencies MISSING - strategy will misbehave, review assembly order!",
                dep_errors);
        else
            WTSLogger::info("[ASSEMBLY] dependency completeness check passed (0 missing)");
    }
}

void FutuModuleAssembler::loadContractInfos(UftFutuMmStrategy& s, wtp::IUftStraCtx* ctx)
{
    auto& _contract_infos = s._contract_infos;
    auto& _session_cache = s._session_cache;
    auto& _config = s._config;

    // v7.6: _last_mid 定码预填 (init 后结构不可变, 值原子, MdSpi/TdSpi 无锁共享)
    for (const auto& ci : _contract_infos) {
        s._last_mid[ci.code] = std::make_unique<UftFutuMmStrategy::MidSlot>();
    }

    for (auto& ci : _contract_infos) {
        std::string stdCode = fullCodeToStdCode(ci.code);
        WTSCommodityInfo* commInfo = ctx->stra_get_comminfo(stdCode.c_str());

        if (commInfo) {
            _session_cache[ci.code] = {commInfo, commInfo->getSessionInfo()};
            WTSLogger::debug("UftFutuMmStrategy[{}] Session cache added: {} -> sessInfo={}",
                             s.id(),
                             ci.code,
                             (void*)commInfo->getSessionInfo());

            if (ci.multiplier <= 0)
                ci.multiplier = commInfo->getVolScale();
            if (ci.tick_size <= 0)
                ci.tick_size = commInfo->getPriceTick();

            if (commInfo->getSessionInfo()) {
                const auto& sections = commInfo->getSessionInfo()->getTradingSections();
                uint32_t dayCloseTime = 150000;
                uint32_t nightCloseTime = 0; // 0 = 无夜盘

                for (const auto& section : sections) {
                    uint32_t startTime = section.first_raw;
                    uint32_t endTime = section.second_raw;

                    // 白盘收盘: endTime > 600 且 <= 2359 (如 1130, 1500, 1515)
                    if (endTime > 600 && endTime <= 2359) {
                        dayCloseTime = endTime * 100; // 转为HHMMSS格式
                    }

                    // 夜盘收盘: endTime <= 600 (凌晨，如 100=01:00, 230=02:30)
                    //           或 startTime >= 2100 且 endTime <= 2359 (不跨日，如 2300, 2330)
                    if (endTime <= 600 && endTime > 0) {
                        // 跨日品种: 夜盘收盘在凌晨 (01:00, 02:30)
                        nightCloseTime = endTime; // 保持HHMM格式 (如 230)
                    } else if (startTime >= 2100 && endTime <= 2359 && endTime > 600) {
                        // 不跨日品种: 夜盘收盘在当晚 (23:00, 23:30)
                        nightCloseTime = endTime; // 保持HHMM格式 (如 2300)
                    }
                }

                ci.close_time = dayCloseTime;
                ci.night_close_time = nightCloseTime;
            } else {
                ci.close_time = 150000;
            }

            if (ci.code == _config.anchor_code) {
                _config.closeout.close_time = ci.close_time;
                _config.closeout.night_close_time = ci.night_close_time;
            }

            WTSLogger::info("UftFutuMmStrategy[{}] contract {} from base: multiplier={}, tickSize={}, closeTime={}, "
                            "nightCloseTime={}",
                            s.id(),
                            ci.code,
                            ci.multiplier,
                            ci.tick_size,
                            ci.close_time,
                            ci.night_close_time);
        } else {
            if (ci.multiplier <= 0)
                ci.multiplier = 1.0;
            if (ci.tick_size <= 0)
                ci.tick_size = 0.2;
            ci.close_time = 150000;
            WTSLogger::warn(
                "UftFutuMmStrategy[{}] contract {} not found in base data, using defaults", s.id(), ci.code);
        }
    }

}

} // namespace futu
