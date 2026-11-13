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

#include <cstdint>
#include <deque>
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

private:
    struct Entry {
        double predicted = 0;
        double baseRealized = 0;
        double dueTime = 0;
    };

    double now() const { return m_getTime ? m_getTime() : 0.0; }

    std::deque<Entry> m_pending;
    uint32_t m_resolved = 0;
    uint32_t m_hits = 0;
    double   m_sumAbsErr = 0;
    double   m_lastRealized = 0;
    double   m_lastReport = 0;
    std::function<double()> m_getTime;
};

} // namespace wt_option
