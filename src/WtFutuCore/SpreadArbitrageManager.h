/*!
 * \file SpreadArbitrageManager.h
 * \brief Main Manager for Cross-Term Spread Arbitrage System
 * 
 * Coordinates all spread arbitrage components:
 *   - Spread calculation and monitoring
 *   - Strategy signal generation
 *   - Risk management
 *   - Order execution coordination
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include "SpreadArbitrageTypes.h"
#include "SpreadCalculator.h"
#include "SpreadRiskManager.h"
#include "MeanReversionStrategy.h"
#include "TrendFollowingStrategy.h"
#include "PairsTradingStrategy.h"
#include "StatisticalArbStrategy.h"
#include "MarketMakingEnhancer.h"
#include "../Includes/FasterDefs.h"
#include <memory>
#include <vector>
#include <functional>
#include <atomic>

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

namespace futu {

// Forward declaration: SpreadArbMgr uses Portfolio as SSOT for derived positions.
// Defined in FutuPortfolio.h; we only need the pointer type here.
class FutuPortfolio;

//==============================================================================
// Arbitrage Manager Configuration
//==============================================================================

struct SpreadArbitrageConfig
{
    // General settings
    bool enabled;                   ///< Enable spread arbitrage
    bool enhance_market_making;     ///< Enable MM enhancement
    bool use_hybrid_strategy;       ///< Use hybrid strategy combination
    
    // Strategy selection
    ArbitrageStrategy primary_strategy;
    
    // Portfolio settings
    double max_total_position;      ///< Maximum total position
    uint32_t max_pairs;             ///< Maximum number of pairs
    
    // Signal settings
    double min_signal_confidence;   ///< Minimum confidence for execution
    uint32_t signal_cooldown_ms;    ///< Cooldown between signals
    
    // Integration settings
    double mm_enhancement_weight;   ///< Weight of MM enhancement signals
    
    SpreadArbitrageConfig()
        : enabled(true)
        , enhance_market_making(true)
        , use_hybrid_strategy(false)
        , primary_strategy(ArbitrageStrategy::MEAN_REVERSION)
        , max_total_position(50.0)
        , max_pairs(10)
        , min_signal_confidence(0.3)
        , signal_cooldown_ms(1000)
        , mm_enhancement_weight(0.5)
    {}
};

//==============================================================================
// Strategy Instance
//==============================================================================

struct StrategyInstance
{
    std::string pair_id;
    ArbitrageStrategy strategy_type;
    
    std::unique_ptr<MeanReversionStrategy> mean_reversion;
    std::unique_ptr<TrendFollowingStrategy> trend_following;
    std::unique_ptr<PairsTradingStrategy> pairs_trading;
    std::unique_ptr<StatisticalArbStrategy> statistical_arb;
    
    StrategyInstance() = default;
    StrategyInstance(StrategyInstance&&) = default;
    StrategyInstance& operator=(StrategyInstance&&) = default;
};

//==============================================================================
// Signal Callback Types
//==============================================================================

/// Callback for spread signals
using SpreadSignalCallback = std::function<void(const SpreadSignal&)>;

/// Callback for risk alerts
using RiskAlertCallback = std::function<void(const RiskAlert&)>;

/// Callback for quoting adjustments
using QuotingAdjustCallback = std::function<void(const std::string& pair_id, 
                                                  const QuotingAdjustment&)>;

//==============================================================================
// Spread Arbitrage Manager
//==============================================================================

class SpreadArbitrageManager
{
public:
    SpreadArbitrageManager();
    ~SpreadArbitrageManager() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const SpreadArbitrageConfig& config) { _config = config; }
    const SpreadArbitrageConfig& getConfig() const { return _config; }
    
    /// Load configuration from YAML file
    /// @param config_file Path to spread_arbitrage.yaml
    /// @return true if successful, false otherwise
    bool loadConfig(const std::string& config_file);
    
    void setCalculatorConfig(const SpreadCalculatorConfig& config);
    void setRiskConfig(const SpreadRiskConfig& config);
    void setMmEnhancerConfig(const MmEnhancerConfig& config);
    
    /// Set expiry date callback for risk manager
    void setExpiryDateCallback(ExpiryDateCallback callback);
    
    /// Set current trading date for expiry calculation
    void setCurrentDate(uint32_t current_date);
    
    //==========================================================================
    // Spread Pair Management
    //==========================================================================
    
    /// Add a spread pair for trading
    bool addSpreadPair(const SpreadPairConfig& pair_config);
    
    /// Remove a spread pair
    void removeSpreadPair(const std::string& pair_id);
    
    /// Get all configured pairs
    std::vector<std::string> getSpreadPairs() const;
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// Process incoming tick
    void onTick(const std::string& code, double price, double multiplier, uint64_t timestamp);
    
    /// Process incoming WTSTickData
    void onWtTick(wtp::WTSTickData* tick);
    
    //==========================================================================
    // Signal Generation
    //==========================================================================
    
    /// Generate signals for all pairs
    std::vector<SpreadSignal> generateSignals(uint64_t current_time);
    
    /// Generate signal for specific pair
    SpreadSignal generateSignal(const std::string& pair_id, uint64_t current_time);
    
    //==========================================================================
    // Market Making Integration
    //==========================================================================
    
    /// Get quoting adjustment for a pair
    QuotingAdjustment getQuotingAdjustment(const std::string& pair_id, uint64_t current_time);
    
    /// Check if should pause quoting
    bool shouldPauseQuoting(const std::string& code, bool is_bid) const;
    
    //==========================================================================
    // Position Management
    //==========================================================================
    
    /// Update position for a pair
    void updatePosition(const std::string& pair_id, 
                        double leg1_pos, double leg2_pos,
                        double unrealized_pnl);
    
    /// Get current state for a pair
    SpreadState getSpreadState(const std::string& pair_id) const;
    
    /// Get all pair states
    std::vector<SpreadState> getAllStates() const;

    //==========================================================================
    // Scheme B-3: Portfolio-Derived Spread Monitoring
    //==========================================================================

    /// Inject Portfolio pointer (SSOT for all positions, MM + ARB combined).
    /// Must be called once at init, before any generateSignal call.
    /// Portfolio must outlive SpreadArbitrageManager.
    void setPortfolio(const FutuPortfolio* portfolio) { _portfolio_ptr = portfolio; }

    /// Callback when an arb order fills (called from UftFutuMmStrategy::on_trade
    /// after consumePairTag identifies the order as an arb order).
    /// Decrements in_flight_qty for the pair. Idempotent on partial fills.
    /// @param pair_id Spread pair id (from consumePairTag)
    /// @param filled_qty Filled quantity (absolute, single-leg fill)
    void onArbOrderFilled(const std::string& pair_id, double filled_qty);

    /// Configure in-flight timeout in milliseconds. Defaults to 60000 (60 seconds).
    /// Once elapsed, in_flight_qty is forcibly reset (defense against stuck orders
    /// that never deliver a fill callback).
    ///
    /// NOTE: applyB3Gate compares this against `current_time` which
    /// AsyncArbitrageExecutor derives from std::chrono::high_resolution_clock.
    /// Units MUST match — both are milliseconds.
    void setInFlightTimeoutMs(uint64_t milliseconds) { _in_flight_timeout_ms = milliseconds; }
    
    //==========================================================================
    // Risk Management
    //==========================================================================
    
    /// Get current risk summary
    PortfolioRiskSummary getRiskSummary() const;
    
    /// Check if position is allowed
    bool canOpenPosition(const std::string& pair_id, double size) const;
    
    /// Get active risk alerts
    std::vector<RiskAlert> getActiveAlerts() const;
    
    //==========================================================================
    // Callbacks
    //==========================================================================
    
    void setSignalCallback(SpreadSignalCallback callback) { _signal_callback = callback; }
    void setAlertCallback(RiskAlertCallback callback) { _alert_callback = callback; }
    void setQuotingCallback(QuotingAdjustCallback callback) { _quoting_callback = callback; }
    
    /// Set contract multiplier for a specific contract code
    void setContractMultiplier(const std::string& code, double multiplier) { _contract_multipliers[code] = multiplier; }
    
    //==========================================================================
    // Management
    //==========================================================================
    
    void reset();
    void enable() { _config.enabled = true; }
    void disable() { _config.enabled = false; }
    bool isEnabled() const { return _config.enabled; }
    
    size_t getPairCount() const { return _strategies.size(); }
    
private:
    //==========================================================================
    // Internal Methods
    //==========================================================================
    
    void initializeStrategy(StrategyInstance& instance, const SpreadPairConfig& config);
    SpreadSignal combineSignals(const std::vector<SpreadSignal>& signals, 
                                 const SpreadState& state,
                                 uint64_t current_time);
    void dispatchSignal(const SpreadSignal& signal);
    void checkRiskAlerts();
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    SpreadArbitrageConfig _config;
    SpreadCalculatorConfig _calc_config;
    SpreadRiskConfig _risk_config;
    MmEnhancerConfig _mm_config;
    
    // Statistical sub-strategy default parameters (from config file)
    uint32_t _default_mr_half_life = 100;
    double   _default_mr_entry_threshold = 2.0;
    double   _default_mr_exit_threshold = 0.5;
    uint32_t _default_pt_correlation_window = 100;
    double   _default_pt_min_correlation = 0.7;
    uint32_t _default_pt_spread_window = 50;
    uint32_t _default_tf_ma_period = 20;
    double   _default_tf_breakout_threshold = 1.5;
    
    //==========================================================================
    // Components
    //==========================================================================
    
    std::unique_ptr<SpreadCalculatorManager> _calculator_manager;
    std::unique_ptr<SpreadRiskManager> _risk_manager;
    std::unique_ptr<MarketMakingEnhancer> _mm_enhancer;
    
    //==========================================================================
    // Strategy Instances
    //==========================================================================
    
    wtp::wt_hashmap<std::string, StrategyInstance> _strategies;
    wtp::wt_hashmap<std::string, SpreadPairConfig> _pair_configs;
    
    //==========================================================================
    // Position Tracking
    //==========================================================================
    
    wtp::wt_hashmap<std::string, SpreadState> _pair_states;  ///< Spread state per pair
    mutable std::atomic_flag _pair_states_spin = ATOMIC_FLAG_INIT;  ///< Protects _pair_states
     
    //==========================================================================
    // Signal State
     //==========================================================================
     
     wtp::wt_hashmap<std::string, uint64_t> _last_signal_time;
     wtp::wt_hashmap<std::string, SpreadSignal> _last_signals;
    
    //==========================================================================
    // Callbacks
    //==========================================================================
    
    SpreadSignalCallback _signal_callback;
    RiskAlertCallback _alert_callback;
    QuotingAdjustCallback _quoting_callback;
    
    //==========================================================================
    // Contract Multipliers
    //==========================================================================
    
    wtp::wt_hashmap<std::string, double> _contract_multipliers;  ///< Per-contract multiplier lookup

    //==========================================================================
    // Scheme B-3: Portfolio-Derived Arb State
    //==========================================================================

    /// Pure helper: derive spread position from Portfolio (signed).
    /// Returns 0 when either leg is empty, legs co-directional, or matched < min_unit.
    /// Sign: +1 = long spread (leg1 long, leg2 short), -1 = short spread.
    /// Caller must hold no locks; this only reads portfolio (NOT thread-safe with
    /// concurrent portfolio writes — must be called from reader thread).
    double computeDerivedSpread(const SpreadPairConfig& cfg) const;

    /// Pure helper: derive arb intent from z-score (with hysteresis).
    /// Hysteresis band: exit_z < |z| < entry_z → keep prev intent.
    ArbIntent computeIntent(double z, const SpreadPairConfig& cfg, ArbIntent prev) const;

    /// B-3 gate: takes raw signal from strategy, applies portfolio-derived
    /// dedup + size adjustment + in-flight check. Returns possibly-modified
    /// signal (type=NONE if suppressed, or with adjusted suggested_size).
    /// Side effects: updates _pair_arb_states (intent, in_flight tracking).
    SpreadSignal applyB3Gate(const std::string& pair_id,
                              const SpreadSignal& raw,
                              uint64_t current_time);

    /// Portfolio SSOT pointer (injected via setPortfolio).
    /// Nullable: if null, B-3 logic falls back to legacy generateSignal path.
    const FutuPortfolio* _portfolio_ptr{nullptr};

    /// Per-pair arb state (intent + in-flight tracking).
    wtp::wt_hashmap<std::string, PairArbState> _pair_arb_states;
    mutable std::atomic_flag _pair_arb_spin = ATOMIC_FLAG_INIT;

    /// Pairs whose in_flight timed out and need cleanup (cancel pending legs).
    /// UftFutuMmStrategy polls this via popTimedOutPairs() each tick.
    std::vector<std::string> _timed_out_pairs;

    /// In-flight timeout in milliseconds. After this, in_flight_qty is forcibly
    /// reset. Default: 60 seconds (60000 ms). Compared against `current_time`
    /// passed to applyB3Gate. Units MUST match.
    uint64_t _in_flight_timeout_ms{60000ULL};

public:
    /// Pop pairs whose in_flight timed out (for external cancel cleanup).
    /// Returns true if any pairs were written to out_pairs.
    bool popTimedOutPairs(std::vector<std::string>& out_pairs)
    {
        while (_pair_arb_spin.test_and_set(std::memory_order_acquire)) {}
        if (_timed_out_pairs.empty()) {
            _pair_arb_spin.clear(std::memory_order_release);
            return false;
        }
        out_pairs = std::move(_timed_out_pairs);
        _timed_out_pairs.clear();
        _pair_arb_spin.clear(std::memory_order_release);
        return true;
    }
};

} // namespace futu
