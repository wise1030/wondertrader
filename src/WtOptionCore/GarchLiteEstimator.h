/*!
 * \file GarchLiteEstimator.h
 * \brief GARCH-lite volatility estimator (borrow plan §1: GARCH-lite as a
 *        second ATM-vol anchor, blended via OptionPricer2::setGarchBlend).
 *
 * EWMA (RiskMetrics) variance recursion with optional long-run mean
 * reversion. Log returns sampled every `sampleSec` seconds. This is a small
 * self-contained estimator — NO new pricer, NO GARCH parameter GUI: it plugs
 * into OptionPricer2's atmVol blend (`garch_weight`).
 */
#pragma once

#include <cmath>
#include <cstdint>

namespace wt_option {

class GarchLiteEstimator {
public:
    struct Config {
        double lambda = 0.94;             // EWMA decay (RiskMetrics default)
        double sampleSec = 60.0;          // log-return sampling interval
        int32_t warmup = 20;              // min samples before vol is emitted
        double longRunVol = 0.0;          // optional long-run vol anchor (0=off)
        double meanRevert = 0.0;          // weight of long-run variance (0=off)
    };

    void configure(const Config& c) { m_cfg = c; }

    /// Feed the underlying price at time nowSec (strategy worker clock).
    /// Returns the annualized volatility once `warmup` samples have accrued,
    /// or 0.0 before that (caller then keeps the fitted ATM vol).
    double update(double nowSec, double price) {
        if (m_lastSampleTs < 0) {
            m_lastSampleTs = nowSec;
            m_lastPrice = price;
            return 0.0;
        }
        if (nowSec - m_lastSampleTs < m_cfg.sampleSec) return m_annVol;

        const double r = (m_lastPrice > 0 && price > 0)
            ? std::log(price / m_lastPrice) : 0.0;
        m_lastSampleTs = nowSec;
        m_lastPrice = price;

        if (m_samples == 0) {
            m_var = r * r;
        } else {
            m_var = (1.0 - m_cfg.lambda) * m_var + m_cfg.lambda * (r * r);
        }
        if (m_cfg.longRunVol > 0 && m_cfg.meanRevert > 0) {
            m_var = (1.0 - m_cfg.meanRevert) * m_var
                  + m_cfg.meanRevert * m_cfg.longRunVol * m_cfg.longRunVol;
        }
        ++m_samples;

        if (m_samples >= m_cfg.warmup && m_var > 0) {
            const double periodsPerYear = 365.0 * 24.0 * 3600.0 / m_cfg.sampleSec;
            m_annVol = std::sqrt(m_var * periodsPerYear);
        }
        return m_annVol;
    }

    double vol() const { return m_annVol; }
    int32_t samples() const { return m_samples; }

private:
    Config m_cfg;
    double m_lastSampleTs = -1.0;
    double m_lastPrice = 0.0;
    double m_var = 0.0;
    double m_annVol = 0.0;
    int32_t m_samples = 0;
};

} // namespace wt_option
