#include "ButterflyScanner.h"
#include <cmath>

namespace wt_option {

double ButterflyScanner::scanOption(OptionData* opt) {
    if (!m_active || !opt) return 0.0;
    if (opt->getBid() <= 0 || opt->getAsk() <= 0) return 0.0;
    double delta = std::abs(opt->greeks().delta());
    if (delta < m_config.delta_min || delta > m_config.delta_max) return 0.0;
    double theo = opt->getTheoPrice();
    if (theo <= 0) return 0.0;
    double mid = (opt->getBid() + opt->getAsk()) * 0.5;
    double edge = std::abs(theo - mid);
    if (edge < m_config.min_edge) return 0.0;
    return edge;
}

} // namespace wt_option
