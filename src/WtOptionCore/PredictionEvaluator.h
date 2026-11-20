/*!
 * \file PredictionEvaluator.h
 * \brief Prediction accuracy tracking — absorbed from quantbox stratlib
 *        (PredictionEvaluator / PeriodicPredictionReport), compact form.
 *
 * Closes the Predictor loop: the strategy already feeds SignalsPredictor via
 * TriggerEngine, but nobody scored the predictions. This evaluator records
 * (predicted forward, horizon) pairs and scores them against the realized
 * forward when the horizon expires, exposing hit-rate / MAE and emitting a
 * periodic report through WTSLogger.
 *
 * Single-threaded: lives on the async worker (called from on_batch_complete).
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <utility>
#include <string>
#include <functional>

#include "WTSLogger.h"

namespace wt_option {

class PredictionEvaluator {
public:
    void setGetTimeFn(std::function<double()> fn) { m_getTime = std::move(fn); }

    /// Record a prediction: predicted forward price valid at now+horizonSec
    void onPrediction(double predictedForward, double horizonSec) {
        if (horizonSec <= 0) return;
        Entry e;
        e.predicted = predictedForward;
        e.baseRealized = m_lastRealized;   // realized at prediction time
        e.dueTime = now() + horizonSec;
        m_pending.push_back(e);
    }

    /// Feed the latest realized forward; resolves any due predictions
    void onRealized(double forwardNow) {
        if (forwardNow <= 0 || m_getTime == nullptr) return;
        const double nowT = now();
        m_lastRealized = forwardNow;

        while (!m_pending.empty() && m_pending.front().dueTime <= nowT) {
            const Entry e = m_pending.front();
            m_pending.pop_front();

            const double actualMove   = forwardNow - e.baseRealized;
            const double predictedMove = e.predicted - e.baseRealized;
            const double err = forwardNow - e.predicted;

            m_sumAbsErr += std::abs(err);
            ++m_resolved;
            // Directional hit: predicted move sign matches realized move sign
            if ((predictedMove >= 0 && actualMove >= 0) ||
                (predictedMove <= 0 && actualMove <= 0))
                ++m_hits;
        }
    }

    /// Emit a report every `period` seconds (call once per batch)
    void maybeReport(double periodSec) {
        if (m_resolved == 0) return;
        const double t = now();
        if (t - m_lastReport < periodSec) return;
        m_lastReport = t;

        const double mae = m_sumAbsErr / m_resolved;
        const double hitRate = 100.0 * m_hits / m_resolved;
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "PredictionEvaluator: resolved={} hitRate={:.1f}% MAE={:.4f} pending={}",
            m_resolved, hitRate, mae, m_pending.size());
    }

    uint32_t resolvedCount() const { return m_resolved; }
    double meanAbsError() const { return m_resolved ? m_sumAbsErr / m_resolved : 0.0; }
    double hitRate() const { return m_resolved ? 100.0 * m_hits / m_resolved : 0.0; }

    // ---- P1 batch-6.4: scanner-signal scoring --------------------------
    // Each scanner combo predicts its primary leg's mid converges toward
    // theo (predMove = theo - mid at hit time).  onScannerMid() resolves
    // due entries against the live mid; the rolling window keeps the last
    // `kWindow` (predMove, realizedMove) pairs for the IC.
    void onScannerSignal(double predMove, double midNow, double horizonSec) {
        if (horizonSec <= 0 || midNow <= 0) return;
        ScanEntry e;
        e.dueTime = now() + horizonSec;
        e.baseMid = midNow;
        e.predMove = predMove;
        m_scanPending.push_back(e);
    }

    void onScannerMid(double midNow) {
        if (midNow <= 0) return;
        const double nowT = now();
        while (!m_scanPending.empty() && m_scanPending.front().dueTime <= nowT) {
            const ScanEntry e = m_scanPending.front();
            m_scanPending.pop_front();
            const double realized = midNow - e.baseMid;
            ++m_scanResolved;
            if ((e.predMove >= 0 && realized >= 0) || (e.predMove <= 0 && realized <= 0))
                ++m_scanHits;
            m_window.emplace_back(e.predMove, realized);
            if (m_window.size() > kWindow) m_window.pop_front();
        }
    }

    uint32_t scanResolvedCount() const { return m_scanResolved; }
    double scanHitRate() const { return m_scanResolved ? 100.0 * m_scanHits / m_scanResolved : 0.0; }

    /// Pearson IC over the rolling window; 0 when fewer than 5 pairs.
    double scanIC() const {
        const size_t n = m_window.size();
        if (n < 5) return 0.0;
        double sp = 0, sr = 0;
        for (const auto& pr : m_window) { sp += pr.first; sr += pr.second; }
        const double mp = sp / n, mr = sr / n;
        double cov = 0, vp = 0, vr = 0;
        for (const auto& pr : m_window) {
            const double dp = pr.first - mp, dr = pr.second - mr;
            cov += dp * dr; vp += dp * dp; vr += dr * dr;
        }
        const double denom = std::sqrt(vp * vr);
        return denom > 0 ? cov / denom : 0.0;
    }

private:
    static constexpr size_t kWindow = 100;

    struct Entry {
        double predicted = 0;
        double baseRealized = 0;
        double dueTime = 0;
    };
    struct ScanEntry {
        double dueTime = 0;
        double baseMid = 0;
        double predMove = 0;
    };

    double now() const { return m_getTime ? m_getTime() : 0.0; }

    std::deque<Entry> m_pending;
    uint32_t m_resolved = 0;
    uint32_t m_hits = 0;
    double   m_sumAbsErr = 0;
    double   m_lastRealized = 0;
    double   m_lastReport = 0;
    std::function<double()> m_getTime;

    // P1 batch-6.4 scanner-signal state
    std::deque<ScanEntry> m_scanPending;
    uint32_t m_scanResolved = 0;
    uint32_t m_scanHits = 0;
    std::deque<std::pair<double, double>> m_window;   // (predMove, realized)
};

} // namespace wt_option
