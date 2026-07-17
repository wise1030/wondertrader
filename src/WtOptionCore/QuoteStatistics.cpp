/*!
 * \file QuoteStatistics.cpp
 * \brief Per-contract quote statistics implementation
 *
 * Borrowed from WtFutuCore/BilateralQuoteStats:
 * - SessionInfo-based time tracking (timeToMinutes)
 * - Bilateral state switch counting
 * - Obligation check (min_valid_qty + max_obligation_spread)
 * - Weighted spread (tick units)
 * - All updates from OQM callbacks (no hot-path overhead)
 */
#include "QuoteStatistics.h"
#include "../Includes/WTSSessionInfo.hpp"
#include "../WTSTools/WTSLogger.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wt_option {

// ============================================================================
// QuoteStats (per-contract)
// ============================================================================

bool QuoteStats::setSessionInfo(wtp::WTSSessionInfo* sessInfo, const char* codeForLog) {
    _sessionInfo = sessInfo;
    if (!sessInfo) {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "QuoteStats: {} setSessionInfo nullptr, stats DISABLED",
            codeForLog ? codeForLog : "");
        return false;
    }

    // Pre-compute session total seconds
    const auto& sections = sessInfo->getTradingSections();
    uint64_t totalMin = 0;
    for (const auto& sec : sections) {
        uint32_t startMin = (sec.first / 100) * 60 + (sec.first % 100);
        uint32_t endMin   = (sec.second / 100) * 60 + (sec.second % 100);
        if (endMin >= startMin)
            totalMin += (endMin - startMin);
        else
            totalMin += (1440 - startMin + endMin);  // cross midnight
    }
    _sessionTotalSec = totalMin * 60;

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "QuoteStats: {} session total={}min ({} sections)",
        codeForLog ? codeForLog : "", totalMin, sections.size());
    return true;
}

void QuoteStats::onSessionStart() {
    _lastSessionSec = 0;
    _bilateralStartSec = 0;
    _totalBilateralSec = 0;
    _timeAtBestSec = 0;
    _bestStartSec = 0;
    _isBilateral = false;
    _isAtBest = false;
    _bilateralSwitchCount = 0;
    _totalSpreadTicks = 0;
    _spreadSampleCount = 0;
    _ordersSent = 0;
    _fills = 0;
    _cancels = 0;
    _rejects = 0;
    _latencySumUs = 0;
    _maxLatencyUs = 0;
    _minLatencyUs = (uint64_t)-1;
    _latencySampleCount = 0;
}

void QuoteStats::onSessionEnd(uint32_t uTimeHHMM, uint32_t secInMin) {
    if (!_sessionInfo) return;

    uint64_t nowSec = computeSessionSeconds(uTimeHHMM, secInMin);
    if (nowSec == INVALID_UNITS) return;

    // Flush remaining bilateral time
    if (_isBilateral && _bilateralStartSec > 0 && nowSec >= _bilateralStartSec) {
        _totalBilateralSec += (nowSec - _bilateralStartSec);
        _bilateralStartSec = 0;
    }
    // Flush remaining at-best time
    if (_isAtBest && _bestStartSec > 0 && nowSec >= _bestStartSec) {
        _timeAtBestSec += (nowSec - _bestStartSec);
        _bestStartSec = 0;
    }
    _isBilateral = false;
    _isAtBest = false;
}

uint64_t QuoteStats::computeSessionSeconds(uint32_t uTimeHHMM, uint32_t secInMin) const {
    if (!_sessionInfo) return INVALID_UNITS;
    uint32_t mins = _sessionInfo->timeToMinutes(uTimeHHMM, false);
    if (mins == INVALID_UINT32) return INVALID_UNITS;
    return (uint64_t)mins * 60 + (secInMin < 60 ? secInMin : 59);
}

bool QuoteStats::checkBilateral(double bidPrice, double bidQty,
                                 double askPrice, double askQty,
                                 double tickSize) const {
    // Both sides must have sufficient quantity
    if (bidQty < _cfg.min_valid_qty || askQty < _cfg.min_valid_qty)
        return false;
    if (bidPrice <= 0 || askPrice <= 0 || tickSize <= 0)
        return false;
    // Spread must be within obligation
    double spreadTicks = (askPrice - bidPrice) / tickSize;
    if (spreadTicks <= 0 || spreadTicks > _cfg.max_obligation_spread)
        return false;
    return true;
}

void QuoteStats::update(double bidPrice, double bidQty,
                         double askPrice, double askQty,
                         double tickSize,
                         uint32_t uTimeHHMM, uint32_t secInMin,
                         bool isAtBest) {
    if (!_sessionInfo || !_cfg.enable) return;

    uint64_t nowSec = computeSessionSeconds(uTimeHHMM, secInMin);
    if (nowSec == INVALID_UNITS) return;  // outside trading hours

    bool newBilateral = checkBilateral(bidPrice, bidQty, askPrice, askQty, tickSize);

    // Bilateral state transition
    if (newBilateral && !_isBilateral) {
        // Enter bilateral
        _bilateralStartSec = nowSec;
        _bilateralSwitchCount++;
    } else if (!newBilateral && _isBilateral && _bilateralStartSec > 0
               && nowSec >= _bilateralStartSec) {
        // Exit bilateral: accumulate time
        _totalBilateralSec += (nowSec - _bilateralStartSec);
        _bilateralStartSec = 0;
    }

    _isBilateral = newBilateral;
    _lastSessionSec = nowSec;

    // At-best state transition
    if (isAtBest && !_isAtBest) {
        _bestStartSec = nowSec;
    } else if (!isAtBest && _isAtBest && _bestStartSec > 0
               && nowSec >= _bestStartSec) {
        _timeAtBestSec += (nowSec - _bestStartSec);
        _bestStartSec = 0;
    }
    _isAtBest = isAtBest;

    // Spread sample (only when bilateral)
    if (newBilateral && tickSize > 0) {
        double spreadTicks = (askPrice - bidPrice) / tickSize;
        if (spreadTicks > 0) {
            _totalSpreadTicks += spreadTicks;
            _spreadSampleCount++;
        }
    }
}

void QuoteStats::onOrderSent() { _ordersSent++; }
void QuoteStats::onFill()      { _fills++; }
void QuoteStats::onCancel()    { _cancels++; }
void QuoteStats::onReject()    { _rejects++; }

void QuoteStats::onQuoteLatency(uint64_t latencyUs) {
    if (latencyUs == 0) return;
    _latencySumUs += latencyUs;
    _latencySampleCount++;
    if (latencyUs > _maxLatencyUs) _maxLatencyUs = latencyUs;
    if (latencyUs < _minLatencyUs) _minLatencyUs = latencyUs;
}

QuoteStatsResult QuoteStats::getResult() const {
    QuoteStatsResult r;

    r.bilateralTimeSec = _totalBilateralSec;
    r.sessionTotalSec = _sessionTotalSec;
    r.bilateralRatio = (_sessionTotalSec > 0)
        ? (double)_totalBilateralSec / _sessionTotalSec : 0.0;

    r.avgSpreadTicks = (_spreadSampleCount > 0)
        ? _totalSpreadTicks / _spreadSampleCount : 0.0;
    r.spreadSampleCount = _spreadSampleCount;
    r.bilateralSwitchCount = _bilateralSwitchCount;

    r.ordersSent = _ordersSent;
    r.fills = _fills;
    r.cancels = _cancels;
    r.rejects = _rejects;
    r.fillRatio = (_ordersSent > 0) ? (double)_fills / _ordersSent : 0.0;
    r.cancelRatio = (_ordersSent > 0) ? (double)_cancels / _ordersSent : 0.0;

    r.avgLatencyUs = (_latencySampleCount > 0)
        ? _latencySumUs / _latencySampleCount : 0;
    r.maxLatencyUs = _maxLatencyUs;
    r.minLatencyUs = (_latencySampleCount > 0) ? _minLatencyUs : 0;
    r.latencySampleCount = _latencySampleCount;

    r.timeAtBestSec = _timeAtBestSec;
    r.timeAtBestRatio = (_sessionTotalSec > 0)
        ? (double)_timeAtBestSec / _sessionTotalSec : 0.0;

    return r;
}

std::string QuoteStats::formatString() const {
    auto r = getResult();
    char buf[512];
    snprintf(buf, sizeof(buf),
        "bilateral=%lus/%lus ratio=%.1f%% switches=%u "
        "spread=%.2fticks(smpl=%lu) "
        "orders=%u fills=%u cancels=%u rejects=%u "
        "fillRatio=%.1f%% cancelRatio=%.1f%% "
        "latency avg=%luus max=%luus "
        "atBest=%.1f%%",
        (unsigned long)r.bilateralTimeSec,
        (unsigned long)r.sessionTotalSec,
        r.bilateralRatio * 100.0,
        r.bilateralSwitchCount,
        r.avgSpreadTicks,
        (unsigned long)r.spreadSampleCount,
        r.ordersSent, r.fills, r.cancels, r.rejects,
        r.fillRatio * 100.0, r.cancelRatio * 100.0,
        (unsigned long)r.avgLatencyUs,
        (unsigned long)r.maxLatencyUs,
        r.timeAtBestRatio * 100.0);
    return std::string(buf);
}

// ============================================================================
// QuoteStatistics (session-level manager)
// ============================================================================

QuoteStatistics::QuoteStatistics() {}

void QuoteStatistics::setSessionInfo(wtp::WTSSessionInfo* sessInfo) {
    _sessionInfo = sessInfo;
}

void QuoteStatistics::onSessionBegin(uint32_t date) {
    m_currentDate = date;
    for (auto& pair : m_stats) {
        pair.second.onSessionStart();
    }
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "QuoteStatistics: session begin date={}", date);
}

void QuoteStatistics::onSessionEnd() {
    for (auto& pair : m_stats) {
        pair.second.onSessionEnd(m_lastTimeHHMM, m_lastSecInMin);
    }

    auto summary = getSessionSummary();
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "QuoteStatistics session {} summary: codes={} bilateralRatio={:.1f}% "
        "spread={:.2f}ticks fillRatio={:.1f}% cancelRatio={:.1f}% "
        "avgLatency={}us orders={} fills={} cancels={} rejects={} switches={}",
        summary.date, summary.totalCodes, summary.avgBilateralRatio * 100,
        summary.avgSpreadTicks, summary.avgFillRatio * 100,
        summary.avgCancelRatio * 100, (uint64_t)summary.avgLatencyUs,
        summary.totalOrders, summary.totalFills, summary.totalCancels,
        summary.totalRejects, summary.totalSwitches);
}

QuoteStats& QuoteStatistics::getOrCreate(const std::string& code) {
    auto& s = m_stats[code];
    if (!s.hasSessionInfo() && _sessionInfo) {
        s.setSessionInfo(_sessionInfo, code.c_str());
        s.setConfig(_cfg);
    }
    return s;
}

const QuoteStats* QuoteStatistics::get(const std::string& code) const {
    auto it = m_stats.find(code);
    return (it != m_stats.end()) ? &it->second : nullptr;
}

void QuoteStatistics::onOrderSent(const std::string& code) {
    getOrCreate(code).onOrderSent();
}

void QuoteStatistics::onFill(const std::string& code) {
    getOrCreate(code).onFill();
}

void QuoteStatistics::onCancel(const std::string& code) {
    getOrCreate(code).onCancel();
}

void QuoteStatistics::onReject(const std::string& code) {
    getOrCreate(code).onReject();
}

void QuoteStatistics::onQuoteLatency(const std::string& code, uint64_t latencyUs) {
    getOrCreate(code).onQuoteLatency(latencyUs);
}

void QuoteStatistics::onPostedMarket(const std::string& code,
    double bidPrice, double bidQty, double askPrice, double askQty,
    double tickSize, uint32_t uTimeHHMM, uint32_t secInMin, bool isAtBest) {
    m_lastTimeHHMM = uTimeHHMM;
    m_lastSecInMin = secInMin;
    getOrCreate(code).update(bidPrice, bidQty, askPrice, askQty,
                              tickSize, uTimeHHMM, secInMin, isAtBest);
}

bool QuoteStatistics::hasStats(const std::string& code) const {
    return m_stats.find(code) != m_stats.end();
}

QuoteStatsResult QuoteStatistics::getStats(const std::string& code) const {
    auto it = m_stats.find(code);
    if (it != m_stats.end()) return it->second.getResult();
    return QuoteStatsResult();
}

QuoteStatsResult QuoteStatistics::getAggregate(const std::vector<std::string>& codes) const {
    QuoteStatsResult agg;
    int count = 0;

    for (const auto& code : codes) {
        auto it = m_stats.find(code);
        if (it == m_stats.end()) continue;

        auto s = it->second.getResult();
        if (s.sessionTotalSec == 0) continue;

        agg.bilateralTimeSec += s.bilateralTimeSec;
        agg.sessionTotalSec = s.sessionTotalSec;  // same for all
        agg.timeAtBestSec += s.timeAtBestSec;
        agg.totalSpreadTicks += s.avgSpreadTicks * s.spreadSampleCount;
        agg.spreadSampleCount += s.spreadSampleCount;
        agg.bilateralSwitchCount += s.bilateralSwitchCount;

        agg.ordersSent += s.ordersSent;
        agg.fills += s.fills;
        agg.cancels += s.cancels;
        agg.rejects += s.rejects;

        agg.avgLatencyUs += s.avgLatencyUs * s.latencySampleCount;
        agg.latencySampleCount += s.latencySampleCount;
        if (s.maxLatencyUs > agg.maxLatencyUs) agg.maxLatencyUs = s.maxLatencyUs;

        count++;
    }

    if (agg.sessionTotalSec > 0) {
        agg.bilateralRatio = (double)agg.bilateralTimeSec / agg.sessionTotalSec;
        agg.timeAtBestRatio = (double)agg.timeAtBestSec / agg.sessionTotalSec;
    }
    if (agg.spreadSampleCount > 0)
        agg.avgSpreadTicks = agg.totalSpreadTicks / agg.spreadSampleCount;
    if (agg.ordersSent > 0) {
        agg.fillRatio = (double)agg.fills / agg.ordersSent;
        agg.cancelRatio = (double)agg.cancels / agg.ordersSent;
    }
    if (agg.latencySampleCount > 0)
        agg.avgLatencyUs /= agg.latencySampleCount;

    return agg;
}

QuoteStatistics::SessionSummary QuoteStatistics::getSessionSummary() const {
    SessionSummary summary;
    summary.date = m_currentDate;

    double sumBilateralRatio = 0, sumSpread = 0, sumFillRatio = 0;
    double sumCancelRatio = 0, sumLatency = 0, sumTimeAtBest = 0;
    int count = 0;

    for (const auto& pair : m_stats) {
        auto s = pair.second.getResult();
        if (s.sessionTotalSec == 0) continue;

        count++;
        summary.totalOrders += s.ordersSent;
        summary.totalFills += s.fills;
        summary.totalCancels += s.cancels;
        summary.totalRejects += s.rejects;
        summary.totalSwitches += s.bilateralSwitchCount;

        sumBilateralRatio += s.bilateralRatio;
        sumSpread += s.avgSpreadTicks;
        sumFillRatio += s.fillRatio;
        sumCancelRatio += s.cancelRatio;
        sumLatency += (double)s.avgLatencyUs;
        sumTimeAtBest += s.timeAtBestRatio;
    }

    if (count > 0) {
        summary.totalCodes = count;
        summary.avgBilateralRatio = sumBilateralRatio / count;
        summary.avgSpreadTicks = sumSpread / count;
        summary.avgFillRatio = sumFillRatio / count;
        summary.avgCancelRatio = sumCancelRatio / count;
        summary.avgLatencyUs = sumLatency / count;
        summary.avgTimeAtBestRatio = sumTimeAtBest / count;
    }

    return summary;
}

} // namespace wt_option
