#pragma once

#include "IVolCurve.h"
#include "SingleFlightTaskRunner.h"
#include "OptionPricer2.h"
#include "GvvVolCurve.h"
#include "PowerVolCurve.h"
#include "LinearVolCurve.h"
#include "ConstantVolCurve.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../WTSTools/WTSLogger.h"

namespace wt_option {

/// Immutable input for one async vol-fit job.  The worker builds this at a
/// batch boundary and never mutates it afterwards.
struct VolFitExpirySnapshot {
    uint32_t expiry = 0;
    double forward = 0.0;
    double maturity = 0.0;
    double atmVol = 0.0;
    IVolCurve::dataset_t points;   // {K-F, iv/atmVol}
    uint64_t version = 0;
    double timestamp = 0.0;        // seconds (host clock) — drives decay carry
};

class AsyncVolFitter {
public:
    explicit AsyncVolFitter(std::string curve2Type = "gvv")
        : m_curve2Type(std::move(curve2Type)) {}
    ~AsyncVolFitter() { m_runner.stop(); }

    /// P1-b: main-curve type. "gvv" (default) | "linear" | "constant".
    void setCurveType(const std::string& t) {
        if (isValidMainCurveType(t)) m_curveType = t;
    }

    /// P1-b: curve2 type, extended beyond the R2 gvv|power pair.
    void setCurve2Type(const std::string& t) {
        if (isValidCurve2Type(t)) m_curve2Type = t;
    }

    /// P1-a: walk power→linear→constant when the main-curve fit fails.
    void setFallbackEnabled(bool b) { m_fallback = b; }

    /// P1-a: age bound (seconds) for carrying previous-fit wing points into
    /// the current dataset (port of PeriodicCurveFitter's decay_window).
    void setDecayWindow(double secs) { m_decayWindow = secs > 0 ? secs : 0.0; }

    /// Replace any queued snapshot set.  Single-flight: while a fit is running
    /// a duplicate submit returns false; caller may retry next period.
    bool submit(std::vector<VolFitExpirySnapshot> snapshots) {
        if (snapshots.empty()) return false;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_pending = std::move(snapshots);
            m_ready.store(false, std::memory_order_release);
        }
        return m_runner.submit("volfit", [this]() -> SingleFlightTaskRunner::TaskResult {
            std::vector<VolFitExpirySnapshot> input;
            {
                std::lock_guard<std::mutex> lock(m_mtx);
                input = m_pending;
            }
            auto fitted = fit(input);
            {
                std::lock_guard<std::mutex> lock(m_mtx);
                m_result = std::move(fitted);
                m_ready.store(true, std::memory_order_release);
            }
            return {"volfit", true, ""};
        });
    }

    bool hasResult() const { return m_ready.load(std::memory_order_acquire); }

    /// Worker-only batch-boundary publish.  Returns false when no completed
    /// result is available.
    bool tryPublish(OptionPricer2& pricer) {
        if (!m_ready.load(std::memory_order_acquire)) return false;
        std::vector<OptionPricer2::FittedCurveSet> result;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            result.swap(m_result);
            m_ready.store(false, std::memory_order_release);
        }
        pricer.publishFittedCurves(result);
        return true;
    }

private:
    /// P1-a: port of PeriodicCurveFitter::updateFitData decay-carry — merge
    /// wing points from the previous fit that are missing from the live
    /// snapshot.  Points older than m_decayWindow, or beyond the live span
    /// (+50% margin), age out so zombie wings do not pollute the smile as
    /// the ATM moves.  Coordinates are converted across forward/atmVol
    /// changes (x is stored as K−F; vol multiplier is re-based on the new
    /// atmVol).
    IVolCurve::dataset_t mergeCarried(const VolFitExpirySnapshot& s) const {
        if (m_decayWindow <= 0.0) return s.points;
        auto it = m_prev.find(s.expiry);
        if (it == m_prev.end() || it->second.points.empty()) return s.points;
        const PrevFit& prev = it->second;
        if (s.timestamp <= prev.timestamp ||
            s.timestamp - prev.timestamp > m_decayWindow)
            return s.points;

        IVolCurve::dataset_t merged = s.points;
        if (merged.empty()) return merged;
        const double spanLo = merged.front().first;
        const double spanHi = merged.back().first;   // caller sorts before submit
        const double margin = 0.5 * (spanHi - spanLo);

        for (const auto& p0 : prev.points) {
            const double strike = p0.first + prev.forward;      // back to absolute K
            const double xNew = strike - s.forward;
            if (xNew < spanLo - margin || xNew > spanHi + margin) continue;
            bool exists = false;
            for (const auto& p1 : merged) {
                if (std::fabs((p1.first + s.forward) - strike) < 1e-6) { exists = true; break; }
            }
            if (exists) continue;
            const double volAbs = p0.second * prev.atmVol;      // de-normalize
            merged.emplace_back(xNew, volAbs / s.atmVol);       // re-normalize
        }
        std::sort(merged.begin(), merged.end(),
                  [](const IVolCurve::datapoint_t& a, const IVolCurve::datapoint_t& b) {
                      return a.first < b.first;
                  });
        return merged;
    }

    std::vector<OptionPricer2::FittedCurveSet>
    fit(const std::vector<VolFitExpirySnapshot>& input) const {
        std::vector<OptionPricer2::FittedCurveSet> out;
        out.reserve(input.size());
        for (const auto& s0 : input) {
            if (s0.version == 0 || !(s0.forward > 0) || !(s0.maturity > 0)
                || !(s0.atmVol > 0) || s0.points.empty())
                continue;

            VolFitExpirySnapshot s = s0;
            s.points = mergeCarried(s0);

            IVolCurvePtr curve;
            const auto tryFit = [&](const std::string& type, int powerParams) -> IVolCurvePtr {
                auto c = createVolCurveByType(type, powerParams);
                c->setATMForward(s.forward);
                c->setMaturity(s.maturity);
                c->setATMVol(s.atmVol);
                if (type == "constant") return c;      // nothing to fit
                return c->fit(s.points) ? c : nullptr;
            };

            curve = tryFit(m_curveType, GvvVolCurve::SPOTVOL);
            if (!curve && m_fallback) {
                // Fallback chain: power → linear → constant(1.0).  The last
                // good curve still wins via OptionPricer2 version gating, so
                // a fallback publish never regresses a better previous fit.
                static std::atomic<uint64_t> s_fallbackWarn{0};
                for (const char* t : {"power", "linear", "constant"}) {
                    curve = tryFit(t, GvvVolCurve::SPOTVOL);
                    if (curve) {
                        uint64_t n = ++s_fallbackWarn;
                        if (n <= 20 || n % 1000 == 0)
                            WTSLogger::log_by_cat("strategy", LL_WARN,
                                "AsyncVolFitter exp={} main curve '{}' fit failed, fallback '{}' used (count={})",
                                s.expiry, m_curveType, t, n);
                        break;
                    }
                }
            }
            if (!curve) continue;

            IVolCurvePtr curve2 = tryFit(m_curve2Type, GvvVolCurve::RHO);
            if (!curve2)
                curve2 = std::make_shared<ConstantVolCurve>(1.0);

            m_prev[s.expiry] = {s.points, s.timestamp, s.atmVol, s.forward};
            out.push_back({s.expiry, curve, curve2, s.atmVol, s.version});
        }
        return out;
    }

private:
    struct PrevFit {
        IVolCurve::dataset_t points;
        double timestamp = 0.0;
        double atmVol = 0.0;
        double forward = 0.0;
    };

    SingleFlightTaskRunner m_runner;
    mutable std::mutex m_mtx;
    std::vector<VolFitExpirySnapshot> m_pending;
    std::vector<OptionPricer2::FittedCurveSet> m_result;
    std::atomic<bool> m_ready{false};
    std::string m_curveType{"gvv"};
    std::string m_curve2Type;
    bool m_fallback = true;
    double m_decayWindow = 0.0;
    mutable std::map<uint32_t, PrevFit> m_prev;   // decay-carry state (fit thread only)
};

} // namespace wt_option
