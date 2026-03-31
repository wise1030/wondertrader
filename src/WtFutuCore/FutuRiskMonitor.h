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
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"
#include "../Share/RingBuffer.hpp"

NS_WTP_BEGIN
class EventNotifier;
NS_WTP_END

namespace futu {

class FutuPortfolio;  // Forward declaration

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
    
    RateLimits()
        : max_orders_per_sec(50)
        , max_cancels_per_sec(30)
        , max_trades_per_sec(20)
    {}
};

/// Recovery configuration for reversible risks
struct RecoveryConfig
{
    uint32_t cooldown_ms;           ///< Cooldown period before recovery (milliseconds)
    uint32_t check_interval_ms;     ///< Interval between recovery checks
    double   recovery_threshold;    ///< Risk utilization threshold for recovery (< 1.0)
    
    RecoveryConfig()
        : cooldown_ms(30000)        // 30 seconds default cooldown
        , check_interval_ms(5000)   // 5 seconds default check interval
        , recovery_threshold(0.8)   // 80% utilization threshold
    {}
};

/// Closeout configuration for session end
struct CloseoutConfig
{
    uint32_t minutes_before;        ///< Minutes before close to stop quoting (0=disabled)
    bool flatten_position;          ///< Whether to flatten position at closeout
    
    CloseoutConfig()
        : minutes_before(5)         // 5 minutes before close
        , flatten_position(true)    // Flatten by default
    {}
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
    
    void setCloseoutConfig(const CloseoutConfig& config) { _closeout_config = config; }
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
    
    /// Check rate limits only
    bool checkRateLimits() const;
    
    /// Get current rate counts (atomic reads)
    inline uint32_t getOrdersPerSec() const { 
        return _orders_last_sec.load(std::memory_order_relaxed); 
    }
    inline uint32_t getCancelsPerSec() const { 
        return _cancels_last_sec.load(std::memory_order_relaxed); 
    }
    inline uint32_t getTradesPerSec() const { 
        return _trades_last_sec.load(std::memory_order_relaxed); 
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
    void haltTrading(RiskCategory category = RiskCategory::REVERSIBLE);
    
    /// Resume trading (only for reversible risks)
    void resumeTrading();
    
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
    // Closeout Management (收盘前平仓)
    //==========================================================================
    
    /// Check if closeout should be triggered
    /// @param currentTime Current time in HHMMSS format
    /// @param closeTime Session close time in HHMMSS format (from WTSSessionInfo)
    /// @return true if closeout triggered, false otherwise
    bool checkCloseout(uint32_t currentTime, uint32_t closeTime);
    
    /// Check if closeout has been triggered
    inline bool isCloseoutTriggered() const {
        return _closeout_triggered.load(std::memory_order_relaxed);
    }
    
    /// Check if closeout has been completed
    inline bool isCloseoutCompleted() const {
        return _closeout_completed.load(std::memory_order_relaxed);
    }
    
    /// Mark closeout as completed
    void markCloseoutCompleted() {
        _closeout_completed.store(true, std::memory_order_relaxed);
    }
    
    /// Reset closeout state (for new trading day)
    void resetCloseout();
    
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

private:
    RateLimits _rate_limits;
    RecoveryConfig _recovery_config;
    
    // Lock-free atomic counters for rate tracking
    std::atomic<uint32_t> _orders_last_sec{0};
    std::atomic<uint32_t> _cancels_last_sec{0};
    std::atomic<uint32_t> _trades_last_sec{0};
    
    // Timestamp tracking using fixed-size RingBuffer (no dynamic allocation)
    // This prevents memory reallocation and potential data races
    static constexpr size_t MAX_TIMESTAMPS = 256;  // Enough for 1 second at 200Hz
    RingBuffer<uint64_t, MAX_TIMESTAMPS> _order_times;
    RingBuffer<uint64_t, MAX_TIMESTAMPS> _cancel_times;
    RingBuffer<uint64_t, MAX_TIMESTAMPS> _trade_times;
    
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
    
    // Closeout state (收盘前平仓)
    CloseoutConfig _closeout_config;
    std::atomic<bool> _closeout_triggered{false};
    std::atomic<bool> _closeout_completed{false};
    
    // Event notifier (optional)
    wtp::EventNotifier* _event_notifier = nullptr;
    
    /// Broadcast risk alert via EventNotifier
    void broadcastAlert(const std::string& alertType, const std::string& message);
};

} // namespace futu
