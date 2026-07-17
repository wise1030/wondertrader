/*!
 * \file PnlTracker.h
 * \brief Per-instrument PnL tracker — tick-by-tick mark-to-mid
 *
 * Migrated from quantbox optioncore/PnlTracker.h (52 lines).
 * Business logic preserved: onBookChanged → unrealized = lastpnl + mult*pos*(mid-lastfillpx);
 * onFill → realized += mult*pos*(fillpx-lastfillpx) - fees.
 * Replaces: IBookListener → onPriceUpdate(bid,ask), IFillListener → onFill(isBuy,qty,price).
 */
#pragma once

#include <cstdint>
#include <string>
#include <memory>

namespace wt_option {

class PnlTracker {
public:
    PnlTracker(double multiplier = 1.0);

    // Set fee percentage (e.g. 0.0001 = 1bp)
    void setFeePct(double feepct) { m_feepct = feepct; }

    // Call on every tick/book update (unrealized PnL)
    void onPriceUpdate(double bid, double ask);

    // Call on every fill (realized PnL)
    void onFill(bool isBuy, uint32_t qty, double price);

    // Initialize overnight position with previous close as cost basis
    void initPosition(int32_t position, double costBasis);

    // Queries
    double getCurTurnover() const { return m_curturnover; }
    double getCurPnl() const { return m_curpnl; }
    double getRealizedPnl() const { return m_lastpnl; }
    uint32_t getFillSize() const { return m_fillsz; }
    int32_t getPosition() const { return m_lastpos; }
    double getLastFillPrice() const { return m_lastfillpx; }

private:
    double m_multiplier;
    double m_feepct;
    double m_curturnover;
    double m_curpnl;     // current total PnL (realized + unrealized)
    double m_lastpnl;    // realized PnL
    int32_t m_lastpos;
    uint32_t m_fillsz;
    double m_lastfillpx;
};

using PnlTrackerPtr = std::shared_ptr<PnlTracker>;

} // namespace wt_option
