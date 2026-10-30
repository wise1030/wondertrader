#include "SyntheticFutureScanner.h"
#include "../ExpiryData.h"
#include <cmath>

namespace wt_option {

double SyntheticFutureScanner::scanOption(OptionData* opt) {
    if (!m_active || !opt) return 0.0;
    if (opt->getBid() <= 0 || opt->getAsk() <= 0) return 0.0;

    auto sd = opt->getStrikeData();
    if (!sd || !sd->call() || !sd->put()) return 0.0;   // parity needs both legs
    const OptionData* call = sd->call().get();
    const OptionData* put = sd->put().get();
    if (call->getBid() <= 0 || call->getAsk() <= 0 ||
        put->getBid() <= 0 || put->getAsk() <= 0)
        return 0.0;

    auto ed = opt->getExpiryData();
    const double underlying = ed ? ed->getUnderlyingPrice() : 0.0;
    if (!(underlying > 0)) return 0.0;

    // Put-call parity: C - P + K should equal the pricing forward.
    const double callMid = (call->getBid() + call->getAsk()) * 0.5;
    const double putMid  = (put->getBid() + put->getAsk()) * 0.5;
    const double syntheticFwd = callMid - putMid + sd->getStrikePrice();
    const double edge = std::fabs(syntheticFwd - underlying);
    if (!(edge >= m_config.min_edge)) return 0.0;
    return edge;
}

void SyntheticFutureScanner::onOptionUpdate(OptionData* od) {
    if (!od || !m_active || !isEnabled()) return;
    const double score = scanOption(od);
    if (score > 0)
        publishHit(od, score, "parity_forward_edge");
}

REGISTER_SCANNER("SyntheticFutureScanner", SyntheticFutureScanner);

} // namespace wt_option
