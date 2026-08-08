/*!
 * \file SpreadArbitrageManager.cpp
 * \brief Spread Arbitrage Manager Implementation
 */
#include "SpreadArbitrageManager.h"
#include "FutuPortfolio.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../WTSTools/WTSLogger.h"
#include "SpinLockGuard.h"
#include "../Share/TimeUtils.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>

namespace futu {

//==============================================================================
// 内置策略注册 (新增策略在此追加一行即可, 无需改 Manager 其他代码)
//==============================================================================
namespace {
struct BuiltinStrategyRegistrar {
    BuiltinStrategyRegistrar()
    {
        auto& reg = SpreadStrategyRegistry::instance();
        if (!reg.has("mean_reversion"))
            reg.registerStrategy("mean_reversion", [] { return std::make_unique<MeanReversionStrategy>(); });
        if (!reg.has("trend_following"))
            reg.registerStrategy("trend_following", [] { return std::make_unique<TrendFollowingStrategy>(); });
        if (!reg.has("pairs_trading"))
            reg.registerStrategy("pairs_trading", [] { return std::make_unique<PairsTradingStrategy>(); });
        if (!reg.has("statistical_arb"))
            reg.registerStrategy("statistical_arb", [] { return std::make_unique<StatisticalArbStrategy>(); });
    }
};
// 进程级静态注册 (首次加载本 TU 时执行)
static BuiltinStrategyRegistrar _builtin_registrar;
} // anonymous namespace

SpreadArbitrageManager::SpreadArbitrageManager()
    : _calculator_manager(std::make_unique<SpreadCalculatorManager>())
    , _risk_manager(std::make_unique<SpreadRiskManager>())
    , _mm_enhancer(std::make_unique<MarketMakingEnhancer>())
{
}

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
        if (!v) return defVal;
        WTSVariant* node = v->get(key);
        return node ? node->asBoolean() : defVal;
    };
    auto readDouble = [](WTSVariant* v, const char* key, double defVal) -> double {
        if (!v) return defVal;
        WTSVariant* node = v->get(key);
        return node ? node->asDouble() : defVal;
    };
    auto readUInt32 = [](WTSVariant* v, const char* key, uint32_t defVal) -> uint32_t {
        if (!v) return defVal;
        WTSVariant* node = v->get(key);
        return node ? (uint32_t)node->asInt64() : defVal;
    };
    auto readInt32 = [](WTSVariant* v, const char* key, int defVal) -> int {
        if (!v) return defVal;
        WTSVariant* node = v->get(key);
        return node ? (int)node->asInt64() : defVal;
    };
    auto readString = [](WTSVariant* v, const char* key, const std::string& defVal) -> std::string {
        if (!v) return defVal;
        WTSVariant* node = v->get(key);
        return node ? node->asString() : defVal;
    };

    // Read basic settings
    _config.enabled = readBool(arb, "enabled", false);
    _config.enhance_market_making = readBool(arb, "enhanceMarketMaking", true);
    _config.use_hybrid_strategy = readBool(arb, "useHybridStrategy", false);

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
    _config.mm_enhancement_weight = readDouble(arb, "mmEnhancementWeight", 0.5);

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
            _arb_close_cfg.allow_signals.close_long  = readBool(allow, "close_long", false);
            _arb_close_cfg.allow_signals.close_short = readBool(allow, "close_short", false);
            _arb_close_cfg.allow_signals.timeout_exit = readBool(allow, "timeout_exit", false);
            _arb_close_cfg.allow_signals.stop_loss   = readBool(allow, "stop_loss", false);
        }
        WTSVariant* slp = close_node->get("stop_loss_policy");
        if (slp) {
            _arb_close_cfg.stop_loss_policy.order_flag = readInt32(slp, "order_flag", 1);
            _arb_close_cfg.stop_loss_policy.price_offset_ticks = readDouble(slp, "price_offset_ticks", 0.0);
            _arb_close_cfg.stop_loss_policy.timeout_ms = readUInt32(slp, "timeout_ms", 1000);
            _arb_close_cfg.stop_loss_policy.upgrade_to_taker = readBool(slp, "upgrade_to_taker", false);
        }
        WTSVariant* top = close_node->get("timeout_policy");
        if (top) {
            _arb_close_cfg.timeout_policy.order_flag = readInt32(top, "order_flag", 0);
            _arb_close_cfg.timeout_policy.price_offset_ticks = readDouble(top, "price_offset_ticks", 0.0);
            _arb_close_cfg.timeout_policy.timeout_ms = readUInt32(top, "timeout_ms", 30000);
            _arb_close_cfg.timeout_policy.upgrade_to_taker = readBool(top, "upgrade_to_taker", true);
        }
        _arb_close_cfg.max_close_size_pct = readDouble(close_node, "max_close_size_pct", 0.5);
        _arb_close_cfg.close_in_flight_timeout_ms = readUInt32(close_node, "close_in_flight_timeout_ms", 5000);
        _arb_close_cfg.oversold_protection = readBool(close_node, "oversold_protection", true);
        _arb_close_cfg.overshoot_cooldown_ms = readUInt32(close_node, "overshoot_cooldown_ms", 3600000);
        _arb_close_cfg.intent_broadcast = readBool(close_node, "intent_broadcast", true);

        WTSLogger::info("SpreadArbMgr: arb_close loaded, enabled={}, allow[SL={},TO={},CL={},CS={}]",
            _arb_close_cfg.enabled,
            _arb_close_cfg.allow_signals.stop_loss, _arb_close_cfg.allow_signals.timeout_exit,
            _arb_close_cfg.allow_signals.close_long, _arb_close_cfg.allow_signals.close_short);
    }

    // Read statistical sub-strategy parameters (global defaults, per-pair overrides in pairs section)
    // A9: 键名与 spread_arbitrage.yaml 对齐 (entryZThreshold/stopLossZ/addSafetyRatio/lookbackWindow/stopLossPct/maxTrendBars)
    WTSVariant* statistical = arb->get("statistical");
    if (statistical) {
        WTSVariant* mr_node = statistical->get("meanReversion");
        if (mr_node) {
            _default_mr_half_life = readUInt32(mr_node, "halfLife", 100);
            _default_mr_entry_threshold = readDouble(mr_node, "entryZThreshold", 2.0);
            _default_mr_exit_threshold = readDouble(mr_node, "exitZThreshold", 0.5);
            _default_mr_stop_loss_z = readDouble(mr_node, "stopLossZ", 4.0);
            _default_mr_add_safety_ratio = readDouble(mr_node, "addSafetyRatio", 0.75);
        }
        WTSVariant* pt_node = statistical->get("pairsTrading");
        if (pt_node) {
            _default_pt_correlation_window = readUInt32(pt_node, "correlationWindow", 100);
            _default_pt_min_correlation = readDouble(pt_node, "minCorrelation", 0.7);
            _default_pt_spread_window = readUInt32(pt_node, "lookbackWindow", 50);
            _default_pt_entry_threshold = readDouble(pt_node, "entryZThreshold", 2.0);
        }
        WTSVariant* tf_node = statistical->get("trendFollowing");
        if (tf_node) {
            _default_tf_ma_period = readUInt32(tf_node, "maPeriod", 20);
            _default_tf_breakout_threshold = readDouble(tf_node, "breakoutThreshold", 1.5);
            _default_tf_stop_loss_pct = readDouble(tf_node, "stopLossPct", 0.02);
            _default_tf_max_trend_bars = readUInt32(tf_node, "maxTrendBars", 50);
        }
    }

    // Read pairs configuration
    WTSVariant* pairs = arb->get("pairs");
    if (pairs && pairs->isArray()) {
        for (uint32_t i = 0; i < pairs->size(); ++i) {
            WTSVariant* pair_cfg = pairs->get(i);
            if (!pair_cfg) continue;

            SpreadPairConfig pair;
            pair.pair_id = readString(pair_cfg, "id", "");
            pair.leg1_code = readString(pair_cfg, "leg1", "");
            pair.leg2_code = readString(pair_cfg, "leg2", "");
            pair.leg1_ratio = readDouble(pair_cfg, "ratio", 1.0);
            pair.leg2_ratio = readDouble(pair_cfg, "ratio2", 1.0);  // A1: 此前硬编码 1.0, 支持独立 leg2 比率
            pair.max_spread_position = readDouble(pair_cfg, "maxPosition", 10.0);
            // A9: pair 级未配置时回落到 statistical 段全局默认
            pair.entry_z_threshold = readDouble(pair_cfg, "entryZScore", _default_mr_entry_threshold);
            pair.exit_z_threshold = readDouble(pair_cfg, "exitZScore", _default_mr_exit_threshold);
            pair.stop_loss_z = readDouble(pair_cfg, "stopLossZ", _default_mr_stop_loss_z);  // A9: 此前无加载, 恒为默认 4.0
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
        _risk_config.portfolio_stop_loss     = readDouble(risk_node, "portfolioStopLoss",     50000.0);
        _risk_config.max_total_position      = readDouble(risk_node, "maxTotalPosition",      50.0);
        _risk_config.max_single_pair         = readDouble(risk_node, "maxSinglePair",         20.0);
        _risk_config.max_correlation_break   = readDouble(risk_node, "maxCorrelationBreak",   0.3);
        _risk_config.max_divergence_zscore   = readDouble(risk_node, "maxDivergenceZscore",   5.0);
        _risk_config.max_divergence_time     = readUInt32(risk_node, "maxDivergenceTime",     7200);
        _risk_manager->setConfig(_risk_config);
        WTSLogger::info("SpreadArbitrageManager: risk_limits loaded, portfolioStopLoss={:.0f}, maxTotalPos={:.0f}, maxSinglePair={:.0f}",
            _risk_config.portfolio_stop_loss, _risk_config.max_total_position, _risk_config.max_single_pair);
    }

    WTSLogger::info("SpreadArbitrageManager: Loaded config from {}, enabled={}, pairs={}",
        config_file, _config.enabled, _strategies.size());

    return true;
}

void SpreadArbitrageManager::setCalculatorConfig(const SpreadCalculatorConfig& config)
{
    _calc_config = config;
    _calculator_manager->setConfig(config);
}

void SpreadArbitrageManager::setRiskConfig(const SpreadRiskConfig& config)
{
    _risk_config = config;
    _risk_manager->setConfig(config);
}

void SpreadArbitrageManager::setMmEnhancerConfig(const MmEnhancerConfig& config)
{
    _mm_config = config;
    _mm_enhancer->setConfig(config);
}

void SpreadArbitrageManager::setExpiryDateCallback(ExpiryDateCallback callback)
{
    _risk_manager->setExpiryDateCallback(callback);
}

void SpreadArbitrageManager::setCurrentDate(uint32_t current_date)
{
    _risk_manager->setCurrentDate(current_date);
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

std::vector<std::string> SpreadArbitrageManager::getSpreadPairs() const
{
    std::vector<std::string> pairs;
    pairs.reserve(_strategies.size());
    for (const auto& kv : _strategies)
    {
        pairs.push_back(kv.first);
    }
    return pairs;
}

void SpreadArbitrageManager::initializeStrategy(StrategyInstance& instance,
                                                  const SpreadPairConfig& config)
{
    // 注册表驱动创建: 新增策略只需在文件顶部注册一行, 无需改本函数
    const std::string& name = strategyTypeName(config.primary_strategy);
    auto strat = SpreadStrategyRegistry::instance().create(name);
    if (!strat)
    {
        // Default to mean reversion
        strat = SpreadStrategyRegistry::instance().create("mean_reversion");
        WTSLogger::warn("SpreadArbitrageManager: unknown strategy '{}', fallback to mean_reversion", name);
    }
    if (strat)
    {
        strat->configure(config);
        instance.strategies.push_back(std::move(strat));
    }
}

void SpreadArbitrageManager::onTick(const std::string& code, double price,
                                     double multiplier, uint64_t timestamp)
{
    if (!_config.enabled)
        return;

    // Update calculators
    _calculator_manager->onTick(code, price, multiplier, timestamp);

    // Update strategy data
    const auto& pairs = _calculator_manager->getPairsForContract(code);

    // Acquire exclusive lock for writing
    SpinLockGuard lock(_pair_states_spin);

    for (const auto& pair_id : pairs)
    {
        auto state = _calculator_manager->getSpreadState(pair_id);

        // Update strategy data — 插件循环, 替代原 else-if 链
        auto strat_it = _strategies.find(pair_id);
        if (strat_it != _strategies.end())
        {
            for (auto& strat : strat_it->second.strategies)
            {
                if (strat) strat->update(state, timestamp);
            }
        }

        // Update state (protected by _pair_states_mutex)
        auto& stored_state = _pair_states[pair_id];
        stored_state.current_spread = state.current_spread;
        stored_state.spread_mean = state.spread_mean;
        stored_state.spread_std = state.spread_std;
        stored_state.zscore = state.zscore;
        // perf#1: 同步发布 lock-free 缓存, 主线程读 z-score 不再抢 _pair_states_spin
        {
            auto zit = _pair_zscore_idx.find(pair_id);
            if (zit != _pair_zscore_idx.end())
                _pair_zscore_cache[zit->second]->store(state.zscore, std::memory_order_relaxed);
        }
        stored_state.correlation = state.correlation;
        stored_state.beta = state.beta;
        stored_state.half_life = state.half_life;
        stored_state.leg1_price = state.leg1_price;
        stored_state.leg2_price = state.leg2_price;
        stored_state.is_active = state.is_active;   // BUG FIX: previously missed,
                                                    // strategies always early-returned
                                                    // (is_active=false) → 0 raw signals
                                                    // ever, B-3 gate never exercised.
        stored_state.last_update = timestamp;

        // Update risk manager
        _risk_manager->updatePairState(pair_id, stored_state);
    }
}

void SpreadArbitrageManager::onWtTick(wtp::WTSTickData* tick)
{
    if (!tick)
        return;

    // Extract data from WTSTickData
    std::string code = tick->code();
    double price = tick->price();
    // Look up multiplier from configured contract multipliers
    double multiplier = 1.0;
    auto mult_it = _contract_multipliers.find(code);
    if (mult_it != _contract_multipliers.end())
    {
        multiplier = mult_it->second;
    }
    else
    {
        WTSLogger::warn("SpreadArbManager: no multiplier configured for {}, using default 1.0", code);
    }
    uint64_t timestamp = tick->actiontime();

    onTick(code, price, multiplier, timestamp);
}

std::vector<SpreadSignal> SpreadArbitrageManager::generateSignals(uint64_t current_time)
{
    std::vector<SpreadSignal> signals;

    if (!_config.enabled)
        return signals;

    // F4: 接线 portfolio PnL → SpreadRiskManager drawdown 计算.
    // 此前 updatePortfolioPnL 无调用者 → _current_drawdown 恒 0 → portfolio_stop_loss
    // 阈值永久失效 → STOP_LOSS alert (EMERGENCY 级) 不可达.
    // generateSignals 在 arb 线程执行, 每 arb 周期调用一次 (5ms interval), 频率足够.
    // A2 fix: 使用 atomic 快照避免 arb 线程与主线程的 data race.
    // 主线程通过 publishPnLSnapshot() 每 tick 更新这两个 atomic<double>.
    if (_portfolio_ptr && _risk_manager)
    {
        double unrealized = _portfolio_ptr->getSnapshotUnrealizedPnL();
        double total = _portfolio_ptr->getSnapshotTotalPnL();
        double realized = total - unrealized;
        // updatePortfolioPnL 写 _current_drawdown 等, 主线程 updateAlerts 读这些字段,
        // 必须持 _pair_states_spin 与写端(updatePairState)对齐, 避免 data race.
        SpinLockGuard lock(_pair_states_spin);
        _risk_manager->updatePortfolioPnL(unrealized, realized);
    }

    for (const auto& kv : _strategies)
    {
        const auto& pair_id = kv.first;
        SpreadSignal signal = generateSignal(pair_id, current_time);

        if (signal.isActionable())
        {
            signals.push_back(signal);
            dispatchSignal(signal);
        }
    }

    checkRiskAlerts();
    return signals;
}

SpreadSignal SpreadArbitrageManager::generateSignal(const std::string& pair_id, uint64_t current_time)
{
    SpreadSignal signal;
    signal.pair_id = pair_id;
    signal.timestamp = current_time;

    if (!_config.enabled)
        return signal;

    // P8: 合并 3 次只读 spinlock 获取为 1 次 (原: 读 last_time / 读 state / canOpenPosition
    // 各自加锁). cooldown 早退保持在锁内 — 未过 cooldown 时不做无谓 state 拷贝.
    // 末尾写 last_signal 仍单独加锁 (在耗时策略执行之后, 不延长持锁窗口).
    uint64_t last_time = 0;
    SpreadState state;
    bool can_open;
    {
        SpinLockGuard lock(_pair_states_spin);
        auto time_it = _last_signal_time.find(pair_id);
        if (time_it != _last_signal_time.end())
            last_time = time_it->second;
        if (current_time / 1000ULL - last_time / 1000ULL < _config.signal_cooldown_ms)
            return signal;  // Still in cooldown (guard 析构释放锁)

        auto state_it = _pair_states.find(pair_id);
        if (state_it == _pair_states.end())
            return signal;
        state = state_it->second; // Copy for thread safety

        // _risk_manager 由 updatePairState 在 _pair_states_spin 下写入, 读取也需同一锁.
        can_open = _risk_manager->canOpenPosition(pair_id, 1.0);
    }
    if (!can_open)
    {
        signal.type = SpreadSignalType::NONE;
        return signal;
    }

    // Generate signal from strategy
    auto strat_it = _strategies.find(pair_id);
    if (strat_it == _strategies.end())
        return signal;

    const auto& instance = strat_it->second;

    // 插件循环: 主策略在 front; hybrid 模式取置信度最高的信号
    for (const auto& strat : instance.strategies)
    {
        if (!strat) continue;
        SpreadSignal s = strat->generateSignal(state, current_time);
        if (signal.type == SpreadSignalType::NONE || s.confidence > signal.confidence)
            signal = s;
        if (instance.strategies.size() == 1)
            break;
    }

    // BUG FIX: strategies populate only type/confidence/suggested_size/reason —
    // leg metadata (codes, prices) is left at defaults (empty string, 0). The
    // downstream consumer (AsyncArbitrageExecutor::executeSignal) hashes
    // signal.leg1_code into _tick_sizes / _mm_buy_orders and uses leg*_price
    // for the actual order submission; an empty code or zero price leads to
    // an ArbOrderRequest with invalid fields and OrderRouter::submitSell
    // tries to format a null code → segfault. Populate from SpreadState so
    // the signal is self-contained. Done before B-3 gate which only touches
    // leg*_qty.
    if (signal.type != SpreadSignalType::NONE)
    {
        if (signal.leg1_code.empty()) signal.leg1_code = state.leg1_code;
        if (signal.leg2_code.empty()) signal.leg2_code = state.leg2_code;
        if (signal.leg1_price <= 0)   signal.leg1_price = state.leg1_price;
        if (signal.leg2_price <= 0)   signal.leg2_price = state.leg2_price;
    }

    // Check confidence threshold
    if (signal.confidence < _config.min_signal_confidence)
    {
        signal.type = SpreadSignalType::NONE;
    }

    //==========================================================================
    // Scheme B-3 Gate: portfolio-derived dedup + size adjustment
    //
    // When Portfolio is injected (setPortfolio called), we apply a final gate
    // that:
    //   (a) derives current spread position from Portfolio (SSOT, MM + ARB combined)
    //   (b) computes intent from raw signal (OPEN_LONG_SPREAD → WANT_LONG, etc.)
    //   (c) computes gap = target - derived
    //   (d) suppresses signal if gap is small (already have enough)
    //   (e) clamps suggested_size to max_order_per_signal (= max_spread_position / 4)
    //   (f) blocks if in-flight from previous signal not yet filled
    //   (g) enforces position limit projection
    //   (h) suppresses CLOSE_X_SPREAD signals (let MM consume naturally)
    //
    // If Portfolio not injected, gate is bypassed (legacy behavior).
    //==========================================================================
    if (_portfolio_ptr != nullptr && signal.type != SpreadSignalType::NONE)
    {
        signal = applyB3Gate(pair_id, signal, current_time);
    }

    // Store last signal time/signal under spinlock
    // (arb thread writes here, main thread reads in getQuotingAdjustment)
    if (signal.isActionable())
    {
        SpinLockGuard lock(_pair_states_spin);
        _last_signal_time[pair_id] = current_time;
        _last_signals[pair_id] = signal;
    }

    return signal;
}

QuotingAdjustment SpreadArbitrageManager::getQuotingAdjustment(const std::string& pair_id,
                                                                 uint64_t current_time)
{
    QuotingAdjustment adj;

    if (!_config.enabled || !_config.enhance_market_making)
        return adj;

    // Get state (read lock - arb thread also writes to this map)
    SpreadState state_copy;
    {
        SpinLockGuard lock(_pair_states_spin);
        auto state_it = _pair_states.find(pair_id);
        if (state_it == _pair_states.end())
            return adj;
        state_copy = state_it->second; // Copy for thread safety
    }

    SpreadSignal signal;
    // _last_signals与generateSignals在arb线程写入，需spinlock保护
    // (旧代码先在锁外 find 一次 — 与 arb 线程写并发构成 data race)
    {
        SpinLockGuard lock(_pair_states_spin);
        auto signal_it = _last_signals.find(pair_id);
        if (signal_it != _last_signals.end())
        {
            signal = signal_it->second;
        }
    }

    adj = _mm_enhancer->calculateAdjustment(state_copy, signal, current_time);

    // Dispatch quoting callback
    if (_quoting_callback && adj.confidence > 0)
    {
        _quoting_callback(pair_id, adj);
    }

    return adj;
}

bool SpreadArbitrageManager::shouldPauseQuoting(const std::string& code, bool is_bid) const
{
    // Find all pairs containing this contract
    const auto& pairs = _calculator_manager->getPairsForContract(code);

    // Lock for _pair_states
    SpinLockGuard lock(_pair_states_spin);

    for (const auto& pair_id : pairs)
    {
        auto state_it = _pair_states.find(pair_id);
        if (state_it == _pair_states.end())
            continue;

        const auto& state = state_it->second;
        auto config_it = _pair_configs.find(pair_id);

        if (config_it != _pair_configs.end())
        {
            const auto& config = config_it->second;

            if (config.enhance_quoting)
            {
                // Pause bid if Z-Score very high
                if (is_bid && state.zscore > config.pause_z_threshold)
                    return true;

                // Pause ask if Z-Score very low
                if (!is_bid && state.zscore < -config.pause_z_threshold)
                    return true;
            }
        }
    }

    return false;
}

void SpreadArbitrageManager::refreshPositionsFromPortfolio()
{
    if (!_portfolio_ptr) return;
    for (const auto& kv : _pair_configs)
    {
        const SpreadPairConfig& cfg = kv.second;
        double leg1_pos = _portfolio_ptr->getPosition(cfg.leg1_code);
        double leg2_pos = _portfolio_ptr->getPosition(cfg.leg2_code);
        updatePosition(kv.first, leg1_pos, leg2_pos, 0.0);
    }
}

void SpreadArbitrageManager::updatePosition(const std::string& pair_id,
                                             double leg1_pos, double leg2_pos,
                                             double unrealized_pnl)
{
    // Acquire exclusive lock for writing
    SpinLockGuard lock(_pair_states_spin);

    auto state_it = _pair_states.find(pair_id);
    if (state_it == _pair_states.end())
        return;

    auto& state = state_it->second;

    double prev_position = state.spread_position;

    // 同向腿防护: 双腿同号不构成价差仓(与 computeDerivedSpread 的 signbit 检查一致),
    // 旧代码会把同向双腿算成有效价差仓.
    if (leg1_pos * leg2_pos > 0)
    {
        state.leg1_position = leg1_pos;
        state.leg2_position = leg2_pos;
        state.unrealized_pnl = unrealized_pnl;
        state.spread_position = 0;
        if (prev_position != 0)
        {
            state.position_open_time = 0;
            state.entry_spread = 0;
        }
        _risk_manager->updatePairState(pair_id, state);
        return;
    }

    state.leg1_position = leg1_pos;
    state.leg2_position = leg2_pos;
    state.unrealized_pnl = unrealized_pnl;

    // Calculate spread position using matched pairs approach
    // The correct formula: min(abs(leg1_pos)/ratio1, abs(leg2_pos)/ratio2) * sign(leg1_pos)
    // This counts how many complete spread pairs we have

    auto config_it = _pair_configs.find(pair_id);
    if (config_it != _pair_configs.end())
    {
        const auto& config = config_it->second;

        // Calculate matched pairs
        // For a 1:1 spread with leg1=1 (long), leg2=-1 (short), we have 1 spread
        // For a 2:1 spread with leg1=2 (long), leg2=-1 (short), we have 1 spread

        double leg1_ratio = config.leg1_ratio;
        double leg2_ratio = config.leg2_ratio;

        if (leg1_ratio > 0 && leg2_ratio > 0)
        {
            // Number of "pair units" each leg represents
            double leg1_pairs = std::abs(leg1_pos) / leg1_ratio;
            double leg2_pairs = std::abs(leg2_pos) / leg2_ratio;

            // The number of complete matched pairs is the minimum
            double matched_pairs = std::min(leg1_pairs, leg2_pairs);

            // Direction follows leg1 (positive = long spread, negative = short spread)
            int sign = (leg1_pos >= 0) ? 1 : -1;

            state.spread_position = matched_pairs * sign;
        }
        else
        {
            // Fallback to simple difference if ratios are invalid
            state.spread_position = leg1_pos - leg2_pos;
        }
    }
    else
    {
        // Default 1:1 spread
        double leg1_pairs = std::abs(leg1_pos);
        double leg2_pairs = std::abs(leg2_pos);
        double matched_pairs = std::min(leg1_pairs, leg2_pairs);
        int sign = (leg1_pos >= 0) ? 1 : -1;
        state.spread_position = matched_pairs * sign;
    }

    // Track position open time
    // 时间基准: 与策略 generateSignal 的 current_time 一致 (µs epoch)。
    // 旧代码用 state.last_update(tick->actiontime() 打包整数 HHMMSSmmm),
    // 导致 positionDuration 算出天文数字, 超时退出逻辑失效.
    if (prev_position == 0 && state.spread_position != 0)
    {
        state.position_open_time = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()).count());
        state.entry_spread = state.current_spread;
    }
    else if (state.spread_position == 0)
    {
        state.position_open_time = 0;
        state.entry_spread = 0;
    }

    // Update risk manager
    _risk_manager->updatePairState(pair_id, state);
}

//==============================================================================
// Scheme B-3: Portfolio-Derived Helpers & Gate
//==============================================================================

double SpreadArbitrageManager::computeDerivedSpread(const SpreadPairConfig& cfg) const
{
    // 线程安全: 本函数在 arb 线程执行(applyB3Gate → generateSignals),
    // 直读 FutuPortfolio(主线程在 on_trade 中写)是 data race。
    // 改读 _pair_states 的腿持仓 — 由主线程 refreshPositionsFromPortfolio
    // 每 tick 经 updatePosition 刷新, 统一走 _pair_states_spin 保护.
    double leg1_pos = 0.0, leg2_pos = 0.0;
    {
        SpinLockGuard lock(_pair_states_spin);
        auto it = _pair_states.find(cfg.pair_id);
        if (it == _pair_states.end())
            return 0.0;
        leg1_pos = it->second.leg1_position;
        leg2_pos = it->second.leg2_position;
    }

    // Either leg empty → no spread
    if (std::abs(leg1_pos) < 1e-6 || std::abs(leg2_pos) < 1e-6)
        return 0.0;

    double r1 = (cfg.leg1_ratio > 0) ? cfg.leg1_ratio : 1.0;
    double r2 = (cfg.leg2_ratio > 0) ? cfg.leg2_ratio : 1.0;

    // Legs co-directional (both long or both short) → invalid spread config,
    // not a valid pair (likely transient state during MM consumption).
    // Returning 0 is conservative: arb will treat as flat and refill via target.
    if (std::signbit(leg1_pos) == std::signbit(leg2_pos))
        return 0.0;

    double leg1_pairs = std::abs(leg1_pos) / r1;
    double leg2_pairs = std::abs(leg2_pos) / r2;
    double matched = std::min(leg1_pairs, leg2_pairs);

    // Sign: +1 = long spread (leg1 long, leg2 short)
    int sign = (leg1_pos > 0) ? +1 : -1;

    // Minimum unit threshold (defense against float residue)
    double min_unit = std::min(r1, r2) * 0.5;
    if (matched < min_unit) return 0.0;

    return matched * sign;
}

ArbIntent SpreadArbitrageManager::computeIntent(double z,
                                                  const SpreadPairConfig& cfg,
                                                  ArbIntent prev) const
{
    // Mean-reversion semantics:
    //   z > +entry_z  → spread too high, want SHORT (sell spread, wait for revert)
    //   z < -entry_z  → spread too low, want LONG
    //   |z| < exit_z  → revert achieved, WANT_FLAT (do NOT actively close)
    //   exit < |z| < entry → hysteresis, keep prev intent
    double entry = cfg.entry_z_threshold;
    double exit_z = cfg.exit_z_threshold;

    if (z > entry)      return ArbIntent::WANT_SHORT;
    if (z < -entry)     return ArbIntent::WANT_LONG;
    if (std::abs(z) < exit_z) return ArbIntent::WANT_FLAT;
    return prev;
}

SpreadSignal SpreadArbitrageManager::applyB3Gate(const std::string& pair_id,
                                                  const SpreadSignal& raw,
                                                  uint64_t current_time_us)
{
    SpreadSignal result = raw;

    // AsyncArbitrageExecutor passes current_time as microseconds-since-epoch.
    // Convert to milliseconds for all in-flight timeout comparisons.
    const uint64_t current_time = current_time_us / 1000;

    // Look up config
    auto cfg_it = _pair_configs.find(pair_id);
    if (cfg_it == _pair_configs.end())
    {
        result.type = SpreadSignalType::NONE;
        return result;
    }
    const SpreadPairConfig& cfg = cfg_it->second;

    // -----------------------------------------------------------------
    // 平仓类信号门控 (C0 分级配置; 默认 enabled=false = 纯 B-3 全抑制)
    //   CLOSE_LONG/SHORT: v2.0 永不解禁 (B-3 特性, MM maker 消耗)
    //   STOP_LOSS:  C1 解禁 (FAK + 对手价)
    //   TIMEOUT:    C2 解禁 (GFD mid, 超时升级)
    //   REBALANCE:  保持抑制
    // -----------------------------------------------------------------
    if (is_close_signal(raw.type))
    {
        if (!_arb_close_cfg.is_allowed(raw.type))
        {
            result.type = SpreadSignalType::NONE;
            return result;  // 原 B-3 行为
        }

        // 通过门控: STOP_LOSS / TIMEOUT (或配置解禁的 CLOSE)
        SpinLockGuard arb_lock(_pair_arb_spin);
        auto& arb_state = _pair_arb_states[pair_id];

        // B5: 过冲冷却期抑制一切信号
        if (isInOvershootCooldown(pair_id))
        {
            result.type = SpreadSignalType::NONE;
            return result;
        }

        // B4: 平仓 in_flight 超时强制清理 (止损单卡死兜底, 远短于开仓 60s)
        if (arb_state.close_in_flight_qty > 0.5 &&
            arb_state.close_in_flight_set_time != 0 &&
            (current_time - arb_state.close_in_flight_set_time) >= _arb_close_cfg.close_in_flight_timeout_ms)
        {
            WTSLogger::warn("SpreadArbMgr[{}] close_in_flight timeout: clearing {} elapsed_ms={}",
                pair_id, arb_state.close_in_flight_qty,
                current_time - arb_state.close_in_flight_set_time);
            arb_state.close_in_flight_qty = 0;
            arb_state.close_in_flight_set_time = 0;
            clearActiveCloseIntent(pair_id);
            _timed_out_pairs.push_back(pair_id);  // 复用撤单清理通道
        }

        // B4: 平仓防双发 (有在飞平仓单则抑制新平仓信号)
        if (arb_state.close_in_flight_qty > 0.5)
        {
            result.type = SpreadSignalType::NONE;
            return result;
        }

        // B3 第一层 (arb 线程粗判): 零持仓降级 + clamp 到实际持仓 × max_close_size_pct
        double derived = computeDerivedSpread(cfg);
        arb_state.last_derived_position = derived;
        if (std::abs(derived) < 0.5)
        {
            result.type = SpreadSignalType::NONE;  // 已被 MM 消耗完, 无需主动平仓
            return result;
        }
        double close_qty = std::min(std::abs(raw.suggested_size),
                                    std::abs(derived) * _arb_close_cfg.max_close_size_pct);
        if (close_qty < 0.5)
        {
            result.type = SpreadSignalType::NONE;
            return result;
        }

        // 方向一致性校验: derived > 0 (多 spread) 的平仓方向 = CLOSE_LONG / STOP_LOSS / TIMEOUT
        // 信号类型与持仓符号由 executeSignal 二次推导 (A5), 此处仅标记 direction 供 intent
        int close_direction = (derived > 0) ? +1 : -1;  // +1 = 平多 (卖leg1买leg2)

        result.suggested_size = close_qty;
        result.leg1_qty = close_qty * cfg.leg1_ratio;
        result.leg2_qty = close_qty * cfg.leg2_ratio;

        // B4: 标记平仓 in_flight (A1 修复后 ratio 正确)
        arb_state.close_in_flight_qty = close_qty * (cfg.leg1_ratio + cfg.leg2_ratio);
        arb_state.close_in_flight_set_time = current_time;

        // B1: 广播平仓 intent (Coordinator 抑制 MM 同侧报价)
        if (_arb_close_cfg.intent_broadcast)
        {
            setActiveCloseIntent(pair_id, close_direction, close_qty, current_time);
        }

        WTSLogger::info("SpreadArbMgr[{}] CLOSE-GATE pass: type={} derived={:.2f} qty={:.2f} dir={}",
            pair_id, (int)raw.type, derived, close_qty, close_direction);
        return result;
    }

    // -----------------------------------------------------------------
    // Derive intent from raw signal type (not from z, since not all
    // strategies use z — trend following uses MA, stat arb uses ML).
    // -----------------------------------------------------------------
    ArbIntent intent;
    int target_sign;
    if (raw.type == SpreadSignalType::OPEN_LONG_SPREAD)
    {
        intent = ArbIntent::WANT_LONG;
        target_sign = +1;
    }
    else if (raw.type == SpreadSignalType::OPEN_SHORT_SPREAD)
    {
        intent = ArbIntent::WANT_SHORT;
        target_sign = -1;
    }
    else
    {
        // Non-action signal types (NONE / PAUSE_QUOTING / RESUME_QUOTING):
        // do not gate, return as-is
        return result;
    }

    // -----------------------------------------------------------------
    // Update per-pair arb state under lock
    // -----------------------------------------------------------------
    SpinLockGuard arb_lock(_pair_arb_spin);

    auto& arb_state = _pair_arb_states[pair_id];

    // Intent transition tracking
    if (arb_state.intent != intent)
    {
        arb_state.intent = intent;
        arb_state.intent_set_tick = current_time;
    }

    // -----------------------------------------------------------------
    // Compute derived spread position (signed)
    // -----------------------------------------------------------------
    double derived = computeDerivedSpread(cfg);
    arb_state.last_derived_position = derived;

    // -----------------------------------------------------------------
    // In-flight check (block double-fire)
    // Reset in_flight if timeout elapsed (defense against stuck orders).
    // -----------------------------------------------------------------
    if (arb_state.in_flight_qty > 0.5 &&
        arb_state.in_flight_set_time != 0 &&
        (current_time - arb_state.in_flight_set_time) >= _in_flight_timeout_ms)
    {
        WTSLogger::warn("SpreadArbMgr[{}] in_flight timeout: clearing {} elapsed_ms={}",
            pair_id, arb_state.in_flight_qty,
            current_time - arb_state.in_flight_set_time);
        arb_state.in_flight_qty = 0;
        arb_state.in_flight_direction = 0;
        arb_state.in_flight_set_time = 0;
        _timed_out_pairs.push_back(pair_id);
    }

    if (arb_state.in_flight_qty > 0.5)
    {
        // Previous order still in-flight; suppress new signal
        result.type = SpreadSignalType::NONE;
        return result;
    }

    // -----------------------------------------------------------------
    // Compute gap (target = sign * max_spread_position, derived is signed)
    // -----------------------------------------------------------------
    double max_pos = cfg.max_spread_position;
    if (max_pos <= 0) max_pos = 5.0;  // Defensive default
    double target = target_sign * max_pos;
    double gap = target - derived;

    // -----------------------------------------------------------------
    // Min-order-size threshold: if gap is small, derived is "close enough"
    // — let MM continue consuming, do not refill.
    // Threshold = 1 lot (minimum tradable unit for futures).
    // -----------------------------------------------------------------
    double min_order_size = 1.0;
    if (std::abs(gap) < min_order_size)
    {
        result.type = SpreadSignalType::NONE;
        return result;
    }

    // -----------------------------------------------------------------
    // Clamp suggested_size to max_order_per_signal (ramp protection).
    // Default to max_spread_position / 4 (≤ 25% of full position per signal).
    // -----------------------------------------------------------------
    double max_order_per_signal = std::max(1.0, max_pos * 0.25);
    double order_qty = std::min(std::abs(gap), max_order_per_signal);

    // -----------------------------------------------------------------
    // Position projection cap: ensure |derived + signed_order| <= max_pos * 1.05
    // (5% headroom for in-flight slack).
    // -----------------------------------------------------------------
    int order_dir = (gap > 0) ? +1 : -1;
    double projected = derived + order_dir * order_qty;
    double abs_limit = max_pos * 1.05;
    if (std::abs(projected) > abs_limit)
    {
        // Shrink order_qty to stay within limit
        double room = abs_limit - std::abs(derived);
        if (room < min_order_size)
        {
            result.type = SpreadSignalType::NONE;
            return result;
        }
        order_qty = std::min(order_qty, room);
    }

    // -----------------------------------------------------------------
    // Apply adjusted size to signal; mark in-flight
    // -----------------------------------------------------------------
    result.suggested_size = order_qty;
    result.leg1_qty = order_qty * cfg.leg1_ratio;
    result.leg2_qty = order_qty * cfg.leg2_ratio;

    arb_state.in_flight_qty = order_qty * (cfg.leg1_ratio + cfg.leg2_ratio);  // A1: 此前硬编码 *2.0, 与 ratio 不匹配时 in_flight 永不清零/过早清零
    arb_state.in_flight_direction = order_dir;
    arb_state.in_flight_set_time = current_time;

    WTSLogger::debug("SpreadArbMgr[{}] B3-gate: intent={} derived={:.2f} target={:.2f} "
                     "gap={:.2f} order_qty={:.2f} dir={}",
        pair_id, (int)intent, derived, target, gap, order_qty, order_dir);

    return result;
}

void SpreadArbitrageManager::onArbSignalDropped(const std::string& pair_id)
{
    {
        SpinLockGuard lock(_pair_arb_spin);
        auto it = _pair_arb_states.find(pair_id);
        if (it != _pair_arb_states.end())
        {
            it->second.in_flight_qty = 0;
            it->second.close_in_flight_qty = 0;       // B4
            it->second.close_in_flight_set_time = 0;
        }
    }
    clearActiveCloseIntent(pair_id);  // B1
}

void SpreadArbitrageManager::onArbOrderFilled(const std::string& pair_id, double filled_qty)
{
    if (filled_qty <= 0) return;

    bool close_done = false;
    {
        SpinLockGuard lock(_pair_arb_spin);
        auto it = _pair_arb_states.find(pair_id);
        if (it == _pair_arb_states.end()) return;

        auto& arb_state = it->second;

        // B4: 平仓成交优先扣 close_in_flight, 再扣 open in_flight
        double remaining = filled_qty;
        if (arb_state.close_in_flight_qty > 0 && remaining > 0)
        {
            double ded = std::min(arb_state.close_in_flight_qty, remaining);
            arb_state.close_in_flight_qty -= ded;
            remaining -= ded;
            if (arb_state.close_in_flight_qty < 0.5)
            {
                arb_state.close_in_flight_qty = 0;
                arb_state.close_in_flight_set_time = 0;
                close_done = true;
            }
        }
        if (remaining > 0)
        {
            arb_state.in_flight_qty = std::max(0.0, arb_state.in_flight_qty - remaining);
            if (arb_state.in_flight_qty < 0.5)
            {
                arb_state.in_flight_qty = 0;
                arb_state.in_flight_direction = 0;
                arb_state.in_flight_set_time = 0;
                WTSLogger::debug("SpreadArbMgr[{}] in-flight fully cleared after fill {:.2f}",
                    pair_id, filled_qty);
            }
        }
    }

    // B1: 平仓单全部成交 → 清理 intent (锁外执行, 避免锁序嵌套)
    if (close_done)
        clearActiveCloseIntent(pair_id);
}

SpreadState SpreadArbitrageManager::getSpreadState(const std::string& pair_id) const
{
    // Lock for _pair_states
    SpinLockGuard lock(_pair_states_spin);
    auto it = _pair_states.find(pair_id);
    if (it != _pair_states.end())
        return it->second;
    return SpreadState();
}

std::vector<SpreadState> SpreadArbitrageManager::getAllStates() const
{
    // Lock for _pair_states
    SpinLockGuard lock(_pair_states_spin);
    std::vector<SpreadState> states;
    states.reserve(_pair_states.size());
    for (const auto& kv : _pair_states)
    {
        states.push_back(kv.second);
    }
    return states;
}

PortfolioRiskSummary SpreadArbitrageManager::getRiskSummary() const
{
    return _risk_manager->calculatePortfolioRisk();
}

bool SpreadArbitrageManager::canOpenPosition(const std::string& pair_id, double size) const
{
    return _risk_manager->canOpenPosition(pair_id, size);
}

std::vector<RiskAlert> SpreadArbitrageManager::getActiveAlerts() const
{
    return _risk_manager->getActiveAlerts();
}

void SpreadArbitrageManager::dispatchSignal(const SpreadSignal& signal)
{
    if (_signal_callback)
    {
        _signal_callback(signal);
    }
}

void SpreadArbitrageManager::checkRiskAlerts()
{
    // _active_alerts 由 updatePairState(主线程, 持 _pair_states_spin) -> updateAlerts 写入,
    // 此处在 arb 线程读取, 必须持同一锁, 否则 vector copy ctor 与 clear/push_back 并发 -> SIGSEGV.
    SpinLockGuard lock(_pair_states_spin);
    auto alerts = _risk_manager->generateAlerts();
    for (const auto& alert : alerts)
    {
        if (_alert_callback)
        {
            _alert_callback(alert);
        }
    }
}

SpreadSignal SpreadArbitrageManager::combineSignals(
    const std::vector<SpreadSignal>& signals,
    const SpreadState& state,
    uint64_t current_time)
{
    if (signals.empty())
    {
        SpreadSignal none;
        none.type = SpreadSignalType::NONE;
        none.pair_id = state.pair_id;
        none.timestamp = current_time;
        return none;
    }

    if (signals.size() == 1)
        return signals[0];

    double long_weight = 0, short_weight = 0;
    double long_conf = 0, short_conf = 0;
    double long_size = 0, short_size = 0;

    for (const auto& sig : signals)
    {
        if (sig.type == SpreadSignalType::NONE) continue;

        bool is_long = (sig.type == SpreadSignalType::OPEN_LONG_SPREAD ||
                        sig.type == SpreadSignalType::CLOSE_SHORT_SPREAD);

        if (is_long)
        {
            long_weight += sig.confidence;
            long_conf = std::max(long_conf, sig.confidence);
            long_size += sig.suggested_size;
        }
        else
        {
            short_weight += sig.confidence;
            short_conf = std::max(short_conf, sig.confidence);
            short_size += sig.suggested_size;
        }
    }

    SpreadSignal result;
    result.pair_id = state.pair_id;
    result.timestamp = current_time;

    double total_weight = long_weight + short_weight;
    if (total_weight <= 0)
    {
        result.type = SpreadSignalType::NONE;
        return result;
    }

    if (long_weight > short_weight && long_conf > 0.5)
    {
        result.type = SpreadSignalType::OPEN_LONG_SPREAD;
        result.confidence = long_weight / total_weight;
        result.suggested_size = long_size / static_cast<double>(signals.size());
        result.reason = "Combined signal: long consensus";
    }
    else if (short_weight > long_weight && short_conf > 0.5)
    {
        result.type = SpreadSignalType::OPEN_SHORT_SPREAD;
        result.confidence = short_weight / total_weight;
        result.suggested_size = short_size / static_cast<double>(signals.size());
        result.reason = "Combined signal: short consensus";
    }
    else
    {
        result.type = SpreadSignalType::NONE;
        result.reason = "Conflicting signals, no consensus";
    }

    return result;
}

void SpreadArbitrageManager::reset()
{
    _calculator_manager->reset();
    _risk_manager->reset();
    _mm_enhancer->reset();

    for (auto& kv : _strategies)
    {
        for (auto& strat : kv.second.strategies)
        {
            if (strat) strat->reset();
        }
    }

    _last_signal_time.clear();
    _last_signals.clear();

    // B1/B5: 清理平仓 intent 与过冲冷却 (session 级复位)
    {
        while (_intent_spin.test_and_set(std::memory_order_acquire)) {}
        _active_close_intents.clear();
        _overshoot_cooldowns.clear();
        _overshoot_pairs.clear();
        _intent_spin.clear(std::memory_order_release);
    }

    // B7 fix: 同步清理 B-3 在途状态, 否则 stale in_flight_qty>0.5
    // 阻断所有新信号直到 60s 超时 (每 session 边界后 arb 冻结)
    {
        SpinLockGuard lock(_pair_arb_spin);
        _pair_arb_states.clear();
    }

    // perf#1: z-score 缓存同步清零, 防跨 session 残留旧值
    for (auto& slot : _pair_zscore_cache)
        slot->store(0.0, std::memory_order_relaxed);
}

//==============================================================================
// B1: ArbIntent 实时通道
//==============================================================================

void SpreadArbitrageManager::setActiveCloseIntent(const std::string& pair_id,
                                                   int direction, double qty, uint64_t now_ms)
{
    CloseIntent intent;
    intent.pair_id = pair_id;
    intent.direction = direction;
    intent.qty = qty;
    intent.set_time = now_ms;

    while (_intent_spin.test_and_set(std::memory_order_acquire)) {}
    _active_close_intents[pair_id] = intent;
    _intent_spin.clear(std::memory_order_release);
}

void SpreadArbitrageManager::clearActiveCloseIntent(const std::string& pair_id)
{
    while (_intent_spin.test_and_set(std::memory_order_acquire)) {}
    _active_close_intents.erase(pair_id);
    _intent_spin.clear(std::memory_order_release);
}

bool SpreadArbitrageManager::hasActiveCloseIntent(const std::string& leg_code) const
{
    return getArbCloseDirection(leg_code) != 0;
}

int SpreadArbitrageManager::getArbCloseDirection(const std::string& leg_code) const
{
    // 1:N 映射: 该合约所属全部 pair, 任一活跃 intent 即返回其方向 (any-match)
    if (!_calculator_manager) return 0;
    const auto& pairs = _calculator_manager->getPairsForContract(leg_code);
    if (pairs.empty()) return 0;

    while (_intent_spin.test_and_set(std::memory_order_acquire)) {}
    int result = 0;
    for (const auto& pid : pairs)
    {
        auto it = _active_close_intents.find(pid);
        if (it == _active_close_intents.end()) continue;
        auto cfg_it = _pair_configs.find(pid);
        if (cfg_it == _pair_configs.end()) continue;
        // 平多 (dir=+1): 卖 leg1 买 leg2; 平空 (dir=-1): 买 leg1 卖 leg2
        bool is_leg1 = (cfg_it->second.leg1_code == leg_code);
        // leg 方向: dir=+1 → leg1=-1(卖), leg2=+1(买); dir=-1 → 反
        int leg_dir = (it->second.direction > 0)
            ? (is_leg1 ? -1 : +1)
            : (is_leg1 ? +1 : -1);
        result = leg_dir;
        break;  // any-match: 取首个活跃 intent
    }
    _intent_spin.clear(std::memory_order_release);
    return result;
}

//==============================================================================
// B5: 过冲保险丝 (事后兜底)
//==============================================================================

void SpreadArbitrageManager::onOvershootDetected(const std::string& leg_code)
{
    if (!_arb_close_cfg.oversold_protection) return;
    if (!_calculator_manager) return;

    const auto& pairs = _calculator_manager->getPairsForContract(leg_code);
    uint64_t now_ms = _now_ms.load(std::memory_order_relaxed); if (now_ms == 0) now_ms = TimeUtils::getLocalTimeNow();

    for (const auto& pid : pairs)
    {
        // B5 连坐过滤: 只冷却有活跃平仓 intent 的 pair.
        // Portfolio.checkOvershootSignFlip 已在合约层做 any-match 过滤 (任一 pair 有 intent 才报),
        // 但合约属多 pair 时, 其它 pair 可能无 intent (纯 MM flip 或被波及) — 这些不应连坐.
        // 精准冷却避免误停无参与 pair 的正常交易 (1h 冷却对高频是重大影响).
        bool has_intent;
        {
            while (_intent_spin.test_and_set(std::memory_order_acquire)) {}
            has_intent = (_active_close_intents.find(pid) != _active_close_intents.end());
            if (has_intent)
            {
                _overshoot_cooldowns[pid] = now_ms + _arb_close_cfg.overshoot_cooldown_ms;
                _overshoot_pairs.push_back(pid);
            }
            _intent_spin.clear(std::memory_order_release);
        }

        if (!has_intent)
        {
            WTSLogger::info("SpreadArbMgr[{}] OVERSHOOT on leg {}: pair has no close intent, skip cooldown (no collateral)",
                pid, leg_code);
            continue;
        }

        WTSLogger::error("SpreadArbMgr[{}] OVERSHOOT detected on leg {}: pair enters cooldown {}ms, canceling orders",
            pid, leg_code, _arb_close_cfg.overshoot_cooldown_ms);

        clearActiveCloseIntent(pid);

        // 告警外发 (复用 RiskAlert 回调通道, 由策略层转发 EventNotifier)
        if (_alert_callback)
        {
            RiskAlert alert;
            alert.type = RiskAlert::Type::STOP_LOSS;
            alert.level = RiskAlert::Level::CRITICAL;
            alert.pair_id = pid;
            alert.message = "OVERSHOOT: sign-flip during arb close on " + leg_code;
            alert.value = 0;
            alert.threshold = 0;
            alert.timestamp = now_ms;
            _alert_callback(alert);
        }
    }
}

bool SpreadArbitrageManager::isLegInActivePair(const std::string& code) const
{
    if (!_calculator_manager) return false;
    return _calculator_manager->isSpreadContract(code);
}

bool SpreadArbitrageManager::isInOvershootCooldown(const std::string& pair_id) const
{
    while (_intent_spin.test_and_set(std::memory_order_acquire)) {}
    auto it = _overshoot_cooldowns.find(pair_id);
    bool in_cd = (it != _overshoot_cooldowns.end()) &&
                 ((_now_ms.load(std::memory_order_relaxed) > 0 ? _now_ms.load(std::memory_order_relaxed) : TimeUtils::getLocalTimeNow()) < it->second);
    _intent_spin.clear(std::memory_order_release);
    return in_cd;
}

bool SpreadArbitrageManager::popOvershootPairs(std::vector<std::string>& out_pairs)
{
    while (_intent_spin.test_and_set(std::memory_order_acquire)) {}
    if (_overshoot_pairs.empty())
    {
        _intent_spin.clear(std::memory_order_release);
        return false;
    }
    out_pairs = std::move(_overshoot_pairs);
    _overshoot_pairs.clear();
    _intent_spin.clear(std::memory_order_release);
    return true;
}

//==============================================================================
// B6: 一合约多 pair 聚合查询
//==============================================================================

double SpreadArbitrageManager::getPairZscore(const std::string& pair_id) const
{
    // perf#1: lock-free 读 atomic 缓存, 消除主线程每 tick K 次 spinlock 竞争.
    // 注册后 _pair_zscore_idx 只读 (addSpreadPair 仅 init/config 期调用),
    // ankerl map 无写并发时读安全.
    auto it = _pair_zscore_idx.find(pair_id);
    return (it != _pair_zscore_idx.end())
        ? _pair_zscore_cache[it->second]->load(std::memory_order_relaxed)
        : 0.0;
}

double SpreadArbitrageManager::getAggregateZscore(const std::string& leg_code) const
{
    if (!_calculator_manager) return 0.0;
    const auto& pairs = _calculator_manager->getPairsForContract(leg_code);
    double max_abs_z = 0.0;
    for (const auto& pid : pairs)
    {
        double z = getPairZscore(pid);
        if (std::abs(z) > std::abs(max_abs_z)) max_abs_z = z;
    }
    return max_abs_z;
}

QuotingAdjustment SpreadArbitrageManager::getQuotingAdjustmentForLeg(const std::string& leg_code,
                                                                       uint64_t now_ms)
{
    if (!_calculator_manager) return QuotingAdjustment();
    const auto& pairs = _calculator_manager->getPairsForContract(leg_code);
    if (pairs.empty()) return QuotingAdjustment();
    return getQuotingAdjustment(pairs.front(), now_ms);
}

} // namespace futu
