#include "GarchScanner.h"
#include <cmath>

namespace wt_option {

// ---------------------------------------------------------------------------
// R7: GARCH(1,1) conditional-volatility kernel (replaces the placeholder).
// quantbox's GarchSimulator is a Monte-Carlo scenario engine (N-path matrix
// simulation for expiry distributions) — wrong tool for a scanner signal.
// The scanner's actual need is a forward vol estimate to compare against ATM
// IV, which the standard GARCH(1,1) recursion provides in O(1) per tick:
//     σ²_t = ω + α·r²_{t−1} + β·σ²_{t−1}
// Stationarity (α+β<1) is enforced by renormalization.
// ---------------------------------------------------------------------------
void GarchScanner::onUnderlyingUpdate(double price)
{
    if (!m_active || price <= 0) return;

    if (m_lastPrice > 0) {
        double ret = price / m_lastPrice - 1.0;   // simple close-to-close return
        updateGarch(ret);
    }
    m_lastPrice = price;
}

void GarchScanner::updateGarch(double ret)
{
    // Init variance with the first squared return
    if (m_samples == 0)
        m_sigma = std::sqrt(std::fabs(ret)) + 1e-12;

    double var = m_sigma * m_sigma;
    var = m_config.garch_omega
        + m_config.garch_alpha * ret * ret
        + m_config.garch_beta * var;

    // Enforce stationarity: if α+β drifts ≥1, scale the persistence term so
    // long-run variance stays finite.
    double ab = m_config.garch_alpha + m_config.garch_beta;
    if (ab >= 1.0) {
        double scaledBeta = (1.0 - m_config.garch_alpha) * 0.999;
        var = m_config.garch_omega
            + m_config.garch_alpha * ret * ret
            + scaledBeta * (m_sigma * m_sigma);
    }

    if (!std::isfinite(var) || var <= 0) var = m_sigma * m_sigma;
    m_sigma = std::sqrt(var);

    if (m_samples < m_config.warmup_returns)
        ++m_samples;
}

double GarchScanner::scanOption(OptionData* opt) {
    if (!m_active || !opt) return 0.0;
    if (opt->getBid() <= 0 || opt->getAsk() <= 0) return 0.0;
    if (!volReady()) return 0.0;                    // R7: warm-up gate
    double delta = std::abs(opt->greeks().delta());
    if (delta < m_config.delta_min || delta > m_config.delta_max) return 0.0;

    // R7 signal: |GARCH forecast (annualized) − ATM IV| priced via vega.
    const double iv = opt->values(0).impliedVol();
    if (iv <= 0) return 0.0;
    const double vega = opt->greeks().vega();
    if (vega == 0) return 0.0;

    const double sigmaAnn = m_sigma * std::sqrt(m_config.annualization_factor);
    const double edge = std::fabs(sigmaAnn - iv) * vega;
    if (edge < m_config.min_edge) return 0.0;
    return edge;
}

// R6: factory registration — closes the "factory map empty" defect
REGISTER_SCANNER("GarchScanner", GarchScanner);

} // namespace wt_option
