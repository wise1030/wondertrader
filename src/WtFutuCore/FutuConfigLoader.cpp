/*!
 * \file FutuConfigLoader.cpp
 * \brief 策略配置加载器实现 (自 UftFutuMmStrategy::init 搬移, 逻辑零修改)
 */
#include "FutuConfigLoader.h"
#include "FutuConfig.h"
#include "../WTSTools/WTSLogger.h"

namespace futu
{

bool FutuConfigLoader::load(wtp::WTSVariant* cfg,
                            FutuMmConfig& _config,
                            std::vector<ContractInfo>& _contract_infos,
                            const char* id)
{
    // 局部别名 (避免 using 声明在非类作用域的编译问题)
    auto readDouble = [](wtp::WTSVariant* v, const char* k, double d) { return FutuConfig::readDouble(v, k, d); };
    auto readUInt32 = [](wtp::WTSVariant* v, const char* k, uint32_t d) { return FutuConfig::readUInt32(v, k, d); };
    auto readBool = [](wtp::WTSVariant* v, const char* k, bool d) { return FutuConfig::readBool(v, k, d); };
    auto readString = [](wtp::WTSVariant* v, const char* k, const char* d) -> std::string {
        return FutuConfig::readString(v, k, d);
    };

    //------------------------------------------------------------
    // 读取合约配置
    //------------------------------------------------------------
    _config.anchor_code = cfg->getCString("anchorCode");
    _config.is_backtest = readBool(cfg, "isBacktest", false);

    //------------------------------------------------------------
    // 读取配置文件路径
    //------------------------------------------------------------
    {
        _config.coordinator_config = readString(cfg, "coordinatorConfig", "");
        _config.spread_arbitrage_config = readString(cfg, "spreadArbitrageConfig", "");
    }

    // 读取合约列表
    // multiplier 和 tickSize 为可选参数，如未配置则从基础数据管理模块自动获取
    WTSVariant* cfgContracts = cfg->get("contracts");
    if (cfgContracts && cfgContracts->type() == WTSVariant::VT_Array) {
        for (uint32_t i = 0; i < cfgContracts->size(); i++) {
            WTSVariant* cfgItem = cfgContracts->get(i);

            ContractInfo ci;
            ci.code = cfgItem->getCString("code");
            // multiplier 和 tickSize 可选，-1 表示未配置，后续从基础数据模块获取
            ci.multiplier = readDouble(cfgItem, "multiplier", -1.0);
            ci.tick_size = readDouble(cfgItem, "tickSize", -1.0);
            // 单合约限制，-1 表示未配置，使用全局默认值
            ci.max_position = readDouble(cfgItem, "maxPosition", -1.0);
            ci.max_delta = readDouble(cfgItem, "maxDelta", -1.0);
            // 单合约目标持仓，默认0（平衡），超过时报价倾向于减仓
            ci.target_position = readDouble(cfgItem, "targetPosition", 0.0);
            // 初始本地持仓: 非零时同步覆盖local_net (用于单策略账户管理遗留持仓)

            _contract_infos.push_back(ci);
        }
    }

    //------------------------------------------------------------
    // 读取 Delta 软指标参数（用于 skew 和对冲决策，不触发风控）
    //------------------------------------------------------------
    WTSVariant* cfgRisk = cfg->get("risk");
    if (cfgRisk) {
        _config.risk.max_exposure = readDouble(cfgRisk, "maxExposure", 35000000.0);
        _config.risk.max_daily_loss = readDouble(cfgRisk, "maxDailyLoss", -200000.0);
    }

    //------------------------------------------------------------
    // 读取报价参数（嵌套在 quoting 节点下）
    //------------------------------------------------------------
    WTSVariant* cfgQuoting = cfg->get("quoting");
    if (cfgQuoting) {
        _config.quoting.num_levels = readUInt32(cfgQuoting, "numLevels", 1);
        _config.quoting.base_spread = readDouble(cfgQuoting, "baseSpread", 2.0);
        _config.quoting.base_qty = readDouble(cfgQuoting, "baseQty", 5.0);
        _config.quoting.level_qty_multiplier = readDouble(cfgQuoting, "levelQtyMultiplier", 0.7);
        _config.quoting.level_step = readDouble(cfgQuoting, "levelStep", 1.0);
        _config.quoting.sticky_threshold = readDouble(cfgQuoting, "stickyThreshold", 1.0);
        _config.quoting.improve_retreat_ratio = readDouble(cfgQuoting, "improveRetreatRatio", 2.0);
        _config.quoting.max_price_deviation = readDouble(cfgQuoting, "maxPriceDeviation", 20.0);
        _config.quoting.use_bilateral_quote = readBool(cfgQuoting, "useBilateralQuote", false);
        _config.quoting.price_protection = readBool(cfgQuoting, "priceProtection", true);
        _config.quoting.protect_ticks = readDouble(cfgQuoting, "protectTicks", 1.0);
        // v3 软风控参数
        _config.quoting.qty_decay_factor = readDouble(cfgQuoting, "qtyDecayFactor", 2.0);
        _config.quoting.obligation_min_qty = readDouble(cfgQuoting, "obligationMinQty", 10.0);
        _config.quoting.obligation_max_spread_ticks = readDouble(cfgQuoting, "obligationMaxSpreadTicks", 10.0);
        // v7.2 scout 多层结构
        _config.quoting.obligation_level = readUInt32(cfgQuoting, "obligationLevel", 0);
        _config.quoting.scout_qty = readDouble(cfgQuoting, "scoutQty", 1.0);
    }

    //------------------------------------------------------------
    // 读取组合管理参数（嵌套在 portfolio 节点下）
    //------------------------------------------------------------
    WTSVariant* cfgPortfolio = cfg->get("portfolio");
    if (cfgPortfolio) {
        _config.portfolio.max_delta = readDouble(cfgPortfolio, "maxDelta", 50.0);
        _config.portfolio.hedge_ratio = readDouble(cfgPortfolio, "hedgeRatio", 1.0);
    }

    // 下单错误处理参数（统一处理所有下单错误）
    _config.order_control.order_error_threshold = readUInt32(cfg, "orderErrorThreshold", 10);
    _config.order_control.max_orders = readUInt32(cfg, "maxOrders", 32);
    _config.order_control.max_pending_per_side = readDouble(cfg, "maxPendingPerSide", 30.0);

    // 收盘前平仓参数（嵌套在 closeout 节点下）
    WTSVariant* cfgCloseout = cfg->get("closeout");
    if (cfgCloseout) {
        _config.closeout.minutes_before = readUInt32(cfgCloseout, "minutesBefore", 5);
        _config.closeout.flatten_position = readBool(cfgCloseout, "flattenPosition", true);
        _config.closeout.max_retries = readUInt32(cfgCloseout, "maxRetries", 3);
        _config.closeout.retry_interval_ms = readUInt32(cfgCloseout, "retryIntervalMs", 5000);
        _config.closeout.night_minutes_before =
            readUInt32(cfgCloseout, "nightMinutesBefore", _config.closeout.minutes_before);
        // CloseoutExecutor 参数
        _config.closeout.drain_timeout_ms = readUInt32(cfgCloseout, "drainTimeoutMs", 3000);
        _config.closeout.depth_ratio_passive = readDouble(cfgCloseout, "depthRatioPassive", 0.3);
        _config.closeout.depth_ratio_mid = readDouble(cfgCloseout, "depthRatioMid", 0.5);
        _config.closeout.depth_ratio_aggr = readDouble(cfgCloseout, "depthRatioAggressive", 0.8);
        _config.closeout.sweep_threshold_ms = readUInt32(cfgCloseout, "sweepThresholdMs", 5000);
        _config.closeout.sweep_ticks = readUInt32(cfgCloseout, "sweepTicks", 3);
        _config.closeout.use_fak = readBool(cfgCloseout, "useFak", true);
    }
    _config.closeout.close_time = 150000;  // 默认值，on_init 中会从 anchor_code 更新
    _config.closeout.night_close_time = 0; // 默认无夜盘，on_init 中会从 anchor_code 更新

    //------------------------------------------------------------
    // 模块开关统一由 coordinator.yaml 管理(唯一权威来源):
    //   5 个策略级开关 + use_signal_aggregator + useHedging 写在 coordinator 根级;
    //   3 个模块级开关(toxicityDetector/spreadOptimizer/adaptiveParam) 写在
    //   coordinator 的 modules.<name>.enabled 子键.
    // config.yaml 不再承载任何开关, 避免多源错配.
    // 字段初值见 UftFutuMmStrategy.h::Modules 构造函数(仅作 coordinator
    // 缺失该键时的编译期默认, 不构成"第二处配置").
    //------------------------------------------------------------

    //------------------------------------------------------------
    // 读取 FutuRiskMonitor 参数（嵌套在 risk.frequency 节点下）
    //------------------------------------------------------------
    if (cfgRisk) {
        WTSVariant* cfgFrequency = cfgRisk->get("frequency");
        if (cfgFrequency) {
            // 频率/速率/仓位/delta 阈值: 单一来源 (RiskRateLimits, 见 RiskLimitsConfig.h)
            _config.risk.rate_limits = RiskRateLimits::fromVariant(cfgFrequency);
            _config.risk.cooldown_ms = readUInt32(cfgFrequency, "cooldownMs", 30000);
            _config.risk.check_interval_ms = readUInt32(cfgFrequency, "checkIntervalMs", 5000);
            _config.risk.recovery_threshold = readDouble(cfgFrequency, "recoveryThreshold", 0.8);
            _config.risk.max_recovery_count = readUInt32(cfgFrequency, "maxRecoveryCount", 3);
            _config.risk.pnl_recovery_ratio = readDouble(cfgFrequency, "pnlRecoveryRatio", 0.5);
            _config.risk.max_loss_for_recovery = readDouble(cfgFrequency, "maxLossForRecovery", 0);
            _config.risk.auto_clear_irreversible_on_reset =
                readBool(cfgFrequency, "autoClearIrreversibleOnReset", false);
        }
    }

    //------------------------------------------------------------
    // 读取 PerformanceMonitor 参数（嵌套在 performance 节点下）
    //------------------------------------------------------------
    WTSVariant* cfgPerformance = cfg->get("performance");
    if (cfgPerformance) {
        _config.perf.monitor_latency_threshold = (uint64_t)readDouble(cfgPerformance, "latencyThreshold", 100000);
        _config.perf.enabled = readBool(cfgPerformance, "enabled", true);
        _config.perf.log_interval = readUInt32(cfgPerformance, "logInterval", 1000);
        _config.perf.warn_threshold_ns = readUInt32(cfgPerformance, "warnThresholdNs", 10000);
        _config.perf.critical_threshold_ns = readUInt32(cfgPerformance, "criticalThresholdNs", 50000);
    }

    //------------------------------------------------------------
    // 注意：以下模块参数已移至独立配置文件:
    //   - SpreadArbitrage -> spread_arbitrage.yaml
    //   - SelfTradePrevention -> coordinator.yaml modules
    //   - AsyncExecutor -> spread_arbitrage.yaml
    //------------------------------------------------------------

    //------------------------------------------------------------
    // 参数边界校验（不影响运行时延迟）
    //------------------------------------------------------------
    {
        // Delta 软指标参数校验（用于 skew 和对冲决策，不触发风控）
        if (_config.portfolio.max_delta <= 0 || _config.portfolio.max_delta > 100000000) {
            WTSLogger::error(
                "UftFutuMmStrategy[{}] invalid maxDelta: {}, expected (0, 100000000]", id, _config.portfolio.max_delta);
            return false;
        }
        if (_config.risk.max_exposure <= 0) {
            WTSLogger::error(
                "UftFutuMmStrategy[{}] invalid maxExposure: {}, expected > 0", id, _config.risk.max_exposure);
            return false;
        }

        // 报价参数校验
        if (_config.quoting.num_levels == 0 || _config.quoting.num_levels > 10) {
            WTSLogger::error(
                "UftFutuMmStrategy[{}] invalid numLevels: {}, expected [1, 10]", id, _config.quoting.num_levels);
            return false;
        }
        if (_config.quoting.level_step <= 0 || _config.quoting.level_step > 100.0) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid levelStep: {}, expected (0, 100] (<=0 会破坏价格阶梯, "
                             "内层价格不再优于外层)",
                             id,
                             _config.quoting.level_step);
            return false;
        }
        if (_config.quoting.obligation_level >= _config.quoting.num_levels) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid obligationLevel: {}, must be < numLevels {}",
                             id,
                             _config.quoting.obligation_level,
                             _config.quoting.num_levels);
            return false;
        }
        if (_config.quoting.scout_qty <= 0 || _config.quoting.scout_qty > _config.quoting.base_qty) {
            WTSLogger::warn(
                "UftFutuMmStrategy[{}] scoutQty={} out of typical range (0, baseQty={}]; scout 应小于义务层手数",
                id,
                _config.quoting.scout_qty,
                _config.quoting.base_qty);
        }
        // v7.2: 路径A(handleBilateralQuote)硬编码 i==0 且不感知 scoutQty/obligationLevel,
        //   与 scout 多层结构(obligationLevel!=0)不兼容 -> L0 会被当双边义务单处理.
        //   路径A为"后议"项, 当前配置均 useBilateralQuote=false 不触发; 此处告警堵潜在误用
        if (_config.quoting.use_bilateral_quote && _config.quoting.obligation_level != 0) {
            WTSLogger::error("UftFutuMmStrategy[{}] useBilateralQuote=true 与 obligationLevel={} 不兼容 "
                             "(路径A硬编码L0双边, 不支持scout多层); 设 useBilateralQuote=false 或 obligationLevel=0",
                             id,
                             _config.quoting.obligation_level);
            return false;
        }
        if (_config.quoting.base_spread <= 0 || _config.quoting.base_spread > 20) {
            WTSLogger::error(
                "UftFutuMmStrategy[{}] invalid baseSpread: {}, expected (0, 20]", id, _config.quoting.base_spread);
            return false;
        }
        if (_config.quoting.base_qty <= 0 || _config.quoting.base_qty > 100) {
            WTSLogger::error(
                "UftFutuMmStrategy[{}] invalid baseQty: {}, expected (0, 100]", id, _config.quoting.base_qty);
            return false;
        }
        if (_config.quoting.level_qty_multiplier < 0.1 || _config.quoting.level_qty_multiplier > 1.0) {
            WTSLogger::warn("UftFutuMmStrategy[{}] levelQtyMultiplier={} out of typical range [0.1, 1.0]",
                            id,
                            _config.quoting.level_qty_multiplier);
        }

        if (_config.portfolio.hedge_ratio < 0 || _config.portfolio.hedge_ratio > 1.0) {
            WTSLogger::error(
                "UftFutuMmStrategy[{}] invalid hedgeRatio: {}, expected [0, 1]", id, _config.portfolio.hedge_ratio);
            return false;
        }

        // P1优化: Sticky 和价格验证参数校验
        if (_config.quoting.sticky_threshold <= 0 || _config.quoting.sticky_threshold > 10.0) {
            WTSLogger::warn("UftFutuMmStrategy[{}] stickyThreshold={} out of typical range (0, 10]",
                            id,
                            _config.quoting.sticky_threshold);
        }
        if (_config.quoting.max_price_deviation < 0 || _config.quoting.max_price_deviation > 100.0) {
            WTSLogger::warn("UftFutuMmStrategy[{}] maxPriceDeviation={} out of typical range [0, 100]",
                            id,
                            _config.quoting.max_price_deviation);
        }

        // 注意：Alpha 参数校验已移至 coordinator.yaml 加载时
        // 注意：SpreadArbitrage 参数校验已移至 spread_arbitrage.yaml 加载时

        // 流控参数校验
        if (_config.risk.rate_limits.max_orders_per_sec == 0 || _config.risk.rate_limits.max_orders_per_sec > 500) {
            WTSLogger::error("UftFutuMmStrategy[{}] invalid maxOrdersPerSec: {}, expected [1, 500]",
                             id,
                             _config.risk.rate_limits.max_orders_per_sec);
            return false;
        }

        WTSLogger::info("UftFutuMmStrategy[{}] parameter validation passed", id);
    }

    // 注意：业务模块初始化移到 on_init 中，以便从基础数据管理模块获取合约参数

    //------------------------------------------------------------
    // MonitorBridge (WtMonSvr GUI 数据桥) — 可选, 默认关
    //------------------------------------------------------------
    {
        WTSVariant* cfgMon = cfg->get("monitor");
        if (cfgMon) {
            _config.monitor.enabled = readBool(cfgMon, "enabled", false);
            _config.monitor.flush_interval_ms = readUInt32(cfgMon, "flushIntervalMs", 1000);
        }
    }

    return true;
}

} // namespace futu
