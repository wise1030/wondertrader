#pragma once
/*!
 * \file InstrumentTradingDataBase.h
 * \brief Common execution-state core of OptionTradingData / UnderlyingTradingData.
 *
 * Convergence (plan 3.1): the two per-instrument trading classes carried
 * byte-identical execution state — desired/current/last markets, the three
 * executor hooks, the per-contract QuoteManager, lifetime counters, the
 * active flag machinery and the lazy PnlTracker.  They drifted once already
 * (OTD::updateOrders additionally syncs OptionData::currentMarket).  This
 * base owns the common core once; the hook below is the single sanctioned
 * difference point.
 *
 * What deliberately stays in the derived classes: the typed values tables
 * (OptionValues vs UnderlyingValues), market access (OTD reads OptionData,
 * UTD holds its own bid/ask), position sources, and UTD-only facts
 * (fee/contractSize/tick/fwd).
 */
#pragma once

#include "optioncoretypes.h"
#include "OptionValues.h"      // MultiMarket
#include "OptionQuoteManager.h"
#include "PnlTracker.h"

#include <cstdint>
#include <string>
#include <functional>
#include <memory>

namespace wt_option {

// Executor callback types (replace longbeach IOrderManager / ITrader / ShfeOrder)
// These are per-instrument executors set on each trading-data object.
// isBuy=true → buy order at price for qty; returns true on successful submit.
using OTDOrderExecutor  = std::function<bool(bool isBuy, double price, uint32_t qty)>;
// Cancel an order by its id; returns true on successful cancel submit.
using OTDCancelExecutor = std::function<bool(uint32_t orderId)>;
// Submit a two-legged quote (bid price/qty, ask price/qty); returns an orderId (0 on failure).
using OTDQuoteExecutor  = std::function<uint32_t(double bidP, uint32_t bidQ, double askP, uint32_t askQ)>;
// External position provider (replaces InstrumentContext PositionProvider).
using PositionProvider = std::function<int32_t()>;

class InstrumentTradingDataBase
{
public:
    // Virtual hook below => polymorphic base; a virtual dtor is required so
    // deletion through a base pointer is well-defined.
    virtual ~InstrumentTradingDataBase() = default;

    const std::string& getCode() const { return m_instr; }

    /// active flag indicates whether this instrument is trading.
    /// Toggling does NOT affect live orders; call updateOrders() to realize.
    /// To be conservative, multiMarket() is cleared when set to false.
    /// The flag is propagated to the QuoteManager (OQM suppresses non-cancel
    /// work while inactive — cancel_only passes still run).
    bool isActive() const { return m_bActive; }
    void setActive(bool b) {
        m_bActive = b;
        if (!b) m_multiMarket.clear();
        if (m_quoteOM) m_quoteOM->setActive(b);
    }
    void enable()  { setActive(true); }
    void disable() { setActive(false); }
    bool enabled() const { return isActive(); }

    MultiMarket& multiMarket() { return m_multiMarket; }
    const MultiMarket& getLastDesiredMarket() const { return m_lastDesiredMarket; }
    const MultiMarket& getCurrentMarket() const     { return m_currentMarket; }
    void setCurrentMarket(const MultiMarket& mkt) { m_currentMarket = mkt; }

    // Executor hooks (replaces IOrderManager / ITrader)
    void setOrderExecutor(OTDOrderExecutor oe)   { m_orderExecutor  = std::move(oe); }
    void setCancelExecutor(OTDCancelExecutor ce) { m_cancelExecutor = std::move(ce); }
    void setQuoteExecutor(OTDQuoteExecutor qe)   { m_quoteExecutor  = std::move(qe); }
    void setPositionProvider(PositionProvider pp) { m_positionProvider = std::move(pp); }

    // Per-contract QuoteManager (same pattern for options and futures)
    void setQuoteManager(std::shared_ptr<class OptionQuoteManager> om) { m_quoteOM = std::move(om); }
    std::shared_ptr<class OptionQuoteManager> getQuoteManager() const { return m_quoteOM; }

    /// Realize desired market → quote manager. Returns number of transactions.
    /// If cancel_only=true, only cancels are issued (no new orders/quotes).
    int32_t updateOrders(bool cancel_only = false) {
        if (!m_quoteOM) return 0;

        int32_t txns = m_quoteOM->updateOrders(m_multiMarket, cancel_only);
        m_lastDesiredMarket = m_multiMarket;
        if (cancel_only) {
            m_currentMarket.clear();
        } else if (txns > 0) {
            m_currentMarket = m_multiMarket;
        }
        onOrdersRealized(txns, cancel_only);
        return txns;
    }

    // Counters
    int32_t getNumCancel() const { return m_numCancel; }
    int32_t getNumReject() const { return m_numReject; }
    int32_t getNumFill() const   { return m_numFill; }

    /// Virtual so UnderlyingTradingData can supply its contract size through
    /// the base interface (its old override HID this method — a call via a
    /// base pointer silently built a multiplier-1 tracker).  The multiplier
    /// is captured lazily at tracker creation; setPnlMultiplier() must run
    /// before the first getPnlTracker() call.
    virtual PnlTrackerPtr getPnlTracker() {
        if (!m_pnlTracker)
            m_pnlTracker = std::make_shared<PnlTracker>(m_pnlMultiplier);
        return m_pnlTracker;
    }

    void setUpdateRank(double r) { m_updateRank = r; }
    double getUpdateRank() const { return m_updateRank; }

    void setQuoteMode(QuoteMode m) { m_quoteMode = m; }
    QuoteMode getQuoteMode() const { return m_quoteMode; }

protected:
    friend class OptionTradingGrid;

    /// Contract multiplier for the lazy PnlTracker (UTD sets its contract
    /// size here; options keep the 1.0 default).
    void setPnlMultiplier(double m) { m_pnlMultiplier = m; }

    /// Single divergence hook after updateOrders(): OptionTradingData syncs
    /// its OptionData::currentMarket mirror; UnderlyingTradingData needs
    /// nothing.  Never issue orders here.
    virtual void onOrdersRealized(int32_t /*txns*/, bool /*cancel_only*/) {}

    bool m_bActive = false;

    std::string m_instr;

    MultiMarket m_multiMarket;
    MultiMarket m_lastDesiredMarket;
    MultiMarket m_currentMarket;

    // Executor callbacks (replace IOrderManager / ITrader)
    OTDOrderExecutor  m_orderExecutor;
    OTDCancelExecutor m_cancelExecutor;
    OTDQuoteExecutor  m_quoteExecutor;
    PositionProvider m_positionProvider;

    // Counters
    int32_t m_numCancel = 0;
    int32_t m_numReject = 0;
    int32_t m_numFill   = 0;

    std::shared_ptr<class OptionQuoteManager> m_quoteOM;

    double m_updateRank = 0;
    QuoteMode m_quoteMode = static_cast<QuoteMode>(0);

    PnlTrackerPtr m_pnlTracker;
    double m_pnlMultiplier = 1.0;
};

} // namespace wt_option
