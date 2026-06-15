/*!
 * \file SpreadArbitrageManager.cpp
 * \brief Spread Arbitrage Manager Implementation
 */
#include "SpreadArbitrageManager.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../WTSTools/WTSLogger.h"
#include "SpinLockGuard.h"
#include <algorithm>
#include <cmath>

namespace futu {

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
    
    // Read statistical sub-strategy parameters (global defaults, per-pair overrides in pairs section)
    WTSVariant* statistical = arb->get("statistical");
    if (statistical) {
        WTSVariant* mr_node = statistical->get("meanReversion");
        if (mr_node) {
            _default_mr_half_life = readUInt32(mr_node, "halfLife", 100);
            _default_mr_entry_threshold = readDouble(mr_node, "entryThreshold", 2.0);
            _default_mr_exit_threshold = readDouble(mr_node, "exitThreshold", 0.5);
        }
        WTSVariant* pt_node = statistical->get("pairsTrading");
        if (pt_node) {
            _default_pt_correlation_window = readUInt32(pt_node, "correlationWindow", 100);
            _default_pt_min_correlation = readDouble(pt_node, "minCorrelation", 0.7);
            _default_pt_spread_window = readUInt32(pt_node, "spreadWindow", 50);
        }
        WTSVariant* tf_node = statistical->get("trendFollowing");
        if (tf_node) {
            _default_tf_ma_period = readUInt32(tf_node, "maPeriod", 20);
            _default_tf_breakout_threshold = readDouble(tf_node, "breakoutThreshold", 1.5);
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
            pair.leg2_ratio = 1.0;
            pair.max_spread_position = readDouble(pair_cfg, "maxPosition", 10.0);
            pair.entry_z_threshold = readDouble(pair_cfg, "entryZScore", 2.0);
            pair.exit_z_threshold = readDouble(pair_cfg, "exitZScore", 0.5);
            pair.lookback_window = readUInt32(pair_cfg, "windowSize", 200);
            pair.primary_strategy = _config.primary_strategy;
            pair.stop_loss_pct = readDouble(pair_cfg, "stopLossPct", 0.02);
            pair.max_trend_bars = readUInt32(pair_cfg, "maxTrendBars", 50);
            pair.add_safety_ratio = readDouble(pair_cfg, "addSafetyRatio", 0.75);
            
            if (!pair.pair_id.empty() && !pair.leg1_code.empty() && !pair.leg2_code.empty()) {
                addSpreadPair(pair);
            }
        }
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
    switch (config.primary_strategy)
    {
    case ArbitrageStrategy::MEAN_REVERSION:
        instance.mean_reversion = std::make_unique<MeanReversionStrategy>();
        {
            MeanReversionConfig mr_config;
            mr_config.entry_z_threshold = config.entry_z_threshold;
            mr_config.exit_z_threshold = config.exit_z_threshold;
            mr_config.stop_loss_z = config.stop_loss_z;
            mr_config.max_position = config.max_spread_position;
            mr_config.convergence_timeout = config.convergence_timeout;
            mr_config.add_safety_ratio = config.add_safety_ratio;
            instance.mean_reversion->setConfig(mr_config);
        }
        break;
        
    case ArbitrageStrategy::TREND_FOLLOWING:
        instance.trend_following = std::make_unique<TrendFollowingStrategy>();
        {
            TrendFollowingConfig tf_config;
            tf_config.fast_ma_period = config.trend_ma_fast;
            tf_config.slow_ma_period = config.trend_ma_slow;
            tf_config.max_position = config.max_spread_position;
            tf_config.stop_loss_pct = config.stop_loss_pct;
            tf_config.max_trend_bars = config.max_trend_bars;
            instance.trend_following->setConfig(tf_config);
        }
        break;
        
    case ArbitrageStrategy::PAIRS_TRADING:
        instance.pairs_trading = std::make_unique<PairsTradingStrategy>();
        {
            PairsTradingConfig pt_config;
            pt_config.entry_z_threshold = config.entry_z_threshold;
            pt_config.exit_z_threshold = config.exit_z_threshold;
            pt_config.stop_loss_z = config.stop_loss_z;
            pt_config.max_position = config.max_spread_position;
            pt_config.lookback_window = config.lookback_window;
            instance.pairs_trading->setConfig(pt_config);
        }
        break;
        
    case ArbitrageStrategy::STATISTICAL_ARB:
        instance.statistical_arb = std::make_unique<StatisticalArbStrategy>();
        {
            StatisticalArbConfig sa_config;
            sa_config.max_position = config.max_spread_position;
            instance.statistical_arb->setConfig(sa_config);
        }
        break;
        
    default:
        // Default to mean reversion
        instance.mean_reversion = std::make_unique<MeanReversionStrategy>();
        break;
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
    auto pairs = _calculator_manager->getPairsForContract(code);
    
    // Acquire exclusive lock for writing
    SpinLockGuard lock(_pair_states_spin);
    
    for (const auto& pair_id : pairs)
    {
        auto state = _calculator_manager->getSpreadState(pair_id);
        
        // Update trend strategy if active
        auto strat_it = _strategies.find(pair_id);
        if (strat_it != _strategies.end())
        {
            if (strat_it->second.trend_following)
            {
                strat_it->second.trend_following->updateSpread(state.current_spread, timestamp);
            }
            if (strat_it->second.pairs_trading)
            {
                strat_it->second.pairs_trading->updatePrices(
                    state.leg1_price, state.leg2_price, timestamp);
            }
            if (strat_it->second.statistical_arb)
            {
                strat_it->second.statistical_arb->updateState(state, timestamp);
            }
        }
        
        // Update state (protected by _pair_states_mutex)
        auto& stored_state = _pair_states[pair_id];
        stored_state.current_spread = state.current_spread;
        stored_state.spread_mean = state.spread_mean;
        stored_state.spread_std = state.spread_std;
        stored_state.zscore = state.zscore;
        stored_state.correlation = state.correlation;
        stored_state.beta = state.beta;
        stored_state.half_life = state.half_life;
        stored_state.leg1_price = state.leg1_price;
        stored_state.leg2_price = state.leg2_price;
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
    
    // Check cooldown under spinlock (_last_signal_time written by arb thread)
    uint64_t last_time = 0;
    {
        SpinLockGuard lock(_pair_states_spin);
        auto time_it = _last_signal_time.find(pair_id);
        if (time_it != _last_signal_time.end())
            last_time = time_it->second;
    }
    if (current_time - last_time < _config.signal_cooldown_ms)
    {
        return signal;  // Still in cooldown
    }
    
    // Get state (read lock - arb thread also writes to this map)
    SpreadState state;
    {
        SpinLockGuard lock(_pair_states_spin);
        auto state_it = _pair_states.find(pair_id);
        if (state_it == _pair_states.end())
            return signal;
        state = state_it->second; // Copy for thread safety
    }
    
    // Check risk limits
    if (!_risk_manager->canOpenPosition(pair_id, 1.0))
    {
        signal.type = SpreadSignalType::NONE;
        return signal;
    }
    
    // Generate signal from strategy
    auto strat_it = _strategies.find(pair_id);
    if (strat_it == _strategies.end())
        return signal;
    
    const auto& instance = strat_it->second;
    
    if (instance.mean_reversion)
    {
        signal = instance.mean_reversion->generateSignal(state, current_time);
    }
    else if (instance.trend_following)
    {
        signal = instance.trend_following->generateSignal(state, current_time);
    }
    else if (instance.pairs_trading)
    {
        signal = instance.pairs_trading->generateSignal(state, current_time);
    }
    else if (instance.statistical_arb)
    {
        signal = instance.statistical_arb->generateSignal(state, current_time);
    }
    
    // Check confidence threshold
    if (signal.confidence < _config.min_signal_confidence)
    {
        signal.type = SpreadSignalType::NONE;
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
    
    auto signal_it = _last_signals.find(pair_id);
    SpreadSignal signal;
    // _last_signals与generateSignals在arb线程写入，需spinlock保护
    {
        SpinLockGuard lock(_pair_states_spin);
        signal_it = _last_signals.find(pair_id);
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
    auto pairs = _calculator_manager->getPairsForContract(code);
    
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
    if (prev_position == 0 && state.spread_position != 0)
    {
        state.position_open_time = state.last_update;
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
        if (kv.second.mean_reversion)
            kv.second.mean_reversion->reset();
        if (kv.second.trend_following)
            kv.second.trend_following->reset();
        if (kv.second.pairs_trading)
            kv.second.pairs_trading->reset();
        if (kv.second.statistical_arb)
            kv.second.statistical_arb->reset();
    }
    
    _last_signal_time.clear();
    _last_signals.clear();
}

} // namespace futu
