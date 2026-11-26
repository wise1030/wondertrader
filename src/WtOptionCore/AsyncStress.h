#pragma once

#include "SingleFlightTaskRunner.h"
#include "StressAnalyzer.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace wt_option {

struct StressGreekSnapshot {
    double delta = 0.0;
    double gamma = 0.0;
    double vega = 0.0;
    double theta = 0.0;
};

struct StressExpirySnapshot {
    uint32_t expiry = 0;
    double totalDelta = 0.0;   // option + expiry-exclusive hedge (calculation)
    double optionDelta = 0.0;  // option-only delta (legacy log field)
    double gamma = 0.0;
    double vega = 0.0;
    double theta = 0.0;
};

struct StressSnapshot {
    uint64_t seq = 0;
    uint32_t sessionDate = 0;
    uint64_t positionVersion = 0;
    double submittedAt = 0.0;
    double spot = 0.0;
    StressGreekSnapshot portfolio;
    std::vector<StressExpirySnapshot> expiries;
    std::vector<StressScenario> scenarios;
};

struct StressExpiryReport {
    uint32_t expiry = 0;
    double optionDelta = 0.0;
    StressResult worst;
};

struct StressReport {
    uint64_t seq = 0;
    uint32_t sessionDate = 0;
    uint64_t positionVersion = 0;
    double submittedAt = 0.0;
    double spot = 0.0;
    StressGreekSnapshot portfolio;
    std::vector<StressResult> all;
    StressResult worst;
    std::vector<StressExpiryReport> expiries;
};

/// Single-flight stress evaluator. Background jobs own a deep value snapshot;
/// they never dereference the strategy, grid, risk book, state machine or UI.
/// Results are consumed exactly once by the strategy worker at a batch edge.
class AsyncStress {
public:
    AsyncStress() = default;
    ~AsyncStress() { stop(); }

    AsyncStress(const AsyncStress&) = delete;
    AsyncStress& operator=(const AsyncStress&) = delete;

    bool submit(StressSnapshot snapshot) {
        if (snapshot.scenarios.empty() || !(snapshot.spot > 0.0)) return false;
        return m_runner.submit("stress",
            [this, snapshot = std::move(snapshot)]() -> SingleFlightTaskRunner::TaskResult {
                StressAnalyzer analyzer;
                for (const auto& scenario : snapshot.scenarios)
                    analyzer.addScenario(scenario);

                StressReport report;
                report.seq = snapshot.seq;
                report.sessionDate = snapshot.sessionDate;
                report.positionVersion = snapshot.positionVersion;
                report.submittedAt = snapshot.submittedAt;
                report.spot = snapshot.spot;
                report.portfolio = snapshot.portfolio;
                const auto& g = snapshot.portfolio;
                report.all = analyzer.evaluateAll(snapshot.spot, g.delta, g.gamma,
                                                  g.vega, g.theta);
                report.worst.scenario.name = "<none>";
                bool haveWorst = false;
                for (const auto& r : report.all) {
                    if (!haveWorst || r.pnl < report.worst.pnl) {
                        report.worst = r;
                        haveWorst = true;
                    }
                }

                report.expiries.reserve(snapshot.expiries.size());
                for (const auto& e : snapshot.expiries) {
                    StressExpiryReport er;
                    er.expiry = e.expiry;
                    er.optionDelta = e.optionDelta;
                    er.worst = analyzer.worst(snapshot.spot, e.totalDelta, e.gamma,
                                              e.vega, e.theta);
                    report.expiries.push_back(std::move(er));
                }

                {
                    std::lock_guard<std::mutex> lock(m_mtx);
                    m_result = std::move(report);
                    m_ready.store(true, std::memory_order_release);
                }
                return {"stress", true, ""};
            });
    }

    bool busy() const { return m_runner.isRunningOrQueued("stress"); }
    bool hasResult() const { return m_ready.load(std::memory_order_acquire); }

    bool tryTake(StressReport& out) {
        if (!m_ready.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> lock(m_mtx);
        if (!m_ready.load(std::memory_order_relaxed)) return false;
        out = std::move(m_result);
        m_result = StressReport{};
        m_ready.store(false, std::memory_order_release);
        return true;
    }

    void stop() { m_runner.stop(); }

    /// Freshness is checked by the worker immediately before any side effect.
    /// ttlSec <= 0 disables only the age bound; backwards time is still invalid.
    static bool isFresh(const StressReport& report, uint64_t lastPublishedSeq,
                        uint32_t currentSession, uint64_t currentPositionVersion,
                        double now, double ttlSec) {
        if (report.seq <= lastPublishedSeq) return false;
        if (report.sessionDate != currentSession) return false;
        if (report.positionVersion != currentPositionVersion) return false;
        if (!std::isfinite(report.submittedAt) || !std::isfinite(now)
            || now < report.submittedAt) return false;
        return ttlSec <= 0.0 || now - report.submittedAt <= ttlSec;
    }

private:
    SingleFlightTaskRunner m_runner;
    mutable std::mutex m_mtx;
    StressReport m_result;
    std::atomic<bool> m_ready{false};
};

} // namespace wt_option
