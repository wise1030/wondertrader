/*!
 * \file QuoteStatistics.h
 * \brief Per-contract quote statistics for option market making
 *
 * Design (borrowed from WtFutuCore/BilateralQuoteStats):
 * - Based on actual posted orders (OQM callbacks), not theoretical quotes
 * - SessionInfo injected: timeToMinutes maps HHMM to session cumulative seconds
 * - Bilateral state switch tracking (enter/exit count)
 * - Obligation check: min_valid_qty depth + max_obligation_spread
 * - Weighted spread: cumulative depth-weighted average price
 * - O(1) update, no hot-path overhead
 *
 * Stats tracked:
 * - Bilateral effective quote time / ratio (双边有效报价时长/占比)
 * - Spread width (加权价差, tick为单位)
 * - Order/fill/cancel/reject counts + ratios
 * - Quote latency (tick -> order confirmed)
 * - Bilateral switch count (双边状态切换次数)
 */
#pragma once

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <cstdint>
#include <cmath>

namespace wtp { class WTSSessionInfo; }

namespace wt_option {

/// Per-contract quote stats config
struct QuoteStatsConfig {
    double  min_valid_qty = 1.0;           ///< 累计深度阈值(手),做市义务最小量
    double  max_obligation_spread = 50.0;  ///< 做市义务最大宽度(tick数)
    bool    enable = true;

    QuoteStatsConfig() = default;
};

/// Per-contract stats result
struct QuoteStatsResult {
    // Bilateral effective quoting
    uint64_t bilateralTimeSec = 0;        ///< 双边有效报价累计时间(秒)
    uint64_t sessionTotalSec = 0;          ///< session总交易时间(秒)
    double   bilateralRatio = 0;          ///< 双边占比 [0,1]
    uint32_t bilateralSwitchCount = 0;    ///< 双边状态切换次数

    // Spread (tick units)
    double   totalSpreadTicks = 0;          ///< 总价差(用于加权平均)
    double   avgSpreadTicks = 0;           ///< 加权平均价差(tick)
    uint64_t spreadSampleCount = 0;       ///< 价差样本数

    // Order lifecycle
    uint32_t ordersSent = 0;
    uint32_t fills = 0;
    uint32_t cancels = 0;
    uint32_t rejects = 0;
    double   fillRatio = 0;               ///< fills / ordersSent
    double   cancelRatio = 0;             ///< cancels / ordersSent

    // Quote latency (microseconds)
    uint64_t avgLatencyUs = 0;
    uint64_t maxLatencyUs = 0;
    uint64_t minLatencyUs = (uint64_t)-1;
    uint32_t latencySampleCount = 0;

    // Time at best
    uint64_t timeAtBestSec = 0;
    double   timeAtBestRatio = 0;

    QuoteStatsResult() = default;
};

/// Per-contract quote statistics (borrowed from BilateralQuoteStats)
class QuoteStats {
public:
    QuoteStats() = default;

    void setConfig(const QuoteStatsConfig& cfg) { _cfg = cfg; }
    const QuoteStatsConfig& getConfig() const { return _cfg; }

    /// Inject session info (required for time tracking)
    bool setSessionInfo(wtp::WTSSessionInfo* sessInfo, const char* codeForLog = "");

    /// Session start: reset all accumulators
    void onSessionStart();

    /// Session end: flush remaining bilateral time
    void onSessionEnd(uint32_t uTimeHHMM, uint32_t secInMin);

    /// Update from actual posted orders (called from OQM callback)
    /// @param bidPrice best active bid price (0 if none)
    /// @param bidQty total remaining bid qty at best
    /// @param askPrice best active ask price (0 if none)
    /// @param askQty total remaining ask qty at best
    /// @param tickSize tick size for spread calculation
    /// @param uTimeHHMM current time HHMM
    /// @param secInMin seconds within minute [0,59]
    /// @param isAtBest whether our quote is at market best
    void update(double bidPrice, double bidQty,
                double askPrice, double askQty,
                double tickSize,
                uint32_t uTimeHHMM, uint32_t secInMin,
                bool isAtBest);

    /// Order lifecycle events (from OQM callbacks)
    void onOrderSent();
    void onFill();
    void onCancel();
    void onReject();

    /// Quote latency (tick -> order confirmed)
    void onQuoteLatency(uint64_t latencyUs);

    /// Get result
    QuoteStatsResult getResult() const;

    /// Format for logging
    std::string formatString() const;

    bool isBilateral() const { return _isBilateral; }
    bool hasSessionInfo() const { return _sessionInfo != nullptr; }

private:
    static constexpr uint64_t INVALID_UNITS = (uint64_t)-1;

    /// Map (HHMM, sec) to session cumulative seconds (monotonic)
    uint64_t computeSessionSeconds(uint32_t uTimeHHMM, uint32_t secInMin) const;

    /// Check if posted orders meet bilateral obligation
    bool checkBilateral(double bidPrice, double bidQty,
                         double askPrice, double askQty,
                         double tickSize) const;

    QuoteStatsConfig _cfg;
    wtp::WTSSessionInfo* _sessionInfo = nullptr;

    // Time tracking (unit: session cumulative seconds)
    uint64_t _lastSessionSec = 0;
    uint64_t _bilateralStartSec = 0;
    uint64_t _totalBilateralSec = 0;
    uint64_t _sessionTotalSec = 0;
    uint64_t _timeAtBestSec = 0;
    uint64_t _bestStartSec = 0;

    bool _isBilateral = false;
    bool _isAtBest = false;
    uint32_t _bilateralSwitchCount = 0;

    // Spread samples (tick units)
    double _totalSpreadTicks = 0;
    uint64_t _spreadSampleCount = 0;

    // Order lifecycle
    uint32_t _ordersSent = 0;
    uint32_t _fills = 0;
    uint32_t _cancels = 0;
    uint32_t _rejects = 0;

    // Latency
    uint64_t _latencySumUs = 0;
    uint64_t _maxLatencyUs = 0;
    uint64_t _minLatencyUs = (uint64_t)-1;
    uint32_t _latencySampleCount = 0;
};

/// Session-level quote statistics manager (per-contract)
class QuoteStatistics {
public:
    QuoteStatistics();

    /// Set config for all contracts
    void setConfig(const QuoteStatsConfig& cfg) { _cfg = cfg; }

    /// Inject session info
    void setSessionInfo(wtp::WTSSessionInfo* sessInfo);

    /// Session management
    void onSessionBegin(uint32_t date);
    void onSessionEnd();

    /// Get or create per-contract stats
    QuoteStats& getOrCreate(const std::string& code);
    const QuoteStats* get(const std::string& code) const;

    /// Order lifecycle events (routed from OQM callbacks)
    void onOrderSent(const std::string& code);
    void onFill(const std::string& code);
    void onCancel(const std::string& code);
    void onReject(const std::string& code);
    void onQuoteLatency(const std::string& code, uint64_t latencyUs);

    /// Update posted market (from OQM callback, not hot path)
    void onPostedMarket(const std::string& code,
                         double bidPrice, double bidQty,
                         double askPrice, double askQty,
                         double tickSize,
                         uint32_t uTimeHHMM, uint32_t secInMin,
                         bool isAtBest);

    /// Access
    bool hasStats(const std::string& code) const;
    QuoteStatsResult getStats(const std::string& code) const;
    QuoteStatsResult getAggregate(const std::vector<std::string>& codes) const;

    /// Session summary
    struct SessionSummary {
        uint32_t date = 0;
        uint32_t totalCodes = 0;
        double avgBilateralRatio = 0;
        double avgSpreadTicks = 0;
        double avgFillRatio = 0;
        double avgCancelRatio = 0;
        double avgLatencyUs = 0;
        double avgTimeAtBestRatio = 0;
        uint32_t totalOrders = 0;
        uint32_t totalFills = 0;
        uint32_t totalCancels = 0;
        uint32_t totalRejects = 0;
        uint32_t totalSwitches = 0;
    };
    SessionSummary getSessionSummary() const;

private:
    std::map<std::string, QuoteStats> m_stats;
    QuoteStatsConfig _cfg;
    wtp::WTSSessionInfo* _sessionInfo = nullptr;
    uint32_t m_currentDate = 0;
    uint32_t m_lastTimeHHMM = 0;
    uint32_t m_lastSecInMin = 0;
};

using QuoteStatisticsPtr = std::shared_ptr<QuoteStatistics>;

} // namespace wt_option
