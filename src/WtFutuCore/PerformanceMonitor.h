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

namespace futu {

/// Latency type for tracking different latency paths
enum class LatencyType : uint8_t
{
    TICK_TO_QUOTE,      ///< Tick arrival to quote placement
    ORDER_TO_ACK,       ///< Order sent to acknowledgment
    QUOTE_TO_FILL,      ///< Quote placed to fill
    CANCEL_TO_ACK,      ///< Cancel sent to acknowledgment
    SIGNAL_TO_ORDER     ///< Strategy signal to order sent
};

/// Percentile type for latency statistics
enum class Percentile : uint8_t
{
    P50 = 50,
    P90 = 90,
    P95 = 95,
    P99 = 99,
    P999 = 99 // P99.9 - need to handle differently
};

/// Latency statistics
struct LatencyStats
{
    double min_ns;          ///< Minimum latency (nanoseconds)
    double max_ns;          ///< Maximum latency (nanoseconds)
    double mean_ns;         ///< Mean latency (nanoseconds)
    double std_ns;          ///< Standard deviation (nanoseconds)
    double p50_ns;          ///< 50th percentile
    double p90_ns;          ///< 90th percentile
    double p95_ns;          ///< 95th percentile
    double p99_ns;          ///< 99th percentile
    uint64_t count;         ///< Sample count
    uint64_t total_ns;      ///< Total latency for mean calculation
    
    LatencyStats()
        : min_ns(1e18), max_ns(0), mean_ns(0), std_ns(0)
        , p50_ns(0), p90_ns(0), p95_ns(0), p99_ns(0)
        , count(0), total_ns(0)
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
    uint64_t ticks_processed{0};
    uint64_t quotes_placed{0};
    uint64_t orders_placed{0};
    uint64_t cancels_sent{0};
    uint64_t fills_received{0};
    
    uint64_t last_second_ticks{0};
    uint64_t last_second_quotes{0};
    uint64_t last_second_orders{0};
    uint64_t last_second_cancels{0};
    uint64_t last_second_fills{0};
    
    uint64_t prev_cumulative_ticks{0};
    uint64_t prev_cumulative_quotes{0};
    uint64_t prev_cumulative_orders{0};
    uint64_t prev_cumulative_cancels{0};
    uint64_t prev_cumulative_fills{0};
    
    uint64_t start_time_ms{0};
    uint64_t last_update_ms{0};
    
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
    uint64_t getLatencyThresholdNs() const { return _latency_threshold_ns; }
    
    void setWarnThresholdNs(uint64_t ns) { _warn_threshold_ns = ns; }
    void setCriticalThresholdNs(uint64_t ns) { _critical_threshold_ns = ns; }
    void setLogInterval(uint32_t ms) { _log_interval_ms = ms; }
    uint64_t getWarnThresholdNs() const { return _warn_threshold_ns; }
    uint64_t getCriticalThresholdNs() const { return _critical_threshold_ns; }
    uint32_t getLogInterval() const { return _log_interval_ms; }
    
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
    
    //==========================================================================
    // Throughput Recording
    //==========================================================================
    
    void recordTickProcessed();
    void recordQuotePlaced();
    void recordOrderPlaced();
    void recordCancelSent();
    void recordFillReceived();
    
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
    
    inline bool isLatencyHigh(LatencyType type, uint64_t thresholdNs) const
    {
        uint64_t current = _current_latency_ns[static_cast<int>(type)].load(std::memory_order_relaxed);
        return current > thresholdNs;
    }
    
    inline uint64_t getLastLatencyNs(LatencyType type) const
    {
        return _current_latency_ns[static_cast<int>(type)].load(std::memory_order_relaxed);
    }
    
    //==========================================================================
    // Summary
    //==========================================================================
    
    /// Generate summary string for logging
    std::string getSummary() const;
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void reset();
    void resetLatency(LatencyType type);
    
private:
    bool _enabled = true;
    uint64_t _latency_threshold_ns = 100000;
    uint64_t _warn_threshold_ns = 10000;
    uint64_t _critical_threshold_ns = 50000;
    uint32_t _log_interval_ms = 1000;
    
    // Latency history (using RingBuffer for O(1) operations)
    RingBuffer<uint64_t, LATENCY_HISTORY_SIZE> _tick_to_quote_history;
    RingBuffer<uint64_t, LATENCY_HISTORY_SIZE> _order_to_ack_history;
    RingBuffer<uint64_t, LATENCY_HISTORY_SIZE> _quote_to_fill_history;
    RingBuffer<uint64_t, LATENCY_HISTORY_SIZE> _cancel_to_ack_history;
    RingBuffer<uint64_t, LATENCY_HISTORY_SIZE> _signal_to_order_history;
    
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
        : _monitor(monitor)
        , _type(type)
        , _start(std::chrono::high_resolution_clock::now())
    {}
    
    ~ScopedTimer()
    {
        if (_monitor.isEnabled())
        {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - _start);
            
            switch (_type)
            {
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
