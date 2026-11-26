#pragma once

#include "IVolCurve.h"
#include "SingleFlightTaskRunner.h"
#include "OptionPricer2.h"
#include "GvvVolCurve.h"
#include "PowerVolCurve.h"
#include "LinearVolCurve.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "WTSLogger.h"

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
    /// Effective blend weight for this expiry at SUBMIT time (0 = curve2
    /// linear prices, 1 = curve1 prices, in-between = blend).  The fitter
    /// uses it to decide which curves MUST succeed; readiness is re-evaluated
    /// at publish/pricing time against the CURRENT weight, so a hot weight
    /// change between submit and publish can only ever make readiness MORE
    /// conservative, never less.
    double weight = 0.0;
};

class AsyncVolFitter {
public:
    explicit AsyncVolFitter(std::string curve2Type = "linear")
        : m_curve2Type(std::move(curve2Type)) {}
    ~AsyncVolFitter() { m_runner.stop(); }

    /// Main-curve type: "gvv" (default) | "power" (2026-11-26 whitelist).
    void setCurveType(const std::string& t) {
        if (isValidMainCurveType(t)) m_curveType = t;
    }

    /// Curve2 type: "linear" only (2026-11-26 design: curve2 is fixed linear;
    /// "constant" retired with the fallback chain).
    void setCurve2Type(const std::string& t) {
        if (isValidCurve2Type(t)) m_curve2Type = t;
    }

    /// P1-a: age bound (seconds) for carrying previous-fit wing points into
    /// the current dataset (port of PeriodicCurveFitter's decay_window).
    void setDecayWindow(double secs) { m_decayWindow = secs > 0 ? secs : 0.0; }

    /// P1 (2026-11-18): WLS wing-weight decay for the GVV fit
    /// (w = exp(-beta*x^2/(atmvol^2*T))); 0 = legacy equal weights.
    void setBeta(double beta) { m_beta = beta >= 0.0 ? beta : 0.0; }

    /// Audit 11-22 (round 3): GVV alpha search-range override (both <0 =
    /// keep the curve defaults). Passed to every GVV fit attempt.
    void setAlphaRange(double dn, double up) {
        if (dn >= 0.0) m_alphaDn = dn;
        if (up >= 0.0) m_alphaUp = up;
        if (m_alphaDn > m_alphaUp) std::swap(m_alphaDn, m_alphaUp);
    }

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
    /// result is available.  When publishedExps is given, it receives the
    /// expiry of every curve set delivered to the pricer (the pricer may
    /// still skip individual stale-version sets) — the strategy uses this
    /// to lift its market-readiness block per expiry.
    bool tryPublish(OptionPricer2& pricer, std::vector<uint32_t>* publishedExps = nullptr) {
        if (!m_ready.load(std::memory_order_acquire)) return false;
        std::vector<OptionPricer2::FittedCurveSet> result;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            result.swap(m_result);
            m_ready.store(false, std::memory_order_release);
        }
        if (publishedExps) {
            publishedExps->clear();
            publishedExps->reserve(result.size());
            for (const auto& r : result) publishedExps->push_back(r.expiry);
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

    /// L1 — model-independent dataset gate (2026-11-26, user-approved plan).
    /// Runs BEFORE any model fit; a dataset that fails here is never fitted
    /// and never published.  This is the primary fix for the 2026-11-26
    /// 21:05 explosion: a snapshot normalized by the default 0.1 anchor
    /// against a ~0.24-0.30 market yields multipliers ~2.3-3.0, far outside
    /// the [0.7, 1.3] band, so the poisoned first fit is rejected outright
    /// instead of being published (or worse, "fallback-published").
    /// NOTE: no explicit "atmVol == default 0.1" check — the median check
    /// subsumes it, and a market whose ATM vol really is 0.10 must still fit.
    static bool validateDataset(const VolFitExpirySnapshot& s, std::string* why) {
        if (s.points.size() < 4) {
            if (why) *why = "too few points";
            return false;
        }
        double lo = 0, hi = 0;
        bool hasUp = false, hasDn = false;
        std::vector<double> ys;
        ys.reserve(s.points.size());
        for (const auto& p : s.points) {
            if (!std::isfinite(p.first) || !std::isfinite(p.second) || !(p.second > 0.0)) {
                if (why) *why = "non-finite/non-positive multiplier";
                return false;
            }
            if (p.second < 0.02 || p.second > 20.0) {
                if (why) *why = "multiplier out of sanity range";
                return false;
            }
            if (p.first > 1e-9) hasUp = true;
            else if (p.first < -1e-9) hasDn = true;
            ys.push_back(p.second);
            lo = std::min(lo, p.first); hi = std::max(hi, p.first);
        }
        if (!hasUp || !hasDn) {
            if (why) *why = "one-sided window";
            return false;
        }
        if (hi - lo < 1e-9) {
            if (why) *why = "degenerate strike span";
            return false;
        }
        std::sort(ys.begin(), ys.end());
        const double med = ys[ys.size() / 2];
        if (med < 0.7 || med > 1.3) {
            if (why) *why = "normalized median off-scale (atmVol mismatch)";
            return false;
        }
        return true;
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

            // L1: shared, model-independent dataset gate — reject before any
            // model sees the data (see validateDataset comment).
            {
                std::string why;
                if (!validateDataset(s, &why)) {
                    static std::atomic<uint64_t> s_l1Reject{0};
                    const uint64_t n = ++s_l1Reject;
                    if (n <= 20 || n % 100 == 0)
                        WTSLogger::log_by_cat("strategy", LL_WARN,
                            "AsyncVolFitter exp={} fit rejected by dataset gate: {} "
                            "(atmVol={:.4f} n={} count={})",
                            s.expiry, why, s.atmVol, s.points.size(), n);
                    continue;
                }
            }

            const double w = std::max(0.0, std::min(1.0, s.weight));
            const bool mainRequired = w > 0.0;
            const bool curve2Required = w < 1.0;

            const auto tryFit = [&](const std::string& type, int powerParams) -> IVolCurvePtr {
                auto c = createVolCurveByType(type, powerParams, m_beta,
                                              m_alphaDn, m_alphaUp);
                c->setATMForward(s.forward);
                c->setMaturity(s.maturity);
                c->setATMVol(s.atmVol);
                return c->fit(s.points) ? c : nullptr;
            };

            // L2: fit BOTH configured curves (curve2 is always needed as the
            // blend partner / observability mirror; NO fallback — a failed
            // curve stays null and is simply not published).
            IVolCurvePtr curve  = tryFit(m_curveType,  GvvVolCurve::SPOTVOL);
            IVolCurvePtr curve2 = tryFit(m_curve2Type, GvvVolCurve::RHO);

            if (mainRequired && !curve) {
                static std::atomic<uint64_t> s_mainFail{0};
                const uint64_t n = ++s_mainFail;
                if (n <= 20 || n % 100 == 0)
                    WTSLogger::log_by_cat("strategy", LL_WARN,
                        "AsyncVolFitter exp={} main curve '{}' fit failed at weight={:.2f} "
                        "— expiry not published (count={})",
                        s.expiry, m_curveType, w, n);
                continue;   // curve1 is required — no publish, no substitute
            }
            if (curve2Required && !curve2) {
                static std::atomic<uint64_t> s_c2Fail{0};
                const uint64_t n = ++s_c2Fail;
                if (n <= 20 || n % 100 == 0)
                    WTSLogger::log_by_cat("strategy", LL_WARN,
                        "AsyncVolFitter exp={} curve2 '{}' fit failed at weight={:.2f} "
                        "— expiry not published (count={})",
                        s.expiry, m_curve2Type, w, n);
                continue;   // curve2 is required — no publish, no constant stub
            }
            // Observability: the non-pricing curve is fitted as a health
            // mirror only — its failure never blocks a weight-driven publish.
            if (!mainRequired && !curve) {
                static std::atomic<uint64_t> s_mainSkip{0};
                const uint64_t n = ++s_mainSkip;
                if (n <= 20 || n % 500 == 0)
                    WTSLogger::log_by_cat("strategy", LL_INFO,
                        "AsyncVolFitter exp={} curve1 unfit — linear pricing "
                        "(weight=0, gate-only mirror failed, count={})",
                        s.expiry, n);
            }

            m_prev[s.expiry] = {s.points, s.timestamp, s.atmVol, s.forward};
            // Per-curve versions: 0 = "no update for this curve".  Both carry
            // the snapshot version; the pricer installs each curve
            // independently against its own version map, so a linear-only
            // history keeps advancing curve2 while the main version stays 0.
            OptionPricer2::FittedCurveSet fcs;
            fcs.expiry   = s.expiry;
            fcs.volCurve = curve;
            fcs.volCurve2 = curve2;
            fcs.version  = curve  ? s.version : 0;
            fcs.version2 = curve2 ? s.version : 0;
            fcs.atmVol   = s.atmVol;
            out.push_back(std::move(fcs));
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
    std::string m_curve2Type{"linear"};
    double m_decayWindow = 0.0;
    double m_beta = 0.0;
    double m_alphaDn = -1.0;   // <0 = GvvVolCurve::Config default
    double m_alphaUp = -1.0;
    mutable std::map<uint32_t, PrevFit> m_prev;   // decay-carry state (fit thread only)
};

} // namespace wt_option
