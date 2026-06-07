/*!
 * \file PerformanceMonitor.cpp
 * \brief High-Performance Latency and Throughput Monitoring Implementation
 */
#include "PerformanceMonitor.h"
#include "../Share/TimeUtils.hpp"
#include "../WTSTools/WTSLogger.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace futu {

PerformanceMonitor::PerformanceMonitor()
    : _enabled(true)
    , _start_time(Clock::now())
{
}

void PerformanceMonitor::recordTickToQuote(uint64_t latencyNs)
{
    if (!_enabled) return;
    
    _tick_to_quote_history.push(latencyNs);
    _current_latency_ns[static_cast<int>(LatencyType::TICK_TO_QUOTE)].store(
        latencyNs, std::memory_order_relaxed);
    
    // Warn if latency exceeds threshold (100 microseconds = 100,000 ns)
    if (latencyNs > 100000)
    {
        WTSLogger::warn("[PERF] High tick-to-quote latency: {} us", latencyNs / 1000.0);
    }
}

void PerformanceMonitor::recordOrderToAck(uint64_t latencyNs)
{
    if (!_enabled) return;
    
    _order_to_ack_history.push(latencyNs);
    _current_latency_ns[static_cast<int>(LatencyType::ORDER_TO_ACK)].store(
        latencyNs, std::memory_order_relaxed);
}

void PerformanceMonitor::recordQuoteToFill(uint64_t latencyNs)
{
    if (!_enabled) return;
    
    _quote_to_fill_history.push(latencyNs);
    _current_latency_ns[static_cast<int>(LatencyType::QUOTE_TO_FILL)].store(
        latencyNs, std::memory_order_relaxed);
}

void PerformanceMonitor::recordCancelToAck(uint64_t latencyNs)
{
    if (!_enabled) return;
    
    _cancel_to_ack_history.push(latencyNs);
    _current_latency_ns[static_cast<int>(LatencyType::CANCEL_TO_ACK)].store(
        latencyNs, std::memory_order_relaxed);
}

void PerformanceMonitor::recordSignalToOrder(uint64_t latencyNs)
{
    if (!_enabled) return;
    
    _signal_to_order_history.push(latencyNs);
    _current_latency_ns[static_cast<int>(LatencyType::SIGNAL_TO_ORDER)].store(
        latencyNs, std::memory_order_relaxed);
}

void PerformanceMonitor::recordTickProcessed()
{
    ++_throughput.ticks_processed;
}

void PerformanceMonitor::recordQuotePlaced()
{
    ++_throughput.quotes_placed;
}

void PerformanceMonitor::recordOrderPlaced()
{
    ++_throughput.orders_placed;
}

void PerformanceMonitor::recordCancelSent()
{
    ++_throughput.cancels_sent;
}

void PerformanceMonitor::recordFillReceived()
{
    ++_throughput.fills_received;
}

LatencyStats PerformanceMonitor::calculateStats(const RingBuffer<uint64_t, LATENCY_HISTORY_SIZE>& history) const
{
    LatencyStats stats;
    
    size_t size = history.size();
    if (size == 0)
        return stats;
    
    // Copy data for sorting (for percentile calculation)
    std::vector<uint64_t> sorted_data;
    sorted_data.reserve(size);
    
    for (size_t i = 0; i < size; ++i)
    {
        uint64_t val = history[i];
        sorted_data.push_back(val);
        stats.total_ns += val;
        
        if (val < stats.min_ns) stats.min_ns = val;
        if (val > stats.max_ns) stats.max_ns = val;
    }
    
    stats.count = size;
    stats.mean_ns = static_cast<double>(stats.total_ns) / size;
    
    // Sort for percentiles
    std::sort(sorted_data.begin(), sorted_data.end());
    
    // Calculate percentiles
    auto percentile = [&sorted_data, size](double p) -> double {
        if (size == 1) return static_cast<double>(sorted_data[0]);
        double index = (p / 100.0) * (size - 1);
        size_t lower = static_cast<size_t>(std::floor(index));
        size_t upper = static_cast<size_t>(std::ceil(index));
        double frac = index - lower;
        return sorted_data[lower] * (1 - frac) + sorted_data[upper] * frac;
    };
    
    stats.p50_ns = percentile(50);
    stats.p90_ns = percentile(90);
    stats.p95_ns = percentile(95);
    stats.p99_ns = percentile(99);
    
    // Calculate standard deviation
    double variance = 0;
    for (uint64_t val : sorted_data)
    {
        double diff = static_cast<double>(val) - stats.mean_ns;
        variance += diff * diff;
    }
    stats.std_ns = std::sqrt(variance / size);
    
    return stats;
}

LatencyStats PerformanceMonitor::getLatencyStats(LatencyType type) const
{
    switch (type)
    {
        case LatencyType::TICK_TO_QUOTE:
            return calculateStats(_tick_to_quote_history);
        case LatencyType::ORDER_TO_ACK:
            return calculateStats(_order_to_ack_history);
        case LatencyType::QUOTE_TO_FILL:
            return calculateStats(_quote_to_fill_history);
        case LatencyType::CANCEL_TO_ACK:
            return calculateStats(_cancel_to_ack_history);
        case LatencyType::SIGNAL_TO_ORDER:
            return calculateStats(_signal_to_order_history);
        default:
            return LatencyStats();
    }
}

ThroughputStats PerformanceMonitor::getThroughputStats() const
{
    ThroughputStats stats;
    stats.ticks_processed = _throughput.ticks_processed;
    stats.quotes_placed = _throughput.quotes_placed;
    stats.orders_placed = _throughput.orders_placed;
    stats.cancels_sent = _throughput.cancels_sent;
    stats.fills_received = _throughput.fills_received;
    stats.last_second_ticks = _throughput.last_second_ticks;
    stats.last_second_quotes = _throughput.last_second_quotes;
    stats.last_second_orders = _throughput.last_second_orders;
    stats.last_second_cancels = _throughput.last_second_cancels;
    stats.last_second_fills = _throughput.last_second_fills;
    return stats;
}

void PerformanceMonitor::updatePerSecondCounters()
{
    uint64_t current_ticks = _throughput.ticks_processed;
    uint64_t current_quotes = _throughput.quotes_placed;
    uint64_t current_orders = _throughput.orders_placed;
    uint64_t current_cancels = _throughput.cancels_sent;
    uint64_t current_fills = _throughput.fills_received;
    
    uint64_t prev_ticks = _throughput.prev_cumulative_ticks;
    uint64_t prev_quotes = _throughput.prev_cumulative_quotes;
    uint64_t prev_orders = _throughput.prev_cumulative_orders;
    uint64_t prev_cancels = _throughput.prev_cumulative_cancels;
    uint64_t prev_fills = _throughput.prev_cumulative_fills;
    
    _throughput.last_second_ticks = current_ticks - prev_ticks;
    _throughput.last_second_quotes = current_quotes - prev_quotes;
    _throughput.last_second_orders = current_orders - prev_orders;
    _throughput.last_second_cancels = current_cancels - prev_cancels;
    _throughput.last_second_fills = current_fills - prev_fills;
    
    _throughput.prev_cumulative_ticks = current_ticks;
    _throughput.prev_cumulative_quotes = current_quotes;
    _throughput.prev_cumulative_orders = current_orders;
    _throughput.prev_cumulative_cancels = current_cancels;
    _throughput.prev_cumulative_fills = current_fills;
}

std::string PerformanceMonitor::getSummary() const
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    
    oss << "=== Performance Summary ===\n";
    
    // Tick-to-Quote latency
    auto t2q = getLatencyStats(LatencyType::TICK_TO_QUOTE);
    if (t2q.count > 0)
    {
        oss << "Tick-to-Quote: ";
        oss << "mean=" << t2q.mean_ns / 1000.0 << "us, ";
        oss << "p99=" << t2q.p99_ns / 1000.0 << "us, ";
        oss << "max=" << t2q.max_ns / 1000.0 << "us ";
        oss << "(n=" << t2q.count << ")\n";
    }
    
    // Order-to-Ack latency
    auto o2a = getLatencyStats(LatencyType::ORDER_TO_ACK);
    if (o2a.count > 0)
    {
        oss << "Order-to-Ack:  ";
        oss << "mean=" << o2a.mean_ns / 1000.0 << "us, ";
        oss << "p99=" << o2a.p99_ns / 1000.0 << "us, ";
        oss << "max=" << o2a.max_ns / 1000.0 << "us ";
        oss << "(n=" << o2a.count << ")\n";
    }
    
    // Cancel-to-Ack latency
    auto c2a = getLatencyStats(LatencyType::CANCEL_TO_ACK);
    if (c2a.count > 0)
    {
        oss << "Cancel-to-Ack: ";
        oss << "mean=" << c2a.mean_ns / 1000.0 << "us, ";
        oss << "p99=" << c2a.p99_ns / 1000.0 << "us, ";
        oss << "max=" << c2a.max_ns / 1000.0 << "us ";
        oss << "(n=" << c2a.count << ")\n";
    }
    
    // Throughput
    auto tp = getThroughputStats();
    oss << "Throughput: ";
    oss << "ticks=" << tp.last_second_ticks << "/s, ";
    oss << "quotes=" << tp.last_second_quotes << "/s, ";
    oss << "orders=" << tp.last_second_orders << "/s, ";
    oss << "cancels=" << tp.last_second_cancels << "/s, ";
    oss << "fills=" << tp.last_second_fills << "/s\n";
    
    oss << "Cumulative: ";
    oss << "ticks=" << tp.ticks_processed << ", ";
    oss << "quotes=" << tp.quotes_placed << ", ";
    oss << "fills=" << tp.fills_received << "\n";
    
    return oss.str();
}

void PerformanceMonitor::reset()
{
    _tick_to_quote_history.clear();
    _order_to_ack_history.clear();
    _quote_to_fill_history.clear();
    _cancel_to_ack_history.clear();
    _signal_to_order_history.clear();
    
    for (int i = 0; i < 5; ++i)
    {
        _current_latency_ns[i].store(0, std::memory_order_relaxed);
    }
    
    _throughput.reset();
    _start_time = Clock::now();
}

void PerformanceMonitor::resetLatency(LatencyType type)
{
    switch (type)
    {
        case LatencyType::TICK_TO_QUOTE:
            _tick_to_quote_history.clear();
            break;
        case LatencyType::ORDER_TO_ACK:
            _order_to_ack_history.clear();
            break;
        case LatencyType::QUOTE_TO_FILL:
            _quote_to_fill_history.clear();
            break;
        case LatencyType::CANCEL_TO_ACK:
            _cancel_to_ack_history.clear();
            break;
        case LatencyType::SIGNAL_TO_ORDER:
            _signal_to_order_history.clear();
            break;
    }
    
    _current_latency_ns[static_cast<int>(type)].store(0, std::memory_order_relaxed);
}

} // namespace futu
