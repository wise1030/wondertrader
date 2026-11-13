/*!
 * \file PerformanceMonitor.h
 * \brief High-Performance Latency and Throughput Monitoring
 *
 * Provides lock-free performance metrics collection for HFT:
 *   - Tick-to-Quote latency (time from tick arrival to order placement)
 *   - Order-to-Ack latency (time from order to acknowledgment)
 *   - Quote-to-Fill latency (time from quote to execution)
 *   - Throughput metrics (ticks/sec, quotes/sec, cancels/sec)
 *
 * Uses RingBuffer for efficient history tracking without allocations.
 * All metrics are collected with nanosecond precision using std::chrono.
 */
#pragma once

#include <string>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include "../Share/RingBuffer.hpp"
#include "../Includes/WTSMarcos.h"

namespace futu
{

/// Latency type for tracking different latency paths
enum class LatencyType : uint8_t
{
    TICK_TO_QUOTE,   ///< Tick arrival to quote placement
    ORDER_TO_ACK,    ///< Order sent to acknowledgment
    QUOTE_TO_FILL,   ///< Quote placed to fill
    CANCEL_TO_ACK,   ///< Cancel sent to acknowledgment
    SIGNAL_TO_ORDER, ///< Strategy signal to order sent
    /// V10-R7e/WS-F: 去大锁验收埋点 —— quoter 自旋锁竞争显性化 (AGENTS 去大锁
    /// 方案 WS-F 验收标准: refreshQuotes 持锁时长 p99 + TdSpi onOrder 因该锁
    /// 阻塞时长; §20.7 起转作 _cb_mtx 临界区健康指标)
    QUOTE_LOCK_HOLD, ///< Md: refreshQuotes 持 quoter 锁全程时长 (含锁内穿越引擎发单)
    ORDER_CB_WAIT    ///< Td: onOrderEvent 中 quoter->onOrder 获取该锁的阻塞时长
};

/// Percentile type for latency statistics
enum class Percentile : uint8_t
{
    P50 = 50,
    P90 = 90,
    P95 = 95,
    P99 = 99
    // V8-R3: P999=99 错误别名已删 (与 P99 同值, 输出会误导)
};

/// Latency statistics
struct LatencyStats
{
    double min_ns;     ///< Minimum latency (nanoseconds)
    double max_ns;     ///< Maximum latency (nanoseconds)
    double mean_ns;    ///< Mean latency (nanoseconds)
    double std_ns;     ///< Standard deviation (nanoseconds)
    double p50_ns;     ///< 50th percentile
    double p90_ns;     ///< 90th percentile
    double p95_ns;     ///< 95th percentile
    double p99_ns;     ///< 99th percentile
    uint64_t count;    ///< Sample count
    uint64_t total_ns; ///< Total latency for mean calculation

    LatencyStats()
        : min_ns(1e18), max_ns(0), mean_ns(0), std_ns(0), p50_ns(0), p90_ns(0), p95_ns(0), p99_ns(0), count(0),
          total_ns(0)
    {}

    /// Reset statistics
    void reset()
    {
        min_ns = 1e18;
        max_ns = 0;
        mean_ns = 0;
        std_ns = 0;
        p50_ns = p90_ns = p95_ns = p99_ns = 0;
        count = 0;
        total_ns = 0;
    }
};

/// Throughput metrics
struct ThroughputStats
{
    // V8-R6 收官: 全部字段原子化 —— record*(Md: tick/quote; Td: fill/order 补挂路径)
    // 双线程写防护(历史去大锁阻断项收编产物, §20.7 起保留为常态保护)。统计口径允许字段间
    // 微撕裂 (监控用途), 单字段不再 torn/lost-update。
    std::atomic<uint64_t> ticks_processed{0};
    std::atomic<uint64_t> quotes_placed{0};
    std::atomic<uint64_t> orders_placed{0};
    std::atomic<uint64_t> cancels_sent{0};
    std::atomic<uint64_t> fills_received{0};

    std::atomic<uint64_t> last_second_ticks{0};
    std::atomic<uint64_t> last_second_quotes{0};
    std::atomic<uint64_t> last_second_orders{0};
    std::atomic<uint64_t> last_second_cancels{0};
    std::atomic<uint64_t> last_second_fills{0};

    std::atomic<uint64_t> prev_cumulative_ticks{0};
    std::atomic<uint64_t> prev_cumulative_quotes{0};
    std::atomic<uint64_t> prev_cumulative_orders{0};
    std::atomic<uint64_t> prev_cumulative_cancels{0};
    std::atomic<uint64_t> prev_cumulative_fills{0};

    std::atomic<uint64_t> start_time_ms{0};
    std::atomic<uint64_t> last_update_ms{0};

    // 原子成员使隐式拷贝失效 —— 显式定义按值快照拷贝 (getThroughputStats 返回值用)
    ThroughputStats() = default;
    ThroughputStats(const ThroughputStats& o) { *this = o; }
    ThroughputStats& operator=(const ThroughputStats& o)
    {
        ticks_processed = o.ticks_processed.load(std::memory_order_relaxed);
        quotes_placed = o.quotes_placed.load(std::memory_order_relaxed);
        orders_placed = o.orders_placed.load(std::memory_order_relaxed);
        cancels_sent = o.cancels_sent.load(std::memory_order_relaxed);
        fills_received = o.fills_received.load(std::memory_order_relaxed);
        last_second_ticks = o.last_second_ticks.load(std::memory_order_relaxed);
        last_second_quotes = o.last_second_quotes.load(std::memory_order_relaxed);
        last_second_orders = o.last_second_orders.load(std::memory_order_relaxed);
        last_second_cancels = o.last_second_cancels.load(std::memory_order_relaxed);
        last_second_fills = o.last_second_fills.load(std::memory_order_relaxed);
        prev_cumulative_ticks = o.prev_cumulative_ticks.load(std::memory_order_relaxed);
        prev_cumulative_quotes = o.prev_cumulative_quotes.load(std::memory_order_relaxed);
        prev_cumulative_orders = o.prev_cumulative_orders.load(std::memory_order_relaxed);
        prev_cumulative_cancels = o.prev_cumulative_cancels.load(std::memory_order_relaxed);
        prev_cumulative_fills = o.prev_cumulative_fills.load(std::memory_order_relaxed);
        start_time_ms = o.start_time_ms.load(std::memory_order_relaxed);
        last_update_ms = o.last_update_ms.load(std::memory_order_relaxed);
        return *this;
    }

    void reset()
    {
        ticks_processed = 0;
        quotes_placed = 0;
        orders_placed = 0;
        cancels_sent = 0;
        fills_received = 0;
        last_second_ticks = 0;
        last_second_quotes = 0;
        last_second_orders = 0;
        last_second_cancels = 0;
        last_second_fills = 0;
        prev_cumulative_ticks = 0;
        prev_cumulative_quotes = 0;
        prev_cumulative_orders = 0;
        prev_cumulative_cancels = 0;
        prev_cumulative_fills = 0;
        start_time_ms = 0;
        last_update_ms = 0;
    }
};

/// Performance Monitor with lock-free counters
class PerformanceMonitor
{
public:
    static constexpr size_t LATENCY_HISTORY_SIZE = 16384;
    static constexpr size_t THROUGHPUT_WINDOW_MS = 1000;

    PerformanceMonitor();
    ~PerformanceMonitor() = default;

    //==========================================================================
    // Configuration
    //==========================================================================

    void setEnabled(bool enabled) { _enabled = enabled; }
    bool isEnabled() const { return _enabled; }

    void setLatencyThresholdNs(uint64_t thresholdNs) { _latency_threshold_ns = thresholdNs; }

    void setWarnThresholdNs(uint64_t ns) { _warn_threshold_ns = ns; }
    void setCriticalThresholdNs(uint64_t ns) { _critical_threshold_ns = ns; }
    void setLogInterval(uint32_t ms) { _log_interval_ms = ms; }
    /// F-core P2(2026-09-17): 同通道告警日志限频 (ms); 0=不限频
    void setAlertThrottleMs(uint32_t ms) { _alert_throttle_ms = ms; }

    /// V8-R3: warn/critical 阈值接线 (此前 set 注入后无任何逻辑读取) --
    /// 全链路(TICK_TO_QUOTE/SIGNAL_TO_ORDER) p99 超阈输出告警,
    /// _log_interval_ms + _alert_throttle_ms 限频; 返回 true = 存在 critical 超限。
    /// 由 StrategyCoordinator 1s 节拍调用 (perf_on 时)。
    /// F-core P2(2026-09-17): 判定样本改用"窗口环"(_win_*), 与 16384 样本累计环解耦 ——
    ///   原实现按累计环算 p99, 越线后每秒重复打印且午休不衰减 (实测 09-17 15448 行/日,
    ///   11:30 与 13:30 的 p99 完全相同 = 2806.2us), 告警淹没真实趋势。
    bool checkThresholds(uint64_t now_ms);

    /// F-core P2(2026-09-17): 结窗评估 —— 用窗口样本算 p99, 累计 EPISODE / 历史最大 p99, 随后清窗口。
    /// 调用点 = 每次输出 60s 汇总之后 (窗口即汇总周期)。
    void evaluateAndResetWindow();

    /// F-core P2(2026-09-17): 巡检 KPI 行 (窗口 p99 + 超限 EPISODE + 历史最大 p99)
    std::string formatKpi() const;

    //==========================================================================
    // Latency Recording (nanosecond precision)
    //==========================================================================

    /// Record tick-to-quote latency
    /// @param latencyNs Latency in nanoseconds
    void recordTickToQuote(uint64_t latencyNs);

    /// Record order-to-ack latency
    void recordOrderToAck(uint64_t latencyNs);

    /// Record quote-to-fill latency
    void recordQuoteToFill(uint64_t latencyNs);

    /// Record cancel-to-ack latency
    void recordCancelToAck(uint64_t latencyNs);

    /// Record signal-to-order latency
    void recordSignalToOrder(uint64_t latencyNs);

    /// V10-R7e/WS-F: refreshQuotes 持锁时长 (Md 单写)
    void recordQuoteLockHold(uint64_t latencyNs);
    /// V10-R7e/WS-F: TdSpi onOrder 取 quoter 锁阻塞时长 (Td 单写)
    void recordOrderCbWait(uint64_t latencyNs);

    //==========================================================================
    // Throughput Recording
    //==========================================================================

    void recordTickProcessed();
    void recordOrderPlaced();
    void recordFillReceived();
    // 19.28: 补接线 — quotes/cancels 计数器自初版起从未被喂养
    void recordQuotePlaced();
    void recordCancelSent();

    //==========================================================================
    // Statistics
    //==========================================================================

    /// Get latency statistics for a specific type
    LatencyStats getLatencyStats(LatencyType type) const;

    /// Get current throughput (per second)
    ThroughputStats getThroughputStats() const;

    /// Update per-second counters (call periodically)
    void updatePerSecondCounters();

    //==========================================================================
    // Latency Quick Checks (inline for hot path)
    //==========================================================================

    //==========================================================================
    // Summary
    //==========================================================================

    /// Generate summary string for logging
    std::string getSummary() const;

    //==========================================================================
    // Reset
    //==========================================================================

    void reset();

private:
    bool _enabled = true;
    uint64_t _latency_threshold_ns = 100000;
    uint64_t _warn_threshold_ns = 10000;
    uint64_t _critical_threshold_ns = 50000;
    uint32_t _log_interval_ms = 1000;
    mutable uint64_t _last_threshold_log_ms = 0; ///< V8-R3: 阈值告警限频

    // F-core P2(2026-09-17): 告警窗口 (仅保留最近一个汇总周期的样本; 结窗清空)
    RingBuffer<uint64_t, LATENCY_HISTORY_SIZE> _win_tick_to_quote;
    RingBuffer<uint64_t, LATENCY_HISTORY_SIZE> _win_signal_to_order;
    uint32_t _alert_throttle_ms = 300000; ///< 同通道日志限频 (默认 5min)
    uint64_t _last_alert_ms[2] = {0, 0};
    uint32_t _crit_episodes[2] = {0, 0}; ///< p99>critical 的窗口计数
    uint32_t _warn_episodes[2] = {0, 0}; ///< p99>warn 的窗口计数
    double _max_p99_ns[2] = {0.0, 0.0};  ///< 历史最大窗口 p99
    /// F-core P2: 最近一个已结窗的 p99/样本数 —— 结窗后窗口已清空, KPI 行必须报这两个值
    ///   (否则 5min 巡检行打印的是空窗口的 0.0us, 掩盖真实窗口读数)
    double _last_win_p99_ns[2] = {0.0, 0.0};
    uint32_t _last_win_count[2] = {0, 0};

    // Latency history (using RingBuffer for O(1) operations)
    RingBuffer<uint64_t, LATENCY_HISTORY_SIZE> _tick_to_quote_history;
    RingBuffer<uint64_t, LATENCY_HISTORY_SIZE> _order_to_ack_history;
    RingBuffer<uint64_t, LATENCY_HISTORY_SIZE> _quote_to_fill_history;
    RingBuffer<uint64_t, LATENCY_HISTORY_SIZE> _cancel_to_ack_history;
    RingBuffer<uint64_t, LATENCY_HISTORY_SIZE> _signal_to_order_history;
    // V10-R7e/WS-F: 去大锁验收埋点 (按类型单写者纪律: QUOTE_LOCK_HOLD=Md, ORDER_CB_WAIT=Td)
    RingBuffer<uint64_t, LATENCY_HISTORY_SIZE> _quote_lock_hold_history;
    RingBuffer<uint64_t, LATENCY_HISTORY_SIZE> _order_cb_wait_history;

    // Current latency (for quick checks)
    std::atomic<uint64_t> _current_latency_ns[5] = {};

    // Throughput counters
    ThroughputStats _throughput;

    // Start time point for latency measurement
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point _start_time;

    /// Calculate statistics from a history buffer
    LatencyStats calculateStats(const RingBuffer<uint64_t, LATENCY_HISTORY_SIZE>& history) const;
};

//==============================================================================
// RAII Timer for latency measurement
//==============================================================================

class ScopedTimer
{
public:
    ScopedTimer(PerformanceMonitor& monitor, LatencyType type)
        : _monitor(monitor), _type(type), _start(std::chrono::high_resolution_clock::now())
    {}

    ~ScopedTimer()
    {
        if (_monitor.isEnabled()) {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - _start);

            switch (_type) {
            case LatencyType::TICK_TO_QUOTE:
                _monitor.recordTickToQuote(duration.count());
                break;
            case LatencyType::ORDER_TO_ACK:
                _monitor.recordOrderToAck(duration.count());
                break;
            case LatencyType::QUOTE_TO_FILL:
                _monitor.recordQuoteToFill(duration.count());
                break;
            case LatencyType::CANCEL_TO_ACK:
                _monitor.recordCancelToAck(duration.count());
                break;
            case LatencyType::SIGNAL_TO_ORDER:
                _monitor.recordSignalToOrder(duration.count());
                break;
            }
        }
    }

private:
    PerformanceMonitor& _monitor;
    LatencyType _type;
    std::chrono::high_resolution_clock::time_point _start;
};

//==============================================================================
// Convenience macros for latency measurement
//==============================================================================

#define PERF_TIMER(monitor, type) ScopedTimer _timer_##__LINE__(monitor, type)
#define PERF_TICK_TO_QUOTE(monitor) PERF_TIMER(monitor, LatencyType::TICK_TO_QUOTE)
#define PERF_ORDER_TO_ACK(monitor) PERF_TIMER(monitor, LatencyType::ORDER_TO_ACK)
#define PERF_QUOTE_TO_FILL(monitor) PERF_TIMER(monitor, LatencyType::QUOTE_TO_FILL)

} // namespace futu
