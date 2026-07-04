#include "MMScanner.h"
#include <cmath>

namespace wt_option {

int MMScanner::shouldScanOption(OptionData* opt, ScanMode smode) {
    if (!m_active || !opt) return 0;
    if (opt->getBid() <= 0 || opt->getAsk() <= 0) return 0;
    double delta = std::abs(opt->greeks().delta());
    if (delta < m_config.delta_min || delta > m_config.delta_max) return 0;
    double spread = opt->getAsk() - opt->getBid();
    if (spread > m_config.max_spread) return 0;
    return 1;
}

double MMScanner::scanOption(OptionData* opt, ScanMode smode) {
    if (!shouldScanOption(opt, smode)) return 0.0;
    double theo = opt->getTheoPrice();
    if (theo <= 0) return 0.0;
    double bid = opt->getBid();
    double ask = opt->getAsk();
    double mid = (bid + ask) * 0.5;
    double edge = std::abs(theo - mid);
    if (edge < m_config.min_edge) return 0.0;
    return edge;
}

bool MMScanner::fireOption(OptionData* opt, int dir, double px, double score, ScanMode smode) {
    if (!opt || score <= 0) return false;
    // Signal to ControllableTradingGrid via onOptionHit
    // In WT, the strategy layer handles actual order placement
    return true;
}

} // namespace wt_option
