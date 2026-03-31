/*!
 * \file QuoteStatistics.cpp
 * \brief Quote statistics implementation
 */
#include "QuoteStatistics.h"
#include <algorithm>

namespace wt_option {

void QuoteStats::reset() {
    totalTimeUs = 0;
    bilateralQuoteTimeUs = 0;
    bilateralQuoteRatio = 0;
    
    avgSpread = 0;
    avgSpreadPct = 0;
    maxSpread = 0;
    minSpread = 1e9;
    
    quoteUpdateCount = 0;
    bilateralQuoteCount = 0;
    
    lastUpdateTime = 0;
    wasBilateral = false;
}

QuoteStatistics::QuoteStatistics() : m_currentDate(0) {}

void QuoteStatistics::onSessionBegin(uint32_t date) {
    m_currentDate = date;
    for (auto& pair : m_stats) {
        pair.second.reset();
    }
}

void QuoteStatistics::onSessionEnd() {
    // Optional: Finalize stats if needed
}

void QuoteStatistics::onQuoteUpdate(const std::string& code, const QuoteSnapshot& snap) {
    
    QuoteStats& stats = m_stats[code];
    
    // First update initialization
    if (stats.lastUpdateTime == 0) {
        stats.lastUpdateTime = snap.timestamp;
        stats.wasBilateral = snap.isBilateralValid;
        
        // Initial snapshot counts towards width stats if valid
        if (snap.isBilateralValid) {
            stats.avgSpread = snap.spread;
            stats.avgSpreadPct = snap.spreadPct;
            stats.minSpread = snap.spread;
            stats.maxSpread = snap.spread;
            stats.bilateralQuoteCount = 1;
        }
        stats.quoteUpdateCount = 1;
        return;
    }
    
    // Calculate elapsed time
    if (snap.timestamp > stats.lastUpdateTime) {
        uint64_t elapsed = snap.timestamp - stats.lastUpdateTime;
        stats.totalTimeUs += elapsed;
        
        if (stats.wasBilateral) {
            stats.bilateralQuoteTimeUs += elapsed;
        }
    }
    
    // Update ratio
    if (stats.totalTimeUs > 0) {
        stats.bilateralQuoteRatio = (double)stats.bilateralQuoteTimeUs / stats.totalTimeUs;
    }
    
    // Update spread statistics (incremental average)
    if (snap.isBilateralValid) {
        stats.bilateralQuoteCount++;
        
        // Cumulative moving average: new_avg = old_avg + (new_val - old_avg) / n
        double n = (double)stats.bilateralQuoteCount;
        stats.avgSpread += (snap.spread - stats.avgSpread) / n;
        stats.avgSpreadPct += (snap.spreadPct - stats.avgSpreadPct) / n;
        
        stats.maxSpread = std::max(stats.maxSpread, snap.spread);
        stats.minSpread = std::min(stats.minSpread, snap.spread);
    }
    
    stats.quoteUpdateCount++;
    stats.lastUpdateTime = snap.timestamp;
    stats.wasBilateral = snap.isBilateralValid;
}

const QuoteStats& QuoteStatistics::getStats(const std::string& code) const {
    // Avoid creating entry if not exists (const method)
    // Use static empty stats
    static QuoteStats emptyStats;
    
    // m_mutex is mutable
    auto it = m_stats.find(code);
    if (it != m_stats.end()) {
        return it->second;
    }
    return emptyStats;
}

bool QuoteStatistics::hasStats(const std::string& code) const {
    return m_stats.find(code) != m_stats.end();
}

QuoteStats QuoteStatistics::getStatsForExpiry(uint32_t expiry, const std::vector<std::string>& codes) const {
    QuoteStats aggStats;
    
    int count = 0;
    
    for (const auto& code : codes) {
        auto it = m_stats.find(code);
        if (it == m_stats.end()) continue;
        
        const auto& s = it->second;
        if (s.quoteUpdateCount == 0) continue;
        
        // Aggregate time-weighted & event-weighted metrics
        // This is a simple average aggregation across options for "Average Quote Quality"
        aggStats.totalTimeUs += s.totalTimeUs; // Sum or Avg? Usually we want average ratio
        aggStats.bilateralQuoteTimeUs += s.bilateralQuoteTimeUs;
        
        aggStats.avgSpread += s.avgSpread;
        aggStats.avgSpreadPct += s.avgSpreadPct;
        
        count++;
    }
    
    if (count > 0) {
        // Average the accumulated stats
        // Note: For totalTimeUs, if we sum specific times, the ratio is sum(valid)/sum(total)
        // Calculating ratio based on aggregated times
        if (aggStats.totalTimeUs > 0)
            aggStats.bilateralQuoteRatio = (double)aggStats.bilateralQuoteTimeUs / aggStats.totalTimeUs;
            
        aggStats.avgSpread /= count;
        aggStats.avgSpreadPct /= count;
    }
    
    return aggStats;
}

} // namespace wt_option
