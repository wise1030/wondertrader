/*!
 * \file FutuRiskMonitor.h
 * \brief Simplified Risk Monitoring for Futures Market Making
 * 
 * Design: Reads data from FutuPortfolio (no redundant state tracking)
 * 
 * Responsibilities:
 *   - Rate limits (order/cancel/trade per second)
 *   - Risk rule evaluation (using Portfolio data)
 *   - Risk action execution
 *   - Event notification for risk alerts
 * 
 * Performance: Uses atomic counters for lock-free rate tracking
 */
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <cmath>
#include <mutex>
#include <array>
#include "../Includes/FasterDefs.h"
#include "FutuConfig.h"
#include "../Share/LockFreeRingBuffer.hpp"

NS_WTP_BEGIN
class EventNotifier;
NS_WTP_END

namespace futu {

class FutuPortfolio;  // Forward declaration
class UnifiedOrderTracker;  // Forward declaration

/// Risk limit types
enum class RiskLimitType
{
    POSITION_LONG,      ///< Maximum long position
    POSITION_SHORT,     ///< Maximum short position
    POSITION_NET,       ///< Maximum net position
    DELTA,              ///< Maximum portfolio delta
    EXPOSURE,           ///< Maximum total exposure
    DAILY_LOSS,         ///< Maximum daily loss
    ORDER_RATE,         ///< Maximum orders per second
    CANCEL_RATE,        ///< Maximum cancels per second
    TRADE_RATE          ///< Maximum trades per second
};

/// Risk violation severity
enum class RiskSeverity
{
    WARNING,            ///< Approaching limit
    BREACH,             ///< Limit breached
    CRITICAL            ///< Multiple breaches or severe violation
};

/// Risk category for recovery mechanism
enum class RiskCategory
{
    REVERSIBLE,         ///< Reversible: position/exposure/delta limits (auto-recovery)
    IRREVERSIBLE        ///< Irreversible: daily loss (requires manual intervention)
};

/// Risk violation record
struct RiskViolation
{
    RiskLimitType type;
    RiskSeverity severity;
    std::string code;
    double current_value;
    double limit_value;
    double utilization;
    uint64_t timestamp;
    std::string message;
    
    RiskViolation()
        : type(RiskLimitType::POSITION_NET)
        , severity(RiskSeverity::WARNING)
        , current_value(0), limit_value(0), utilization(0)
        , timestamp(0)
    {}
};

/// Risk action to take
enum class RiskAction
{
    NONE,               ///< No action
    WARN,               ///< Log warning
    WIDEN_SPREAD,       ///< Widen quotes
    REDUCE_SIZE,        ///< Reduce order sizes
    BLOCK_SIDE_LONG,    ///< Block opening long positions
    BLOCK_SIDE_SHORT,   ///< Block opening short positions
    BLOCK_CONTRACT_OPENING, ///< Block opening orders on the specific breached contract
    PAUSE_QUOTING,      ///< Stop quoting temporarily (auto-recovery)
    FLATTEN_POSITION,   ///< Exit all positions
    HALT_TRADING        ///< Stop all trading (irreversible, requires manual intervention)
};

/// Rate limits configuration
struct RateLimits
{
    uint32_t max_orders_per_sec;
    uint32_t max_cancels_per_sec;
    uint32_t max_trades_per_sec;
    double max_delta_change_per_sec;
    uint32_t delta_rate_window_sec;
    uint32_t delta_rate_cooldown_ms;
    
    // 分级响应阈值
    double position_breach_pause_threshold;  ///< POSITION_NET 利用率触发 PAUSE_QUOTING (default 1.2)
    double delta_critical_mult;              ///< Delta critical 倍数 (default 1.5)
    double delta_warning_mult;              ///< Delta warning 倍数 (default 0.8)
    
    // 升级响应阈值
    uint32_t widen_threshold;               ///< breachCount 触发 WIDEN_SPREAD (default 1)
    uint32_t pause_threshold;               ///< breachCount 触发 PAUSE_QUOTING (default 2)
    uint32_t flatten_threshold;             ///< breachCount 触发 FLATTEN_POSITION (default 3)
    
    RateLimits()
        : max_orders_per_sec(50)
        , max_cancels_per_sec(30)
        , max_trades_per_sec(20)
    , max_delta_change_per_sec(10.0)
    , delta_rate_window_sec(5)
    , delta_rate_cooldown_ms(5000)
        , position_breach_pause_threshold(1.2)
        , delta_critical_mult(1.5)
        , delta_warning_mult(0.8)
        , widen_threshold(1)
        , pause_threshold(2)
        , flatten_threshold(3)
    {}
    
    static RateLimits fromVariant(wtp::WTSVariant* v) {
        RateLimits r;
        r.max_orders_per_sec = FutuConfig::readUInt32(v, "maxOrdersPerSec", 50);
        r.max_cancels_per_sec = FutuConfig::readUInt32(v, "maxCancelsPerSec", 30);
        r.max_trades_per_sec = FutuConfig::readUInt32(v, "maxTradesPerSec", 20);
        r.max_delta_change_per_sec =        FutuConfig::readDouble(v, "maxDeltaChangePerSec", 10.0);
        r.delta_rate_window_sec = FutuConfig::readUInt32(v, "deltaRateWindowSec", 5);
        r.delta_rate_cooldown_ms = FutuConfig::readUInt32(v, "deltaRateCooldownMs", 5000);
        r.position_breach_pause_threshold = FutuConfig::readDouble(v, "positionBreachPauseThreshold", 1.2);
        r.delta_critical_mult = FutuConfig::readDouble(v, "deltaCriticalMult", 1.5);
        r.delta_warning_mult = FutuConfig::readDouble(v, "deltaWarningMult", 0.8);
        r.widen_threshold = FutuConfig::readUInt32(v, "widenThreshold", 1);
        r.pause_threshold = FutuConfig::readUInt32(v, "pauseThreshold", 2);
        r.flatten_threshold = FutuConfig::readUInt32(v, "flattenThreshold", 3);
        return r;
    }
};

/// Recovery configuration for reversible risks - P1-3.3 enhanced
struct RecoveryConfig
{
    uint32_t cooldown_ms;           ///< Cooldown period before recovery (milliseconds)
    uint32_t check_interval_ms;     ///< Interval between recovery checks
    double   recovery_threshold;    ///< Risk utilization threshold for recovery (< 1.0)
    
    // Enhanced recovery limits
    uint32_t max_recovery_count;    ///< Maximum number of auto-recoveries per session
    double   pnl_recovery_ratio;    ///< Required PnL recovery ratio (e.g., 0.5 = 50% of loss recovered)
    double   max_loss_for_recovery; ///< Max absolute loss at halt to allow auto-recovery (0=disabled)
    
    RecoveryConfig()
        : cooldown_ms(30000)
        , check_interval_ms(5000)
        , recovery_threshold(0.8)
        , max_recovery_count(3)
        , pnl_recovery_ratio(0.5)
        , max_loss_for_recovery(0)
    {}
    
    static RecoveryConfig fromVariant(wtp::WTSVariant* v) {
        RecoveryConfig c;
        c.cooldown_ms = FutuConfig::readUInt32(v, "cooldownMs", 30000);
        c.check_interval_ms = FutuConfig::readUInt32(v, "checkIntervalMs", 5000);
        c.recovery_threshold = FutuConfig::readDouble(v, "recoveryThreshold", 0.8);
        c.max_recovery_count = FutuConfig::readUInt32(v, "maxRecoveryCount", 3);
        c.pnl_recovery_ratio = FutuConfig::readDouble(v, "pnlRecoveryRatio", 0.5);
        c.max_loss_for_recovery = FutuConfig::readDouble(v, "maxLossForRecovery", 0);
        return c;
    }
};

/// Closeout configuration for session end
struct CloseoutConfig
{
    uint32_t minutes_before;        ///< Minutes before day close to stop quoting (0=disabled)
    uint32_t max_retries;           ///< Max retries for closeout orders
    uint32_t retry_interval_ms;     ///< Retry interval in ms
    uint32_t night_close_time;      ///< Night session close time (HHMM format, 0=no night session)
    uint32_t night_minutes_before;  ///< Minutes before night close to stop quoting
    
    CloseoutConfig()
        : minutes_before(5)
        , max_retries(3)
        , retry_interval_ms(5000)
        , night_close_time(0)
        , night_minutes_before(5)
    {}
    
    static CloseoutConfig fromVariant(wtp::WTSVariant* v) {
        CloseoutConfig c;
        c.minutes_before = FutuConfig::readUInt32(v, "minutesBefore", 5);
        c.max_retries = FutuConfig::readUInt32(v, "maxRetries", 3);
        c.retry_interval_ms = FutuConfig::readUInt32(v, "retryIntervalMs", 5000);
        c.night_close_time = FutuConfig::readUInt32(v, "nightCloseTime", 0);
        c.night_minutes_before = FutuConfig::readUInt32(v, "nightMinutesBefore", c.minutes_before);
        return c;
    }
};

/// Closeout state machine states
enum class CloseoutState
{
    IDLE,           ///< Not triggered, normal trading
    TRIGGERED,      ///< Triggered, waiting to execute closeout
    FLATTENING,     ///< Executing flatten orders
    COMPLETED,      ///< Closeout completed
    FAILED,         ///< Closeout failed (e.g., partial fill, no liquidity)
    RETRYING        ///< Retrying closeout after failure
};

/// Closeout state with transition tracking
struct CloseoutStateInfo
{
    CloseoutState state;
    uint64_t trigger_time;      ///< When state was triggered
    uint64_t flatten_start;     ///< When flattening started
    uint64_t complete_time;     ///< When completed
    uint64_t fail_time;         ///< When last failure occurred
    uint32_t retry_count;       ///< Number of retries attempted
    uint32_t max_retries;       ///< Maximum retries before giving up (default 3)
    uint64_t retry_interval_ms; ///< Interval between retries in ms (default 5000)
    
    CloseoutStateInfo()
        : state(CloseoutState::IDLE)
        , trigger_time(0), flatten_start(0), complete_time(0), fail_time(0)
        , retry_count(0), max_retries(3), retry_interval_ms(5000)
    {}
    
    inline bool canTransitionTo(CloseoutState next) const
    {
        switch (state)
        {
            case CloseoutState::IDLE:
                return next == CloseoutState::TRIGGERED;
            case CloseoutState::TRIGGERED:
                return next == CloseoutState::FLATTENING || next == CloseoutState::COMPLETED;
            case CloseoutState::FLATTENING:
                return next == CloseoutState::COMPLETED || next == CloseoutState::FAILED;
            case CloseoutState::COMPLETED:
                // FIX P2-5: Allow COMPLETED→IDLE for night session reset
                // (白盘时段需重置夜盘的COMPLETED状态以恢复交易)
                return next == CloseoutState::IDLE;
            case CloseoutState::FAILED:
                return next == CloseoutState::RETRYING || next == CloseoutState::COMPLETED;
            case CloseoutState::RETRYING:
                return next == CloseoutState::FLATTENING || next == CloseoutState::FAILED || next == CloseoutState::COMPLETED;
            default:
                return false;
        }
    }
};

/// Simplified Risk Monitor - reads from Portfolio
class FutuRiskMonitor
{
public:
    FutuRiskMonitor();
    ~FutuRiskMonitor() {}
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setRateLimits(const RateLimits& limits) { _rate_limits = limits; }
    const RateLimits& getRateLimits() const { return _rate_limits; }
    
    void setRecoveryConfig(const RecoveryConfig& config) { _recovery_config = config; }
    const RecoveryConfig& getRecoveryConfig() const { return _recovery_config; }
    
    void setCloseoutConfig(const CloseoutConfig& config) { 
        _closeout_config = config; 
        _closeout_state.max_retries = config.max_retries;
        _closeout_state.retry_interval_ms = config.retry_interval_ms;
    }
    const CloseoutConfig& getCloseoutConfig() const { return _closeout_config; }
    
    void setCurrentTime(uint64_t time) { _current_time.store(time, std::memory_order_relaxed); }
    
    /// Set event notifier for risk alerts (optional)
    void setEventNotifier(wtp::EventNotifier* notifier) { _event_notifier = notifier; }
    
    //==========================================================================
    // Rate Tracking (lock-free using atomics)
    //==========================================================================
    
    void recordOrder();
    void recordCancel();
    void recordTrade();
    
    //==========================================================================
    // Risk Checks - using Portfolio data
    //==========================================================================
    
    /// Check all risk limits using Portfolio data
    std::vector<RiskViolation> checkRiskLimits(const FutuPortfolio* portfolio);
    
    /// Pre-trade position limit check: can we place bid/ask for this contract?
    /// Checks current position + pending orders against max_position
    struct PreTradeResult {
        bool allow_bid;
        bool allow_ask;
    };
    PreTradeResult checkPreTradePosition(const std::string& code,
                                          const FutuPortfolio* portfolio,
                                          const UnifiedOrderTracker* tracker) const;
    
    /// Check rate limits only
    bool checkRateLimits() const;
    
    /// Get current rate counts (atomic reads)
    inline uint32_t getOrdersPerSec() const { 
        return static_cast<uint32_t>(std::max(0, _orders_last_sec.load(std::memory_order_relaxed))); 
    }
    inline uint32_t getCancelsPerSec() const { 
        return static_cast<uint32_t>(std::max(0, _cancels_last_sec.load(std::memory_order_relaxed))); 
    }
    inline uint32_t getTradesPerSec() const { 
        return static_cast<uint32_t>(std::max(0, _trades_last_sec.load(std::memory_order_relaxed))); 
    }
    
    //==========================================================================
    // Actions
    //==========================================================================
    
    /// Determine appropriate action based on violations
    RiskAction determineAction(const std::vector<RiskViolation>& violations) const;
    
    /// Determine action with risk category (for recovery mechanism)
    /// @param violations List of violations
    /// @param outCategory Output: risk category (reversible/irreversible)
    /// @return Action to take
    RiskAction determineActionWithCategory(const std::vector<RiskViolation>& violations,
                                           RiskCategory& outCategory) const;
    
    //==========================================================================
    // State
    //==========================================================================
    
    inline bool isTradingHalted() const { 
        return _trading_halted.load(std::memory_order_relaxed); 
    }
    
    inline bool isLongBlocked() const {
        return _long_blocked.load(std::memory_order_relaxed);
    }
    
    inline bool isShortBlocked() const {
        return _short_blocked.load(std::memory_order_relaxed);
    }
    
    inline bool isQuotingPaused() const {
        return _quoting_paused.load(std::memory_order_relaxed);
    }
    
    inline RiskCategory getHaltCategory() const {
        return _halt_category;
    }
    
    /// Halt trading with category (irreversible risks need manual recovery)
    /// @param category Risk category (reversible/irreversible)
    /// @param pnl_snapshot Current PnL at halt time (for loss-based recovery check)
    void haltTrading(RiskCategory category = RiskCategory::REVERSIBLE, double pnl_snapshot = 0);
    
    /// Resume trading (only for reversible risks)
    /// FIX P1-10: Returns true if successfully resumed, false if IRREVERSIBLE
    bool resumeTrading();
    
    /// Block opening long positions
    void blockLong() {
        _long_blocked.store(true, std::memory_order_relaxed);
        broadcastAlert("LONG_BLOCKED", "Opening long positions has been blocked");
    }
    
    /// Block opening short positions
    void blockShort() {
        _short_blocked.store(true, std::memory_order_relaxed);
        broadcastAlert("SHORT_BLOCKED", "Opening short positions has been blocked");
    }
    
    /// Unblock long positions
    void unblockLong() {
        _long_blocked.store(false, std::memory_order_relaxed);
    }
    
    /// Unblock short positions
    void unblockShort() {
        _short_blocked.store(false, std::memory_order_relaxed);
    }
    
    /// Pause quoting (reversible)
    void pauseQuoting();
    
    /// Resume quoting
    void resumeQuoting();
    
    //==========================================================================
    // Closeout Management (收盘前平仓) - State Machine
    //==========================================================================
    
    /// Check if closeout should be triggered
    /// @param currentTime Current time in HHMMSS format
    /// @param closeTime Session close time in HHMMSS format (from WTSSessionInfo)
    /// @return true if closeout triggered, false otherwise
    bool checkCloseout(uint32_t currentTime, uint32_t closeTime);
    
    /// Get current closeout state
    inline CloseoutState getCloseoutState() const {
        return _closeout_state.state;
    }
    
    /// Get closeout state info
    inline const CloseoutStateInfo& getCloseoutStateInfo() const {
        return _closeout_state;
    }
    
    /// Check if closeout has been triggered
    inline bool isCloseoutTriggered() const {
        return _closeout_state.state != CloseoutState::IDLE;
    }
    
    /// Check if closeout has been completed
    inline bool isCloseoutCompleted() const {
        return _closeout_state.state == CloseoutState::COMPLETED;
    }
    
    /// Check if currently flattening positions
    inline bool isCloseoutFlattening() const {
        return _closeout_state.state == CloseoutState::FLATTENING;
    }
    
    /// Transition closeout state (with validation)
    /// @return true if transition successful, false otherwise
    bool transitionCloseoutState(CloseoutState next_state, uint64_t timestamp = 0);
    
    /// Mark closeout as triggered
    void markCloseoutTriggered(uint64_t timestamp = 0);
    
    /// Mark closeout as flattening
    void markCloseoutFlattening(uint64_t timestamp = 0);
    
    /// Mark closeout as completed
    void markCloseoutCompleted(uint64_t timestamp = 0);
    
    /// Mark closeout as failed (e.g., partial fill, no liquidity)
    void markCloseoutFailed(uint64_t timestamp = 0);
    
    /// Check if closeout retry is due and transition to RETRYING
    /// @param current_time_ms Current time in milliseconds
    /// @return true if retry should be attempted
    bool checkCloseoutRetry(uint64_t current_time_ms);
    
    /// Reset closeout state (for new trading day)
    void resetCloseout();
    
    //==========================================================================
    // Delta Rate Tracking (Delta变化速率监控)
    //==========================================================================
    
    /// Record current delta snapshot for rate tracking
    /// @param currentDelta Current portfolio delta
    /// @param timestampMs Current timestamp in milliseconds
    void recordDeltaSnapshot(double currentDelta, uint64_t timestampMs);
    
    /// Check if delta change rate exceeds limit
    /// @return true if delta rate breached, false otherwise
    bool checkDeltaRate() const;
    
    /// Get current delta change rate (absolute value per second)
    double getDeltaChangeRate() const;
    
    /// Check and handle delta rate breach (pause quoting if needed)
    /// @return true if quoting was paused due to delta rate breach
    bool checkAndHandleDeltaRateBreach();
    
    //==========================================================================
    // Recovery
    //==========================================================================
    
    /// Check if recovery is possible and perform recovery if conditions met
    /// @param portfolio Current portfolio state
    /// @return true if trading resumed, false otherwise
    bool checkAndRecover(const FutuPortfolio* portfolio);
    
    /// Check if recovery conditions are met
    bool canRecover(const FutuPortfolio* portfolio) const;
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void resetDaily();
    void resetSession();
    
    /// FIX P2-6: Manually clear IRREVERSIBLE halt (requires human confirmation)
    /// Returns true if successfully cleared, false if not in IRREVERSIBLE state
    bool clearIrreversible();

private:
    RateLimits _rate_limits;
    RecoveryConfig _recovery_config;
    
    // Lock-free atomic counters for rate tracking
    std::atomic<int32_t> _orders_last_sec{0};
    std::atomic<int32_t> _cancels_last_sec{0};
    std::atomic<int32_t> _trades_last_sec{0};
    
    // Timestamp tracking using fixed-size RingBuffer (no dynamic allocation)
    // This prevents memory reallocation and potential data races
    static constexpr size_t MAX_TIMESTAMPS = 256;  // Enough for 1 second at 200Hz
    wtp::LockFreeRingBuffer<uint64_t, MAX_TIMESTAMPS> _order_times;
    wtp::LockFreeRingBuffer<uint64_t, MAX_TIMESTAMPS> _cancel_times;
    wtp::LockFreeRingBuffer<uint64_t, MAX_TIMESTAMPS> _trade_times;
    
    // State
    std::atomic<bool> _trading_halted{false};
    std::atomic<bool> _long_blocked{false};
    std::atomic<bool> _short_blocked{false};
    std::atomic<bool> _quoting_paused{false};
    std::atomic<uint64_t> _current_time{0};
    
    // Risk category for halt (determines if recovery is possible)
    RiskCategory _halt_category{RiskCategory::REVERSIBLE};
    
    // Recovery timestamps
    uint64_t _halt_timestamp{0};        ///< When trading was halted
    uint64_t _pause_timestamp{0};       ///< When quoting was paused
    uint64_t _last_recovery_check{0};   ///< Last time recovery was checked
    
    // Recovery tracking
    mutable uint32_t _recovery_count{0};        ///< Number of auto-recoveries this session
    mutable double _halt_pnl_snapshot{0};       ///< PnL at halt time (for loss-based halt)
    mutable bool _was_loss_triggered{false};    ///< Whether halt was triggered by daily loss
    
    // Closeout state (收盘前平仓) - State Machine
    CloseoutConfig _closeout_config;
    CloseoutStateInfo _closeout_state;
    
    // Delta rate tracking
    struct DeltaSnapshot {
        double delta;
        uint64_t timestamp_ms;
        DeltaSnapshot() : delta(0), timestamp_ms(0) {}
        DeltaSnapshot(double d, uint64_t t) : delta(d), timestamp_ms(t) {}
    };
    static constexpr size_t DELTA_SNAPSHOT_CAPACITY = 32;
    std::array<DeltaSnapshot, DELTA_SNAPSHOT_CAPACITY> _delta_snapshots;
    size_t _delta_snapshot_count;
    size_t _delta_snapshot_head;
    std::atomic<bool> _delta_rate_breached{false};
    uint64_t _delta_rate_breach_time{0};
    
    // Event notifier (optional)
    wtp::EventNotifier* _event_notifier = nullptr;
    
    /// Broadcast risk alert via EventNotifier
    void broadcastAlert(const std::string& alertType, const std::string& message);
};

} // namespace futu
