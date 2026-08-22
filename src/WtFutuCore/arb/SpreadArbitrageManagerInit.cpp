/*!
 * \file SpreadArbitrageManagerInit.cpp
 * \brief SpreadArbitrageManager 装配段 (V8-R4b/S-4 文件级二分)
 *
 * 与 SpreadArbitrageManager.cpp 同一个 class 的两个翻译单元:
 * 本文件 = 纯装配 (配置加载/策略注册, 仅启动期执行, 非热路径);
 * 主文件 = 运行时 (tick 扇出/信号生成/B-3 门/in_flight/B1/B5/B6)。
 * 零 API 变化, 纯搬运。
 */
#include "SpreadArbitrageManager.h"
#include "../../Includes/WTSVariant.hpp"
#include "../../WTSUtils/WTSCfgLoader.h"
#include "../../WTSTools/WTSLogger.h"

namespace futu
{

bool SpreadArbitrageManager::loadConfig(const std::string& config_file)
{
    // Load YAML file using WTSVariant
    WTSVariant* cfg = WTSCfgLoader::load_from_file(config_file);
    if (!cfg) {
        WTSLogger::error("SpreadArbitrageManager: Failed to load config from {}", config_file);
        return false;
    }

    // Get spread_arbitrage section
    WTSVariant* arb = cfg->get("spread_arbitrage");
    if (!arb) {
        WTSLogger::error("SpreadArbitrageManager: Missing 'spread_arbitrage' section in {}", config_file);
        return false;
    }

    // Helper functions
    auto readBool = [](WTSVariant* v, const char* key, bool defVal) -> bool {
        if (!v)
            return defVal;
        WTSVariant* node = v->get(key);
        return node ? node->asBoolean() : defVal;
    };
    auto readDouble = [](WTSVariant* v, const char* key, double defVal) -> double {
        if (!v)
            return defVal;
        WTSVariant* node = v->get(key);
        return node ? node->asDouble() : defVal;
    };
    auto readUInt32 = [](WTSVariant* v, const char* key, uint32_t defVal) -> uint32_t {
        if (!v)
            return defVal;
        WTSVariant* node = v->get(key);
        return node ? (uint32_t)node->asInt64() : defVal;
    };
    auto readInt32 = [](WTSVariant* v, const char* key, int defVal) -> int {
        if (!v)
            return defVal;
        WTSVariant* node = v->get(key);
        return node ? (int)node->asInt64() : defVal;
    };
    auto readString = [](WTSVariant* v, const char* key, const std::string& defVal) -> std::string {
        if (!v)
            return defVal;
        WTSVariant* node = v->get(key);
        return node ? node->asString() : defVal;
    };

    // Read basic settings
    _config.enabled = readBool(arb, "enabled", false);
    _config.enhance_market_making = readBool(arb, "enhanceMarketMaking", true);

    // Read strategy selection
    std::string strategy_str = readString(arb, "primaryStrategy", "mean_reversion");
    if (strategy_str == "mean_reversion") {
        _config.primary_strategy = ArbitrageStrategy::MEAN_REVERSION;
    } else if (strategy_str == "statistical") {
        _config.primary_strategy = ArbitrageStrategy::STATISTICAL_ARB;
    } else if (strategy_str == "pairs_trading") {
        _config.primary_strategy = ArbitrageStrategy::PAIRS_TRADING;
    } else if (strategy_str == "trend_following") {
        _config.primary_strategy = ArbitrageStrategy::TREND_FOLLOWING;
    }

    // Read portfolio settings
    _config.max_total_position = readDouble(arb, "maxTotalPosition", 50.0);
    _config.max_pairs = readUInt32(arb, "maxPairs", 10);

    // Read signal settings
    _config.min_signal_confidence = readDouble(arb, "minSignalConfidence", 0.3);
    _config.signal_cooldown_ms = readUInt32(arb, "signalCooldownMs", 1000);

    // Read MM enhancement weight

    // A10: 开仓信号最低利润门槛 (ticks), 接线到 AsyncArbitrageExecutor
    _config.min_profit_threshold_ticks = readDouble(arb, "minProfitThresholdTicks", 1.0);

    // ----------------------------------------------------------------------
    // C0: arb_close 分级平仓配置 (默认 enabled=false = 纯 B-3, 行为不变)
    // ----------------------------------------------------------------------
    WTSVariant* close_node = arb->get("arb_close");
    if (close_node) {
        _arb_close_cfg.enabled = readBool(close_node, "enabled", false);
        WTSVariant* allow = close_node->get("allow_signals");
        if (allow) {
            _arb_close_cfg.allow_signals.close_long = readBool(allow, "close_long", false);
            _arb_close_cfg.allow_signals.close_short = readBool(allow, "close_short", false);
            _arb_close_cfg.allow_signals.timeout_exit = readBool(allow, "timeout_exit", false);
            _arb_close_cfg.allow_signals.stop_loss = readBool(allow, "stop_loss", false);
        }
        WTSVariant* slp = close_node->get("stop_loss_policy");
        if (slp) {
            _arb_close_cfg.stop_loss_policy.order_flag = readInt32(slp, "order_flag", 1);
            _arb_close_cfg.stop_loss_policy.timeout_ms = readUInt32(slp, "timeout_ms", 1000);
        }
        WTSVariant* top = close_node->get("timeout_policy");
        if (top) {
            _arb_close_cfg.timeout_policy.order_flag = readInt32(top, "order_flag", 0);
            _arb_close_cfg.timeout_policy.timeout_ms = readUInt32(top, "timeout_ms", 30000);
        }
        _arb_close_cfg.max_close_size_pct = readDouble(close_node, "max_close_size_pct", 0.5);
        _arb_close_cfg.close_in_flight_timeout_ms = readUInt32(close_node, "close_in_flight_timeout_ms", 5000);
        _arb_close_cfg.oversold_protection = readBool(close_node, "oversold_protection", true);
        _arb_close_cfg.overshoot_cooldown_ms = readUInt32(close_node, "overshoot_cooldown_ms", 3600000);
        _arb_close_cfg.intent_broadcast = readBool(close_node, "intent_broadcast", true);

        WTSLogger::info("SpreadArbMgr: arb_close loaded, enabled={}, allow[SL={},TO={},CL={},CS={}]",
                        _arb_close_cfg.enabled,
                        _arb_close_cfg.allow_signals.stop_loss,
                        _arb_close_cfg.allow_signals.timeout_exit,
                        _arb_close_cfg.allow_signals.close_long,
                        _arb_close_cfg.allow_signals.close_short);
    }

    // Read statistical sub-strategy parameters (global defaults, per-pair overrides in pairs section)
    // A9: 键名与 spread_arbitrage.yaml 对齐 (entryZThreshold/stopLossZ/addSafetyRatio/stopLossPct/maxTrendBars)
    // V8-R3: 无消费的键已删 (halfLife/correlationWindow/minCorrelation/lookbackWindow/entryZThreshold/maPeriod/breakoutThreshold)
    WTSVariant* statistical = arb->get("statistical");
    if (statistical) {
        WTSVariant* mr_node = statistical->get("meanReversion");
        if (mr_node) {
            _default_mr_entry_threshold = readDouble(mr_node, "entryZThreshold", 2.0);
            _default_mr_exit_threshold = readDouble(mr_node, "exitZThreshold", 0.5);
            _default_mr_stop_loss_z = readDouble(mr_node, "stopLossZ", 4.0);
            _default_mr_add_safety_ratio = readDouble(mr_node, "addSafetyRatio", 0.75);
        }
        WTSVariant* pt_node = statistical->get("pairsTrading");
        if (pt_node) {
        }
        WTSVariant* tf_node = statistical->get("trendFollowing");
        if (tf_node) {
            _default_tf_stop_loss_pct = readDouble(tf_node, "stopLossPct", 0.02);
            _default_tf_max_trend_bars = readUInt32(tf_node, "maxTrendBars", 50);
        }
    }

    // Read pairs configuration
    WTSVariant* pairs = arb->get("pairs");
    if (pairs && pairs->isArray()) {
        for (uint32_t i = 0; i < pairs->size(); ++i) {
            WTSVariant* pair_cfg = pairs->get(i);
            if (!pair_cfg)
                continue;

            SpreadPairConfig pair;
            pair.pair_id = readString(pair_cfg, "id", "");
            pair.leg1_code = readString(pair_cfg, "leg1", "");
            pair.leg2_code = readString(pair_cfg, "leg2", "");
            pair.leg1_ratio = readDouble(pair_cfg, "ratio", 1.0);
            pair.leg2_ratio = readDouble(pair_cfg, "ratio2", 1.0); // A1: 此前硬编码 1.0, 支持独立 leg2 比率
            // V8-A1: 乘数死亡链修复 — 此前 loadConfig 不读/calculator 无 setter,
            // 恒 1.0。同品种跨期乘数相同可不配 (默认 1.0), 跨品种显式配置。
            pair.leg1_multiplier = readDouble(pair_cfg, "leg1Multiplier", 1.0);
            pair.leg2_multiplier = readDouble(pair_cfg, "leg2Multiplier", 1.0);
            pair.max_spread_position = readDouble(pair_cfg, "maxPosition", 10.0);
            // A9: pair 级未配置时回落到 statistical 段全局默认
            pair.entry_z_threshold = readDouble(pair_cfg, "entryZScore", _default_mr_entry_threshold);
            pair.exit_z_threshold = readDouble(pair_cfg, "exitZScore", _default_mr_exit_threshold);
            pair.stop_loss_z =
                readDouble(pair_cfg, "stopLossZ", _default_mr_stop_loss_z); // A9: 此前无加载, 恒为默认 4.0
            pair.lookback_window = readUInt32(pair_cfg, "windowSize", 200);
            pair.primary_strategy = _config.primary_strategy;
            pair.stop_loss_pct = readDouble(pair_cfg, "stopLossPct", _default_tf_stop_loss_pct);
            pair.max_trend_bars = readUInt32(pair_cfg, "maxTrendBars", _default_tf_max_trend_bars);
            pair.add_safety_ratio = readDouble(pair_cfg, "addSafetyRatio", _default_mr_add_safety_ratio);

            if (!pair.pair_id.empty() && !pair.leg1_code.empty() && !pair.leg2_code.empty()) {
                addSpreadPair(pair);
            }
        }
    }

    // H4: 加载 risk_limits 段 → SpreadRiskConfig (此前未加载, 用编译期默认值)
    WTSVariant* risk_node = arb->get("risk_limits");
    if (risk_node) {
        _risk_config.portfolio_stop_loss = readDouble(risk_node, "portfolioStopLoss", 50000.0);
        _risk_config.max_total_position = readDouble(risk_node, "maxTotalPosition", 50.0);
        _risk_config.max_single_pair = readDouble(risk_node, "maxSinglePair", 20.0);
        _risk_config.max_correlation_break = readDouble(risk_node, "maxCorrelationBreak", 0.3);
        _risk_config.max_divergence_zscore = readDouble(risk_node, "maxDivergenceZscore", 5.0);
        _risk_config.max_divergence_time = readUInt32(risk_node, "maxDivergenceTime", 7200);
        _risk_manager->setConfig(_risk_config);
        WTSLogger::info("SpreadArbitrageManager: risk_limits loaded, portfolioStopLoss={:.0f}, maxTotalPos={:.0f}, "
                        "maxSinglePair={:.0f}",
                        _risk_config.portfolio_stop_loss,
                        _risk_config.max_total_position,
                        _risk_config.max_single_pair);
    }

    WTSLogger::info("SpreadArbitrageManager: Loaded config from {}, enabled={}, pairs={}",
                    config_file,
                    _config.enabled,
                    _strategies.size());

    return true;
}

bool SpreadArbitrageManager::addSpreadPair(const SpreadPairConfig& pair_config)
{
    if (_strategies.size() >= _config.max_pairs)
        return false;

    if (_strategies.find(pair_config.pair_id) != _strategies.end())
        return false;

    // Add to calculator manager
    _calculator_manager->addSpreadPair(pair_config);

    // Initialize strategy
    StrategyInstance instance;
    instance.pair_id = pair_config.pair_id;
    instance.strategy_type = pair_config.primary_strategy;
    initializeStrategy(instance, pair_config);

    _strategies[pair_config.pair_id] = std::move(instance);
    _pair_configs[pair_config.pair_id] = pair_config;

    // Initialize state with contract codes for expiry lookup
    _pair_states[pair_config.pair_id] = SpreadState();
    _pair_states[pair_config.pair_id].pair_id = pair_config.pair_id;
    _pair_states[pair_config.pair_id].leg1_code = pair_config.leg1_code;
    _pair_states[pair_config.pair_id].leg2_code = pair_config.leg2_code;

    // perf#1: 注册 lock-free z-score 缓存槽位
    _pair_zscore_idx[pair_config.pair_id] = _pair_zscore_cache.size();
    _pair_zscore_cache.push_back(std::make_unique<std::atomic<double>>(0.0));

    return true;
}

void SpreadArbitrageManager::removeSpreadPair(const std::string& pair_id)
{
    _calculator_manager->removeSpreadPair(pair_id);
    _strategies.erase(pair_id);
    _pair_configs.erase(pair_id);
    _pair_states.erase(pair_id);
    _last_signal_time.erase(pair_id);
    _last_signals.erase(pair_id);
}

void SpreadArbitrageManager::initializeStrategy(StrategyInstance& instance, const SpreadPairConfig& config)
{
    // 注册表驱动创建: 新增策略只需在文件顶部注册一行, 无需改本函数
    const std::string& name = strategyTypeName(config.primary_strategy);
    auto strat = SpreadStrategyRegistry::instance().create(name);
    if (!strat) {
        // Default to mean reversion
        strat = SpreadStrategyRegistry::instance().create("mean_reversion");
        WTSLogger::warn("SpreadArbitrageManager: unknown strategy '{}', fallback to mean_reversion", name);
    }
    if (strat) {
        strat->configure(config);
        instance.strategies.push_back(std::move(strat));
    }
}

} // namespace futu
