/*!
 * \file PnlTracker.cpp
 * \brief Per-instrument PnL tracker implementation
 */
#include "PnlTracker.h"

#include <algorithm>
#include <cmath>

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
    m_lastpnl = m_lastpnl + m_multiplier * m_lastpos * (price - m_lastfillpx)
                - m_multiplier * qty * price * m_feepct;
    m_lastfillpx = price;
    m_lastpos += sign * static_cast<int32_t>(qty);
    m_fillsz += qty;
    m_curpnl = m_lastpnl;

    // B5 additions (non-invasive to the aggregate path above)
    updateWeightedEntry(isBuy, qty, price);
    if (m_fifoMode) fifoMatch(isBuy, qty, price);
}

// ---------------------------------------------------------------------------
// B5: weighted average entry price (quantbox MarkerToMarket semantics)
// Signed-basis bookkeeping: adds blend at cost, reduces consume at the stored
// average, and a cross through flat closes the old basis and opens a new one
// on the far side at the fill price.
void PnlTracker::updateWeightedEntry(bool isBuy, uint32_t qty, double price) {
    const double q = static_cast<double>(qty);
    const double signedFill = isBuy ? q : -q;
    const double basisQty = m_wavgEntryQty;

    if (std::fabs(basisQty) < 1e-9) {
        m_wavgEntryQty = signedFill;
        m_wavgEntryPx  = price;
        return;
    }

    const bool sameDir = (basisQty > 0) == (signedFill > 0);
    if (sameDir) {
        const double tot = std::fabs(basisQty) + q;
        m_wavgEntryPx  = (m_wavgEntryPx * std::fabs(basisQty) + price * q) / tot;
        m_wavgEntryQty = (basisQty > 0 ? 1.0 : -1.0) * tot;
    } else {
        const double remain = std::fabs(basisQty) - q;
        if (remain > 1e-9) {
            // pure reduction — average entry price unchanged
            m_wavgEntryQty = (basisQty > 0 ? 1.0 : -1.0) * remain;
        } else if (remain < -1e-9) {
            // crossed flat: residual opens a new basis on the other side
            m_wavgEntryQty = (signedFill > 0 ? 1.0 : -1.0) * (-remain);
            m_wavgEntryPx  = price;
        } else {
            // exactly flat
            m_wavgEntryQty = 0;
            m_wavgEntryPx  = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// B5: FIFO matching (quantbox FillDequeLedger semantics)
void PnlTracker::fifoMatch(bool isBuy, uint32_t qty, double price) {
    auto& opposite = isBuy ? m_fifoAsk : m_fifoBid;
    auto& own = isBuy ? m_fifoBid : m_fifoAsk;
    uint32_t remaining = qty;

    while (remaining > 0 && !opposite.empty()) {
        FillRec& head = opposite.front();
        uint32_t matched = std::min<uint32_t>(remaining, head.qty);
        double buyPx = isBuy ? price : head.px;
        double sellPx = isBuy ? head.px : price;
        m_fifoRealized += m_multiplier * matched * (sellPx - buyPx);
        remaining -= matched;
        if (head.qty <= matched) opposite.pop_front();
        else head.qty -= matched;
    }
    if (remaining > 0)
        own.push_back({price, remaining});
}

size_t PnlTracker::dumpLedgerCsv(const std::string& path) const {
    std::ofstream ofs(path, std::ios::app);
    if (!ofs.is_open()) return 0;
    size_t rows = 0;
    for (const auto& r : m_fifoBid) { ofs << "OPEN_BID," << r.px << ',' << r.qty << '\n'; rows++; }
    for (const auto& r : m_fifoAsk) { ofs << "OPEN_ASK," << r.px << ',' << r.qty << '\n'; rows++; }
    return rows;
}

void PnlTracker::initPosition(int32_t position, double costBasis) {
    m_lastpos = position;
    m_lastfillpx = costBasis;
    m_curpnl = m_lastpnl;
}

} // namespace wt_option
