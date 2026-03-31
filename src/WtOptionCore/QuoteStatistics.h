/*!
 * \file QuoteStatistics.h
 * \brief Quote statistics tracking for market making
 * 
 * Tracks bilateral effective quote time, spread width, and other metrics
 * for compliance and performance monitoring.
 */
#pragma once

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <stdint.h>

namespace wt_option {

struct QuoteSnapshot {
    double bidPrice = 0;
    double askPrice = 0;
    int32_t bidSize = 0;
    int32_t askSize = 0;
    uint64_t timestamp = 0;        // microseconds
    bool isBilateralValid = false; // both bid>0 && ask>0 && sizes>0
    double spread = 0;             // ask - bid
    double spreadPct = 0;          // spread / mid
};

struct QuoteStats {
    // Bilateral effective quoting
    uint64_t totalTimeUs = 0;           // total duration tracked
    uint64_t bilateralQuoteTimeUs = 0;  // time with valid two-sided quote
    double   bilateralQuoteRatio = 0;   // bilateralQuoteTimeUs / totalTimeUs
    
    // Quote width (incremental calculation)
    double   avgSpread = 0;             // average absolute spread
    double   avgSpreadPct = 0;          // average spread as % of mid price
    double   maxSpread = 0;             // widest spread observed
    double   minSpread = 1e9;           // tightest spread observed
    
    // Volume/Count
    uint32_t quoteUpdateCount = 0;      // number of quote updates
    uint32_t bilateralQuoteCount = 0;   // updates that were bilateral
    
    // Internal state for time tracking
    uint64_t lastUpdateTime = 0;
    bool     wasBilateral = false;
    
    void reset();
};

class QuoteStatistics {
public:
    QuoteStatistics();
    
    // Session management
    void onSessionBegin(uint32_t date);
    void onSessionEnd();
    
    // Update logic
    void onQuoteUpdate(const std::string& code, const QuoteSnapshot& snap);
    
    // Access
    const QuoteStats& getStats(const std::string& code) const;
    QuoteStats getStatsForExpiry(uint32_t expiry, const std::vector<std::string>& codes) const;
    
    // Helper to check if a code is tracked
    bool hasStats(const std::string& code) const;

private:
    std::map<std::string, QuoteStats> m_stats;
    
    uint32_t m_currentDate = 0;
};

} // namespace wt_option
