#include "VolSpreadScanner.h"
#include "../OptionGrid.h"
#include "../StrikeData.h"
#include <cmath>

namespace wt_option {

double VolSpreadScanner::scanOption(OptionData* opt) {
    // A vol spread is a cross-expiry relationship and cannot be computed from
    // one option in isolation.  The real scan happens in onTick().
    (void)opt;
    return 0.0;
}

void VolSpreadScanner::onTick(const OptionGrid* grid) {
    if (!m_active || !isEnabled() || !grid) return;

    uint32_t baseExp = 0;
    double baseIv = 0.0;
    for (const auto& [exp, ed] : grid->expiries()) {
        if (!ed || !isExpiryEnabled(exp) || !ed->isValuesReady()) continue;
        StrikeDataPtr atm = const_cast<OptionGrid*>(grid)->getAtmStrike(exp);
        if (!atm) continue;
        const OptionData* od = atm->call() ? atm->call().get()
                            : (atm->put() ? atm->put().get() : nullptr);
        if (!od) continue;
        const double iv = od->values(0).impliedVol();
        if (!(iv > 0)) continue;
        if (baseExp == 0) {
            baseExp = exp;
            baseIv = iv;
            continue;
        }

        const double edge = std::fabs(iv - baseIv);
        if (edge < m_config.min_edge) continue;
        auto it = m_lastEdge.find(exp);
        if (it != m_lastEdge.end() &&
            std::fabs(edge - it->second) < m_config.min_edge * 0.5)
            continue;
        m_lastEdge[exp] = edge;
        publishHit(const_cast<OptionData*>(od), edge,
                   iv > baseIv ? "vol_spread_far_rich" : "vol_spread_far_cheap");
    }
}

void VolSpreadScanner::onOptionUpdate(OptionData* od) {
    if (!od || !m_active || !isEnabled()) return;
    const double score = scanOption(od);
    if (score > 0)
        publishHit(od, score, "vol_spread_edge");
}

REGISTER_SCANNER("VolSpreadScanner", VolSpreadScanner);

} // namespace wt_option
