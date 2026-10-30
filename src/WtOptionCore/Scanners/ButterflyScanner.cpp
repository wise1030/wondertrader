#include "ButterflyScanner.h"
#include "../OptionGrid.h"
#include "../StrikeData.h"
#include <cmath>

namespace wt_option {

double ButterflyScanner::scanOption(OptionData* opt) {
    // A real butterfly needs three consecutive strikes; a per-option callback
    // alone is structurally insufficient.  The full scan happens in onTick().
    (void)opt;
    return 0.0;
}

void ButterflyScanner::onTick(const OptionGrid* grid) {
    if (!m_active || !isEnabled() || !grid) return;

    for (const auto& [exp, ed] : grid->expiries()) {
        if (!ed || !isExpiryEnabled(exp)) continue;
        const auto strikes = grid->getStrikesByExpiry(exp);
        if (strikes.size() < 3) continue;

        for (size_t i = 1; i + 1 < strikes.size(); ++i) {
            const StrikeData* lower = strikes[i - 1].get();
            const StrikeData* middle = strikes[i].get();
            const StrikeData* upper = strikes[i + 1].get();
            if (!lower || !middle || !upper) continue;

            // Long one lower strike, short two middle, long one upper.
            // Try calls first, then puts if the call chain is incomplete.
            auto chainPrice = [](const StrikeData* sd, bool useCall, double& px, OptionData*& hit) {
                const OptionDataPtr& od = useCall ? sd->call() : sd->put();
                if (!od || od->getBid() <= 0 || od->getAsk() <= 0) return false;
                px = (od->getBid() + od->getAsk()) * 0.5;
                hit = od.get();
                return true;
            };

            double lp = 0, mp = 0, up = 0;
            OptionData *lo = nullptr, *mo = nullptr, *uo = nullptr;
            bool ok = chainPrice(lower, true, lp, lo) &&
                      chainPrice(middle, true, mp, mo) &&
                      chainPrice(upper, true, up, uo);
            if (!ok) {
                ok = chainPrice(lower, false, lp, lo) &&
                     chainPrice(middle, false, mp, mo) &&
                     chainPrice(upper, false, up, uo);
            }
            if (!ok || !mo) continue;

            const double fly = lp - 2.0 * mp + up;   // negative => cheap long fly
            const double edge = std::fabs(fly);
            if (edge < m_config.min_edge) continue;

            auto lastIt = m_lastEdge.find(exp);
            if (lastIt != m_lastEdge.end() &&
                std::fabs(edge - lastIt->second) < m_config.min_edge * 0.5)
                continue;   // avoid resubmitting the same fly every tick
            m_lastEdge[exp] = edge;

            ScannerHitEvent ev;
            ev.option = mo;
            ev.legs = {lo, mo, uo};   // simultaneous 3-leg butterfly
            ev.signal = edge;
            ev.reason = fly < 0 ? "butterfly_cheap_long" : "butterfly_cheap_short";
            ev.timestamp = getTimestamp();
            notifyHit(ev);
            break;
        }
    }
}

void ButterflyScanner::onOptionUpdate(OptionData* od) {
    if (!od || !m_active || !isEnabled()) return;
    const double score = scanOption(od);
    if (score > 0)
        publishHit(od, score, "butterfly_edge");
}

REGISTER_SCANNER("ButterflyScanner", ButterflyScanner);

} // namespace wt_option
