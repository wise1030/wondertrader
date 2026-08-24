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
#include <deque>
#include <fstream>

namespace wt_option {

class PnlTracker {
public:
    PnlTracker(double multiplier = 1.0);

    // B5: enable FIFO matching mode (quantbox FillDequeLedger absorption).
    // In FIFO mode every fill is matched against the head of the opposite
    // side's deque to realize PnL per matched pair, in addition to the
    // incremental aggregate path.
    void setFifoMode(bool on) { m_fifoMode = on; }
    bool isFifoMode() const { return m_fifoMode; }

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

    // B5: weighted average entry price (quantbox MarkerToMarket absorption).
    // Increases weighted by qty on adds; decreases at avg price on reduces;
    // resets when flat.
    double getWeightedEntryPrice() const { return m_wavgEntryPx; }

    // B5: realized PnL from FIFO matching only (0 when mode disabled)
    double getFifoRealizedPnl() const { return m_fifoRealized; }

    // B5/C5: dump session fill ledger (FIFO pairs) as CSV. Returns rows written.
    size_t dumpLedgerCsv(const std::string& path) const;

private:
    struct FillRec { double px; uint32_t qty; };
    void fifoMatch(bool isBuy, uint32_t qty, double price);
    void updateWeightedEntry(bool isBuy, uint32_t qty, double price);

    double m_multiplier;
    double m_feepct;
    double m_curturnover;
    double m_curpnl;     // current total PnL (realized + unrealized)
    double m_lastpnl;    // realized PnL
    int32_t m_lastpos;
    uint32_t m_fillsz;
    double m_lastfillpx;

    // B5: FIFO matching state
    bool m_fifoMode = false;
    std::deque<FillRec> m_fifoBid;   // open longs awaiting match
    std::deque<FillRec> m_fifoAsk;   // open shorts awaiting match
    double m_fifoRealized = 0;

    // B5: weighted entry price
    double m_wavgEntryQty = 0;   // signed basis quantity backing the average
    double m_wavgEntryPx = 0;
};

using PnlTrackerPtr = std::shared_ptr<PnlTracker>;

} // namespace wt_option
