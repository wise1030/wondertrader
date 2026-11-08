#include "FillPriceChecker.h"
#include "../WTSTools/WTSLogger.h"
#include <cmath>

namespace wt_option {

void FillPriceChecker::onFill(const std::string& code, double fillPx, double issuePx) {
    if (issuePx <= 0) return;

    double pct = std::abs(fillPx - issuePx) / issuePx;

    if (pct >= m_cfg.panicThreshold) {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "FillPriceChecker PANIC: {} fill={} issue={} pct={:.4f} >= {:.4f}",
            code, fillPx, issuePx, pct, m_cfg.panicThreshold);
        m_panicked = true;  // B13: latch until explicitly cleared
        if (m_panicCb) m_panicCb(code, fillPx, issuePx, pct);
    } else if (pct >= m_cfg.warningThreshold) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "FillPriceChecker WARNING: {} fill={} issue={} pct={:.4f} >= {:.4f}",
            code, fillPx, issuePx, pct, m_cfg.warningThreshold);
        if (m_warnCb) m_warnCb(code, fillPx, issuePx, pct);
    }
}

} // namespace wt_option
