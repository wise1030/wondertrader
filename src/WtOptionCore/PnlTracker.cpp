/*!
 * \file PnlTracker.cpp
 * \brief Per-instrument PnL tracker implementation
 */
#include "PnlTracker.h"

namespace wt_option {

PnlTracker::PnlTracker(double multiplier)
    : m_multiplier(multiplier)
    , m_feepct(0)
    , m_curturnover(0)
    , m_curpnl(0)
    , m_lastpnl(0)
    , m_lastpos(0)
    , m_fillsz(0)
    , m_lastfillpx(0)
{
}

void PnlTracker::onPriceUpdate(double bid, double ask) {
    if (bid <= 0 || ask <= 0) return;
    double curmid = (bid + ask) * 0.5;
    // Unrealized = realized + position * (curmid - lastfillpx) * mult
    m_curpnl = m_lastpnl + m_multiplier * m_lastpos * (curmid - m_lastfillpx);
}

void PnlTracker::onFill(bool isBuy, uint32_t qty, double price) {
    int32_t sign = isBuy ? 1 : -1;
    m_curturnover += m_multiplier * qty * price;
    // Realized PnL: previous position * price change - fees
    m_lastpnl = m_lastpnl + m_multiplier * m_lastpos * (price - m_lastfillpx)
                - m_multiplier * qty * price * m_feepct;
    m_lastfillpx = price;
    m_lastpos += sign * static_cast<int32_t>(qty);
    m_fillsz += qty;
    // Update curpnl after fill
    m_curpnl = m_lastpnl;
}

} // namespace wt_option
