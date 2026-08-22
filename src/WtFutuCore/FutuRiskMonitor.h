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
#include <algorithm>
#include <unordered_map>
#include "../Includes/FasterDefs.h"
#include "FutuConfig.h"
#include "PreTradeDecision.h"
#include "SideFillBreaker.h"
#include "RiskLimitsConfig.h"
#include "SpinLockGuard.h"
#include "../Share/LockFreeRingBuffer.hpp"

NS_WTP_BEGIN
class EventNotifier;
NS_WTP_END

namespace futu
{

class FutuPortfolio;       // Forward declaration
struct ContractState;       // A3: forward decl for checkPreTradePosition overload
class UnifiedOrderTracker; // Forward declaration

/// Risk limit types
enum class RiskLimitType
{
    // B5: POSITION_LONG/SHORT 已删除 — v7.1 连续控制接管仓位风险
    // (skew/qty-decay/obligation/taker), BLOCK_SIDE 硬动作实际等价全停,
    // 方向性违规类型从未被产生, 属 v7.1 前残留死代码
    POSITION_NET, ///< Maximum net position
    DELTA,        ///< Maximum portfolio delta
    EXPOSURE,     ///< Maximum total exposure
    DAILY_LOSS,   ///< Maximum daily loss
    ORDER_RATE,   ///< Maximum orders per second
    CANCEL_RATE,  ///< Maximum cancels per second
    TRADE_RATE    ///< Maximum trades per second
};

/// Risk violation severity
enum class RiskSeverity
{
    WARNING, ///< Approaching limit
    BREACH,  ///< Limit breached
    CRITICAL ///< Multiple breaches or severe violation
};

/// Risk category for recovery mechanism
enum class RiskCategory
{
    REVERSIBLE,  ///< Reversible: position/exposure/delta limits (auto-recovery)
    IRREVERSIBLE ///< Irreversible: daily loss (requires manual intervention)
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
        : type(RiskLimitType::POSITION_NET), severity(RiskSeverity::WARNING), current_value(0), limit_value(0),
          utilization(0), timestamp(0)
    {}
};

/// Risk action to take
enum class RiskAction
{
    NONE,             ///< No action
    WARN,             ///< Log warning
    WIDEN_SPREAD,     ///< Widen quotes (策略性软响应: util 0.8→×1.5, 0.9→×2.0)
    BLOCK_SIDE_LONG,  ///< Block opening long positions
    BLOCK_SIDE_SHORT, ///< Block opening short positions
    PAUSE_QUOTING,    ///< v7.3 已退役: 判定数学上不可达, 分支已删 (保留枚举值防序号错位)
    FLATTEN_POSITION, ///< v7.3 已退役: breachCount 恒<=1 不可达, 分支已删; 强平由 HALT FORCE FLAT 承担
    HALT_TRADING      ///< Stop all trading (irreversible, requires manual intervention)
};

/// Rate limits configuration (single source of truth, see RiskLimitsConfig.h)
using RateLimits = RiskRateLimits;

/// Recovery configuration for reversible risks - P1-3.3 enhanced
struct RecoveryConfig
{
    uint32_t cooldown_ms;       ///< Cooldown period before recovery (milliseconds)
    uint32_t check_interval_ms; ///< Interval between recovery checks
    double recovery_threshold;  ///< Risk utilization threshold for recovery (< 1.0)

    // Enhanced recovery limits
    uint32_t max_recovery_count;  ///< Maximum number of auto-recoveries per session
    double pnl_recovery_ratio;    ///< Required PnL recovery ratio (e.g., 0.5 = 50% of loss recovered)
    double max_loss_for_recovery; ///< Max absolute loss at halt to allow auto-recovery (0=disabled)
    bool
        auto_clear_irreversible_on_reset; ///< v7.1: resetDaily 自动清除 IRREVERSIBLE halt (回测用, 模拟隔夜人工复核; 生产默认 false)

    RecoveryConfig()
        : cooldown_ms(30000), check_interval_ms(5000), recovery_threshold(0.8), max_recovery_count(3),
          pnl_recovery_ratio(0.5), max_loss_for_recovery(0), auto_clear_irreversible_on_reset(false)
    {}

    static RecoveryConfig fromVariant(wtp::WTSVariant* v)
    {
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
    uint32_t minutes_before;       ///< Minutes before day close to stop quoting (0=disabled)
    uint32_t max_retries;          ///< Max retries for closeout orders
    uint32_t retry_interval_ms;    ///< Retry interval in ms
    uint32_t night_close_time;     ///< Night session close time (HHMM format, 0=no night session)
    uint32_t night_minutes_before; ///< Minutes before night close to stop quoting

    CloseoutConfig()
        : minutes_before(5), max_retries(10), retry_interval_ms(2000), night_close_time(0), night_minutes_before(5)
    {}

    static CloseoutConfig fromVariant(wtp::WTSVariant* v)
    {
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
    IDLE,      ///< Not triggered, normal trading
    TRIGGERED, ///< Triggered, waiting to execute closeout
    DRAINING,  ///< Executor: waiting for inflight orders to settle
    ASSESSING, ///< Executor: reading net delta, computing remaining
    EXECUTING, ///< Executor: iterative FAK batches
    COMPLETED, ///< Closeout completed
    FAILED,    ///< Closeout failed (e.g., partial fill, no liquidity)
    RETRYING   ///< Retrying closeout after failure
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
        : state(CloseoutSub::IDLE), trigger_time(0), flatten_start(0), complete_time(0), fail_time(0), retry_count(0),
          max_retries(3), retry_interval_ms(5000), is_night_closeout(false), night_closeout_done(false)
    {}

    inline bool canTransitionTo(CloseoutSub next) const
    {
        switch (state) {
        case CloseoutSub::IDLE:
            return next == CloseoutSub::TRIGGERED;
        case CloseoutSub::TRIGGERED:
            return next == CloseoutSub::DRAINING || next == CloseoutSub::COMPLETED;
        case CloseoutSub::DRAINING:
            return next == CloseoutSub::ASSESSING || next == CloseoutSub::COMPLETED || next == CloseoutSub::FAILED;
        case CloseoutSub::ASSESSING:
            return next == CloseoutSub::EXECUTING || next == CloseoutSub::COMPLETED || next == CloseoutSub::FAILED;
        case CloseoutSub::EXECUTING:
            return next == CloseoutSub::DRAINING || next == CloseoutSub::ASSESSING || next == CloseoutSub::COMPLETED ||
                   next == CloseoutSub::FAILED;
        case CloseoutSub::COMPLETED:
            // Allow COMPLETED→IDLE for night session reset
            return next == CloseoutSub::IDLE;
        case CloseoutSub::FAILED:
            return next == CloseoutSub::RETRYING || next == CloseoutSub::COMPLETED;
        case CloseoutSub::RETRYING:
            return next == CloseoutSub::DRAINING || next == CloseoutSub::FAILED || next == CloseoutSub::COMPLETED;
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

    void setRateLimits(const RateLimits& limits)
    {
        _rate_limits = limits;
        SideFillBreakerConfig breaker_cfg;
        breaker_cfg.max_consecutive_same_side = limits.max_consecutive_same_side;
        breaker_cfg.window_ms = limits.same_side_window_ms;
        breaker_cfg.pause_ms = limits.same_side_pause_ms;
        _side_fill_breaker.setConfig(breaker_cfg);
    }
    const RateLimits& getRateLimits() const { return _rate_limits; }
    void setMaxPendingPerSide(double v) { _max_pending_per_side = v; }

    /// B+: zombie 撤单升级的合约 halt 闩锁 (coordinator 在 zombie 升级时置位,
    /// 通道恢复 onChannelReady 时 clearZombieHalts 复位)。闩锁期间该合约
    /// halt_quoting - zombie 单悬而未决时不允许新增报价敞口。
    /// B+ 修复(P1-1): 数据线程 (升级/读) 与交易线程 (通道恢复/事件补挂读)
    /// 并发访问, SpinLockGuard 保护 (临界区 = 一次 map 操作, 无嵌套锁)。
    void setZombieHalt(const std::string& code)
    {
        SpinLockGuard _g(_zombie_halt_lock);
        _zombie_halt[code] = true;
    }
    void clearZombieHalts()
    {
        SpinLockGuard _g(_zombie_halt_lock);
        _zombie_halt.clear();
    }
    /// B+ 修复(P2-3): 释放无存活 zombie 单合约的闩锁 -- zombie 被 cancelAll 兜底
    /// 杀掉且回报清账后 (存活集合不再含该合约) 自动恢复报价, 无需等通道重连。
    /// 由 processAutoCancel 每 tick 以 tracker 的存活集合驱动。
    void retainZombieHalts(const std::vector<std::string>& aliveZombieContracts)
    {
        SpinLockGuard _g(_zombie_halt_lock);
        if (_zombie_halt.empty())
            return;
        for (auto it = _zombie_halt.begin(); it != _zombie_halt.end();) {
            if (std::find(aliveZombieContracts.begin(), aliveZombieContracts.end(), it->first)
                == aliveZombieContracts.end())
                it = _zombie_halt.erase(it);
            else
                ++it;
        }
    }

    void setRecoveryConfig(const RecoveryConfig& config) { _recovery_config = config; }
    const RecoveryConfig& getRecoveryConfig() const { return _recovery_config; }

    void setCloseoutConfig(const CloseoutConfig& config)
    {
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

    /// V8-P0-2: 批量计入 n 笔报单 (MM 路径 refreshQuotes 返回的实际挂单数;
    /// 单次加锁, 避免逐笔 recordOrder 的重复临界区开销)
    void recordOrders(uint32_t n);

    /// 同侧连续成交熔断（按合约独立计数）: 记录一笔成交,
    /// 返回 true 表示本次触发了熔断（调用方应立即撤该合约全部报价）。
    /// 暂停到期自动恢复；CLOSEOUT 阶段由调用方豁免（不调用本方法）。
    /// adds_inventory=false 的减仓成交只打断反侧序列, 不累计熔断。
    bool onSideFill(const std::string& code, bool is_buy, uint64_t now_ms, bool adds_inventory = true)
    {
        return _side_fill_breaker.onFill(code, is_buy, now_ms, adds_inventory);
    }

    /// 查询该合约是否处于熔断暂停期（合约级双边暂停，报价路径每 tick 调用）
    bool isPaused(const std::string& code, uint64_t now_ms) const
    {
        return _side_fill_breaker.isPaused(code, now_ms);
    }

    /// 清空熔断状态（策略重启/交易日切换时调用）
    void resetSideBreaker() { _side_fill_breaker.clear(); }

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

    /// Pre-trade check: returns PreTradeDecision = RiskVerdict (风控闸门) + StrategyInputs (策略输入).
    /// 分层: 风控(halt_quoting/drain)基于净头寸 vs maxPosition;
    ///       策略(util/obligation/block_add)基于 projected utilization.
    /// See PreTradeDecision.h for field documentation.
    PreTradeDecision checkPreTradePosition(const std::string& code,
                                         const FutuPortfolio* portfolio,
                                         const UnifiedOrderTracker* tracker,
                                         uint64_t now_ms = 0) const;
    /// A3: 复用 TickContext.cs 快照, 跳过重复 getContractSnapshot (递归锁+ContractState 拷贝)
    PreTradeDecision checkPreTradePosition(const ContractState& cs,
                                         const UnifiedOrderTracker* tracker,
                                         uint64_t now_ms = 0) const;

    /// Check rate limits only
    bool checkRateLimits();

    /// Get current rate counts (ring buffer size)
    inline uint32_t getOrdersPerSec() const { return static_cast<uint32_t>(_order_times.size()); }
    inline uint32_t getCancelsPerSec() const { return static_cast<uint32_t>(_cancel_times.size()); }
    inline uint32_t getTradesPerSec() const { return static_cast<uint32_t>(_trade_times.size()); }

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
                                           RiskCategory& outCategory,
                                           bool cost_basis_stale = false) const;

    //==========================================================================
    // State
    //==========================================================================

    inline bool isTradingHalted() const { return _trading_halted.load(std::memory_order_relaxed); }

    inline bool isLongBlocked() const { return _long_blocked.load(std::memory_order_relaxed); }

    inline bool isShortBlocked() const { return _short_blocked.load(std::memory_order_relaxed); }

    inline bool isQuotingPaused() const { return _quoting_paused.load(std::memory_order_relaxed); }

    inline RiskCategory getHaltCategory() const { return _halt_category; }

    /// 成本簿 stale 事件上报（EventNotifier/GUI 可见；按时间节流）
    void broadcastCostBasisStale(const std::string& code);

    /// Halt trading with category (irreversible risks need manual recovery)
    /// @param category Risk category (reversible/irreversible)
    /// @param pnl_snapshot Current PnL at halt time (for loss-based recovery check)
    void haltTrading(RiskCategory category = RiskCategory::REVERSIBLE, double pnl_snapshot = 0);

    /// Resume trading (only for reversible risks)
    /// Returns true if successfully resumed, false if IRREVERSIBLE
    bool resumeTrading();

    /// Block opening long positions
    void blockLong()
    {
        _long_blocked.store(true, std::memory_order_relaxed);
        broadcastAlert("LONG_BLOCKED", "Opening long positions has been blocked");
    }

    /// Block opening short positions
    void blockShort()
    {
        _short_blocked.store(true, std::memory_order_relaxed);
        broadcastAlert("SHORT_BLOCKED", "Opening short positions has been blocked");
    }

    /// Unblock long positions
    void unblockLong() { _long_blocked.store(false, std::memory_order_relaxed); }

    /// Unblock short positions
    void unblockShort() { _short_blocked.store(false, std::memory_order_relaxed); }

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
    inline CloseoutSub getCloseoutSub() const { return _closeout_state.state; }

    /// Get closeout state info
    inline const CloseoutSubInfo& getCloseoutSubInfo() const { return _closeout_state; }

    /// Get night session close time (HHMM, 0=no night session) -- Bug A close_time
    inline uint32_t getNightCloseTime() const { return _closeout_config.night_close_time; }

    /// Check if closeout has been triggered
    inline bool isCloseoutTriggered() const { return _closeout_state.state != CloseoutSub::IDLE; }

    /// Check if closeout has been completed
    inline bool isCloseoutCompleted() const { return _closeout_state.state == CloseoutSub::COMPLETED; }

    /// Check if currently in executor-managed active states
    inline bool isCloseoutFlattening() const
    {
        return _closeout_state.state == CloseoutSub::DRAINING || _closeout_state.state == CloseoutSub::ASSESSING ||
               _closeout_state.state == CloseoutSub::EXECUTING;
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
    double _max_pending_per_side{0.0}; ///< Per-side max pending qty (from OrderControl, 0=disabled)
    /// A3: checkPreTradePosition 实现体 (共享逻辑, 两个公开重载委托至此)
    PreTradeDecision checkPreTradePositionImpl(const std::string& code,
                                             const ContractState* cs,
                                             const UnifiedOrderTracker* tracker,
                                             uint64_t now_ms) const;
    /// 风控层实现：position 硬闸门、同侧成交熔断、pending drain
    RiskVerdict checkHardPositionRisk(const std::string& code,
                                     const ContractState* cs,
                                     double pending_buy,
                                     double pending_sell,
                                     uint64_t now_ms) const;
    /// 策略层实现：util、义务报价、flexible block_add
    StrategyInputs computeInventoryStrategyInputs(const std::string& code,
                                                 const ContractState* cs,
                                                 double pending_buy,
                                                 double pending_sell) const;
    RecoveryConfig _recovery_config;

    // Lock-free atomic counters for rate tracking
    // P2-1: atomic 双轨计数已移除,直接用 ring buffer size()
    // 旧代码 try_push 失败仍 +1 导致计数虚高

    // Timestamp tracking using fixed-size RingBuffer (no dynamic allocation)
    // This prevents memory reallocation and potential data races
    static constexpr size_t MAX_TIMESTAMPS = 256; // Enough for 1 second at 200Hz
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
    uint64_t _halt_timestamp{0};      ///< When trading was halted
    uint64_t _pause_timestamp{0};     ///< When quoting was paused
    uint64_t _last_recovery_check{0}; ///< Last time recovery was checked

    // Recovery tracking
    mutable uint32_t _recovery_count{0};     ///< Number of auto-recoveries this session
    mutable double _halt_pnl_snapshot{0};    ///< PnL at halt time (for loss-based halt)
    mutable bool _was_loss_triggered{false}; ///< Whether halt was triggered by daily loss

    // Closeout state (收盘前平仓) - State Machine
    CloseoutConfig _closeout_config;
    CloseoutSubInfo _closeout_state;

    // Delta rate tracking
    struct DeltaSnapshot
    {
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

    // 同侧连续成交熔断器（按合约独立计数，跨线程安全）
    SideFillBreaker _side_fill_breaker;

    // 热路径告警限频（避免持续超限时每 tick 刷屏/写盘，线上曾单日 100MB+）:
    // 同一条告警按时间节流，仅每 1s 最多输出一次。跨线程仅用于日志节流，
    // 偶发重复输出可接受，故 per-contract map 采用与 _halt_quoting_state 一致的
    // 宽松竞争模型；portfolio 级用原子时间戳。
    static constexpr uint64_t WARN_THROTTLE_MS = 1000;
    mutable std::atomic<uint64_t> _last_delta_warn_ms{0};
    mutable std::atomic<uint64_t> _last_pos_breach_warn_ms{0};
    mutable std::atomic<uint64_t> _last_cost_stale_alert_ms{0};
    mutable std::unordered_map<std::string, uint64_t> _last_soft_warn_ms;

    // B+ 修复(P1-1): 双线程 (数据/交易) 并发访问的软状态 map 均以自旋锁保护。
    // _zombie_halt: setZombieHalt/clearZombieHalts/retainZombieHalts 写,
    //   checkHardPositionRisk 读 (两线程均达)。
    // _last_soft_warn_ms: checkHardPositionRisk/computeInventoryStrategyInputs
    //   读改写 (两线程均达, 告警节流表; 既有竞态, 一并修复)。
    mutable std::atomic_flag _zombie_halt_lock = ATOMIC_FLAG_INIT;
    mutable std::atomic_flag _soft_warn_lock = ATOMIC_FLAG_INIT;
    /// V8-P0-2: 频控时间环自旋锁 -- LockFreeRingBuffer 为 SPSC, 但
    /// recordOrder 存在多生产者 (MdSpi processQuoting/requoteAfterFill、
    /// arb 线程 ArbExecutionBridge), recordTrade/recordCancel 在 TdSpi,
    /// pruneRateWindows 在 MdSpi -- 不加锁即 data race
    mutable std::atomic_flag _rate_lock = ATOMIC_FLAG_INIT;

    // B+: zombie 升级 halt 闩锁 (code -> true)
    std::unordered_map<std::string, bool> _zombie_halt;

    // Event notifier (optional)
    wtp::EventNotifier* _event_notifier = nullptr;

    /// Broadcast risk alert via EventNotifier
    void broadcastAlert(const std::string& alertType, const std::string& message);
};

} // namespace futu
