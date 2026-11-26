#include "OptionExpiryLifecycle.h"

#include <cmath>

namespace wt_option {

void OptionExpiryLifecycle::onSessionBegin(OptionGrid& grid)
{
    grid.refreshExpiryDays();
    grid.purgeExpired();
    // 2026-11-26: reevaluateFrontMonth() removed — "front" is a query
    // (frontExpiry()), not state; purgeExpired() above keeps the expiry
    // table clean so the query always picks the nearest live month.
}

double OptionExpiryLifecycle::grossPosition(const std::unordered_map<std::string, double>& positions)
{
    double gross = 0.0;
    for (const auto& [code, pos] : positions) {
        (void)code;
        if (std::isfinite(pos)) gross += std::fabs(pos);
    }
    return gross;
}

void OptionExpiryLifecycle::onSessionEnd(wtp::IHftStraCtx* ctx,
                                         const std::unordered_map<std::string, double>& positions,
                                         const AlertFn& alert)
{
    if (ctx)
        ctx->stra_cancel_all("");
    const double gross = grossPosition(positions);
    if (gross > 0 && alert)
        alert(gross, positions.size());
}

} // namespace wt_option
