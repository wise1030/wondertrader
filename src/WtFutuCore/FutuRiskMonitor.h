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
    // B5: POSITION_LONG/SHORT 已删除 — v7.1 连续控制接管仓位风险
    // (skew/qty-decay/obligation/taker), BLOCK_SIDE 硬动作实际等价全停,
    // 方向性违规类型从未被产生, 属 v7.1 前残留死代码
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
    WIDEN_SPREAD,       ///< Widen quotes (策略性软响应: util 0.8→×1.5, 0.9→×2.0)
    BLOCK_SIDE_LONG,    ///< Block opening long positions
    BLOCK_SIDE_SHORT,   ///< Block opening short positions
    PAUSE_QUOTING,      ///< v7.3 已退役: 判定数学上不可达, 分支已删 (保留枚举值防序号错位)
    FLATTEN_POSITION,   ///< v7.3 已退役: breachCount 恒<=1 不可达, 分支已删; 强平由 HALT FORCE FLAT 承担
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
    double position_breach_pause_threshold;  ///< v7.3: 仅作 canRecover 恢复闸 (pos < maxPos×1.2 才允许恢复);
                                             ///<   PAUSE_QUOTING 入口已删除, 名称保留兼容配置
    double delta_critical_mult;              ///< Delta critical 倍数 (default 1.5)
    double delta_warning_mult;              ///< Delta warning 倍数 (default 0.8)

    // R2.2: 策略性软响应阈值 (WIDEN_SPREAD 分级; 做市最低报价数量要求 → 无 REDUCE_SIZE)
    double position_warning_l1;              ///< util L1 → WIDEN_SPREAD ×1.5 (default 0.8)
    double position_warning_l2;              ///< util L2 → WIDEN_SPREAD ×2.0 (default 0.9)
    double position_hard_block_ratio;        ///< 持仓硬止比例 (default 1.0, 仅flexible模式)

    // 升级响应阈值
    uint32_t widen_threshold;               ///< breachCount 触发 WIDEN_SPREAD (default 1)
    // v7.3: flatten_threshold 已删除 — FLATTEN_POSITION 不可达分支随 PAUSE 一并清理
    
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
        , position_warning_l1(0.8)
        , position_warning_l2(0.9)
        , position_hard_block_ratio(1.0)
        , widen_threshold(1)
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
        r.position_warning_l1 = FutuConfig::readDouble(v, "positionWarningL1", 0.8);
        r.position_warning_l2 = FutuConfig::readDouble(v, "positionWarningL2", 0.9);
        r.position_hard_block_ratio = FutuConfig::readDouble(v, "positionHardBlockRatio", 1.0);
        r.widen_threshold = FutuConfig::readUInt32(v, "widenThreshold", 1);
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
    bool     auto_clear_irreversible_on_reset; ///< v7.1: resetDaily 自动清除 IRREVERSIBLE halt (回测用, 模拟隔夜人工复核; 生产默认 false)
    
    RecoveryConfig()
        : cooldown_ms(30000)
        , check_interval_ms(5000)
        , recovery_threshold(0.8)
        , max_recovery_count(3)
        , pnl_recovery_ratio(0.5)
        , max_loss_for_recovery(0)
        , auto_clear_irreversible_on_reset(false)
    {}
    
    static RecoveryConfig fromVariant(wtp::WTSVariant* v) {
        RecoveryConfig c;
        c.cooldown_ms = FutuConfig::readUInt32(v, "cooldownMs", 30000);
        c.check_interval_ms = FutuConfig::readUInt32(v, "checkIntervalMs", 5000);
        c.recovery_threshold = FutuConfig::readDouble(v, "recoveryThreshold", 0.8);
        c.max_recovery_count = FutuConfig::readUInt32(v, "maxRecoveryCount", 3);
        c.pnl_recovery_ratio = FutuConfig::readDouble(v, "pnlRecoveryRatio", 0.5);
        c.max_loss_for_recovery = FutuConfig::readDouble(v, "maxLossForRecovery", 0);
        c.auto_clear_irreversible_on_reset = FutuConfig::readBool(v, "autoClearIrreversibleOnReset", false);
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
        , max_retries(10)
        , retry_interval_ms(2000)
        , night_close_time(0)
        , night_minutes_before(5)
    {}
    
    static CloseoutConfig fromVariant(wtp::WTSVariant* v) {
        CloseoutConfig c;
        c.minutes_before = FutuConfig::readUInt32(v, "minutesBefore", 5);
        c.max_retries = FutuConfig::readUInt32(v, "maxRetries", 10);
        c.retry_interval_ms = FutuConfig::readUInt32(v, "retryIntervalMs", 2000);
        c.night_close_time = FutuConfig::readUInt32(v, "nightCloseTime", 0);
        c.night_minutes_before = FutuConfig::readUInt32(v, "nightMinutesBefore", c.minutes_before);
        return c;
    }
};

/// Closeout sub-state machine (P1-1: merged old CloseoutState + CloseoutPhase)
/// Used by both RiskMonitor (SSOT with metadata) and CloseoutExecutor
enum class CloseoutSub : uint8_t
{
    IDLE,           ///< Not triggered, normal trading
    TRIGGERED,      ///< Triggered, waiting to execute closeout
    DRAINING,       ///< Executor: waiting for inflight orders to settle
    ASSESSING,      ///< Executor: reading net delta, computing remaining
    EXECUTING,      ///< Executor: iterative FAK batches
    COMPLETED,      ///< Closeout completed
    FAILED,         ///< Closeout failed (e.g., partial fill, no liquidity)
    RETRYING        ///< Retrying closeout after failure
};

/// Closeout sub-state with transition tracking
struct CloseoutSubInfo
{
    CloseoutSub state;
    uint64_t trigger_time;      ///< When state was triggered
    uint64_t flatten_start;     ///< When executor started (DRAINING entry)
    uint64_t complete_time;     ///< When completed
    uint64_t fail_time;         ///< When last failure occurred
    uint32_t retry_count;       ///< Number of retries attempted
    uint32_t max_retries;       ///< Maximum retries before giving up (default 3)
    uint64_t retry_interval_ms; ///< Interval between retries in ms (default 5000)
    bool is_night_closeout;     ///< was this triggered by night session closeout?
    bool night_closeout_done;   ///< night closeout already executed this session
    
    CloseoutSubInfo()
        : state(CloseoutSub::IDLE)
        , trigger_time(0), flatten_start(0), complete_time(0), fail_time(0)
        , retry_count(0), max_retries(3), retry_interval_ms(5000)
        , is_night_closeout(false), night_closeout_done(false)
    {}
    
    inline bool canTransitionTo(CloseoutSub next) const
    {
        switch (state)
        {
            case CloseoutSub::IDLE:
                return next == CloseoutSub::TRIGGERED;
            case CloseoutSub::TRIGGERED:
                return next == CloseoutSub::DRAINING || next == CloseoutSub::COMPLETED;
            case CloseoutSub::DRAINING:
                return next == CloseoutSub::ASSESSING || next == CloseoutSub::COMPLETED
                    || next == CloseoutSub::FAILED;
            case CloseoutSub::ASSESSING:
                return next == CloseoutSub::EXECUTING || next == CloseoutSub::COMPLETED
                    || next == CloseoutSub::FAILED;
            case CloseoutSub::EXECUTING:
                return next == CloseoutSub::DRAINING || next == CloseoutSub::ASSESSING
                    || next == CloseoutSub::COMPLETED || next == CloseoutSub::FAILED;
            case CloseoutSub::COMPLETED:
                // Allow COMPLETED→IDLE for night session reset
                return next == CloseoutSub::IDLE;
            case CloseoutSub::FAILED:
                return next == CloseoutSub::RETRYING || next == CloseoutSub::COMPLETED;
            case CloseoutSub::RETRYING:
                return next == CloseoutSub::DRAINING || next == CloseoutSub::FAILED
                    || next == CloseoutSub::COMPLETED;
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
    void setMaxPendingPerSide(double v) { _max_pending_per_side = v; }
    
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
    
    /// 读侧剔除过期样本 — 旧实现只在 record 时推进窗口,
    /// 停止报单后旧时间戳永不过期 → RATE 误报持续存在.
    void pruneRateWindows(uint64_t now);
    
    //==========================================================================
    // Risk Checks - using Portfolio data
    //==========================================================================
    
    /// Check all risk limits using Portfolio data
    std::vector<RiskViolation> checkRiskLimits(const FutuPortfolio* portfolio);
    
    /// 零堆分配版本: 复用调用方缓冲 (热路径每 tick 调用)
    void checkRiskLimits(const FutuPortfolio* portfolio, std::vector<RiskViolation>& violations);

    /// R2.2: 策略性软响应检查 (util 0.8/0.9 → WIDEN_SPREAD, 不产生硬 violation)
    /// 由 Coordinator 在 checkRiskLimits 之前调用, soft action 不阻断 hard check
    RiskAction checkSoftLimits(const FutuPortfolio* portfolio) const;
    
    /// Pre-trade position limit check: can we place bid/ask for this contract?
    /// v3 软风控：不再硬 BLOCK，返回 utilization 让 Quoter 做 qty 衰减；
    ///           util>=1.0 时设 obligation 标志，强制减仓侧义务报价（≥10手/≤10ticks）
    /// 旧 allow_bid/allow_ask 保留兼容（v3 默认始终 true，仅 Toxicity/TradingState 可关）
    struct PreTradeResult {
        bool allow_bid;
        bool allow_ask;
        bool pending_drain_bid;        ///< pending超限 -> 撤该侧旧单+跳过本轮(obligation也生效)
        bool pending_drain_ask;
        bool hard_block_bid;           ///< 持仓超限 -> flexible模式qty=0 (obligation靠skew)
        bool hard_block_ask;
        double long_utilization;       ///< projected_long  / max_position，>=1 → ask 义务
        double short_utilization;      ///< projected_short / max_position，>=1 → bid 义务
        bool force_ask_obligation;     ///< 多头打满 → ask 必须保持义务报价
        bool force_bid_obligation;     ///< 空头打满 → bid 必须保持义务报价
    };
    PreTradeResult checkPreTradePosition(const std::string& code,
                                          const FutuPortfolio* portfolio,
                                          const UnifiedOrderTracker* tracker) const;
    
    /// Check rate limits only
    bool checkRateLimits();
    
    /// Get current rate counts (ring buffer size)
    inline uint32_t getOrdersPerSec() const {
        return static_cast<uint32_t>(_order_times.size());
    }
    inline uint32_t getCancelsPerSec() const {
        return static_cast<uint32_t>(_cancel_times.size());
    }
    inline uint32_t getTradesPerSec() const {
        return static_cast<uint32_t>(_trade_times.size());
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
    /// Returns true if successfully resumed, false if IRREVERSIBLE
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
    inline CloseoutSub getCloseoutSub() const {
        return _closeout_state.state;
    }
    
    /// Get closeout state info
    inline const CloseoutSubInfo& getCloseoutSubInfo() const {
        return _closeout_state;
    }
    
    /// Get night session close time (HHMM, 0=no night session) -- Bug A close_time
    inline uint32_t getNightCloseTime() const {
        return _closeout_config.night_close_time;
    }
    
    /// Check if closeout has been triggered
    inline bool isCloseoutTriggered() const {
        return _closeout_state.state != CloseoutSub::IDLE;
    }
    
    /// Check if closeout has been completed
    inline bool isCloseoutCompleted() const {
        return _closeout_state.state == CloseoutSub::COMPLETED;
    }
    
    /// Check if currently in executor-managed active states
    inline bool isCloseoutFlattening() const {
        return _closeout_state.state == CloseoutSub::DRAINING
            || _closeout_state.state == CloseoutSub::ASSESSING
            || _closeout_state.state == CloseoutSub::EXECUTING;
    }
    
    /// Transition closeout state (with validation)
    /// @return true if transition successful, false otherwise
    bool transitionCloseoutSub(CloseoutSub next_state, uint64_t timestamp = 0);
    
    /// Mark closeout as triggered
    void markCloseoutTriggered(uint64_t timestamp = 0);
    
    /// Mark closeout as draining (executor started, first executor-managed state)
    void markCloseoutDraining(uint64_t timestamp = 0);
    
    /// Mark closeout as completed
    void markCloseoutCompleted(uint64_t timestamp = 0);
    
    /// Mark closeout as failed (e.g., partial fill, no liquidity)
    void markCloseoutFailed(uint64_t timestamp = 0);
    
    /// Check if closeout retry is due and transition to RETRYING
    /// @param current_time_ms Current time in milliseconds
    /// @return true if retry should be attempted
    bool checkCloseoutRetry(uint64_t current_time_ms);
    
    /// Reset closeout state (for new trading day)
    /// @param force If true, bypass state machine canTransitionTo check.
    ///              session_begin must use force=true: a new trading day is a hard
    ///              boundary, any leftover non-IDLE state from previous session
    ///              (e.g. stuck FLATTENING due to hedge fill not fully closing delta)
    ///              must be wiped clean rather than blocked.
    void resetCloseout(bool force = false);
    
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
    
    /// Manually clear IRREVERSIBLE halt (requires human confirmation)
    /// Returns true if successfully cleared, false if not in IRREVERSIBLE state
    bool clearIrreversible();

private:
    RateLimits _rate_limits;
    double _max_pending_per_side{0.0};  ///< Per-side max pending qty (from OrderControl, 0=disabled)
    RecoveryConfig _recovery_config;
    
    // Lock-free atomic counters for rate tracking
    // P2-1: atomic 双轨计数已移除,直接用 ring buffer size()
    // 旧代码 try_push 失败仍 +1 导致计数虚高
    
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
    CloseoutSubInfo _closeout_state;
    
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
