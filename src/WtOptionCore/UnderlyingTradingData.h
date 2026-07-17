/*!
 * \file UnderlyingTradingData.h
 * \brief Per-underlying (future) trading state + order executor hooks
 *
 * Migrated from quantbox optioncore/UnderlyingTradingData.h + .cc (184 + 131 lines).
 * Business logic preserved: UnderlyingValues multi-value table, Adjustments/Alphas/
 * Prices structs, EMA filters, multiMarket, getLastDesiredMarket/getCurrentMarket,
 * setActive/enable/disable, updateOrders, forward, updateRank, quoteMode,
 * displayShark, PnlTracker lazy init.
 *
 * Dependency replacements (same scheme as OptionTradingData):
 *  - longbeach IOrderManager/ITrader → std::function executors (OTDOrderExecutor /
 *    OTDCancelExecutor / OTDQuoteExecutor from OptionTradingData.h).
 *  - TradingContext/InstrumentContext/ITrader/InstrumentMDContext → deleted;
 *    underlying code + market + fees provided externally.
 *  - trading::OrderPtr → fire-and-forget executor calls.
 *  - Subscription/IFillListener/IOrderStatusListener/Priority → deleted.
 *  - instrument_t → std::string (the future/underlying code).
 *  - PnlTracker now takes only a multiplier (migrated variant).
 */
#pragma once

#include "optioncoretypes.h"
#include "OptionValues.h"      // MultiMarket, EMAFilter, PriceSize
#include "OptionTradingData.h" // OTDOrderExecutor / OTDCancelExecutor / OTDQuoteExecutor / PositionProvider
#include "PnlTracker.h"

#include <cstdint>
#include <string>
#include <functional>
#include <memory>

namespace wt_option {

class ExpiryData;
class OptionTradingGrid;
using OptionTradingGridPtr     = std::shared_ptr<OptionTradingGrid>;
using OptionTradingGridWeakPtr = std::weak_ptr<OptionTradingGrid>;

class UnderlyingValues
{
public:
    UnderlyingValues()
        : m_adj()
        , m_alpha()
    {}

    MultiMarket& ourMarket() { return m_ourMarket; }
    const MultiMarket& ourMarket() const { return m_ourMarket; }

    struct Adjustments
    {
        double risk;
        double bump;
        double total;
        void clear()
        {
            memset(this, 0, sizeof(*this));
            total = NAN;
        }
    };
    Adjustments& adj() { return m_adj; }
    const Adjustments& adj() const { return m_adj; }

    struct Alphas
    {
        double atmsig;
        double sizebias;
        double tradebias;
        double rollema;
        double total;
        void clear()
        {
            memset(this, 0, sizeof(*this));
            total = NAN;
        }
    };
    Alphas& alpha() { return m_alpha; }
    const Alphas& alpha() const { return m_alpha; }

    struct Prices
    {
    public:
        Prices() { clear(); }
        void clear()
        {
            mid = bid = ask = 0;
        }
        double mid, bid, ask;
    };
    /// theoretical prices
    const Prices& theoPrices() const { return m_theoPrices; }
    Prices& theoPrices() { return m_theoPrices; }

    EMAFilter& ema() { return m_ema; }
    EMAFilter& ema_tradebias() { return m_ema_tradebias; }
    EMAFilter& ema_rollema() { return m_ema_rollema; }

private:
    MultiMarket m_ourMarket;
    Adjustments m_adj;
    Alphas      m_alpha;
    Prices      m_theoPrices;

    EMAFilter m_ema;
    EMAFilter m_ema_tradebias;
    EMAFilter m_ema_rollema;
};

class UnderlyingTradingData
{
public:
    static const int32_t MAX_VALUES = 5;

public:
    UnderlyingTradingData(const std::string& code,
                          const ExpiryDataPtr& ed,
                          const OptionTradingGridPtr& otg);

    const std::string& getCode() const { return m_instr; }
    uint32_t getExpiry() const;
    int32_t getPosition() const;

    MultiMarket& multiMarket() { return m_multiMarket; }
    const MultiMarket& getLastDesiredMarket() const { return m_lastDesiredMarket; }
    const MultiMarket& getCurrentMarket() const     { return m_currentMarket; }
    void setCurrentMarket(const MultiMarket& mkt) { m_currentMarket = mkt; }
    int32_t updateOrders(bool cancel_only = false);

    // Per-contract QuoteManager (same pattern as OptionTradingData)
    void setQuoteManager(std::shared_ptr<class OptionQuoteManager> om) { m_quoteOM = std::move(om); }
    std::shared_ptr<class OptionQuoteManager> getQuoteManager() const { return m_quoteOM; }

    // Executor hooks (replaces IOrderManager / ITrader) — same semantics as OptionTradingData.
    void setOrderExecutor(OTDOrderExecutor oe)   { m_orderExecutor  = std::move(oe); }
    void setCancelExecutor(OTDCancelExecutor ce) { m_cancelExecutor = std::move(ce); }
    void setQuoteExecutor(OTDQuoteExecutor qe)   { m_quoteExecutor  = std::move(qe); }
    void setPositionProvider(PositionProvider pp) { m_positionProvider = std::move(pp); }

    bool isActive() const;
    void setActive(bool b);
    void enable();
    void disable();

    // Market access (replaces InstrumentMDContext / IBook)
    double getBid() const { return m_bid; }
    double getAsk() const { return m_ask; }
    double getMid() const { return (m_bid > 0 && m_ask > 0) ? (m_bid + m_ask) * 0.5 : 0.0; }
    void setMarket(double bid, double ask) { m_bid = bid; m_ask = ask; }

    double getContractSize() const { return m_contractSize; }
    void setContractSize(double cs) { m_contractSize = cs; }
    void setFeePct(double feepct) { m_feePct = feepct; }
    double getFees(double price) const;

    double getTickSize() const { return m_tickSize; }
    void setTickSize(double ts) { m_tickSize = ts; }

    const MultiMarket& ourMarket() const { return values(0).ourMarket(); }
    MultiMarket& ourMarket() { return values(0).ourMarket(); }

    int32_t getNumCancel() const { return m_numCancel; }
    int32_t getNumReject() const { return m_numReject; }
    int32_t getNumFill() const   { return m_numFill; }

    void setFwd(double v) { m_fwd = v; }
    double getFwd() const { return m_fwd; }

    void setUpdateRank(double r) { m_updateRank = r; }
    double getUpdateRank() const { return m_updateRank; }
    double getDelta() const { return 1; }

    UnderlyingValues& values(int32_t i = 0) { return m_values[i]; }
    const UnderlyingValues& values(int32_t i = 0) const { return m_values[i]; }

    void setQuoteMode(QuoteMode m) { m_quoteMode = m; }
    QuoteMode getQuoteMode() const { return m_quoteMode; }

    void setDisplayShark(bool b) { m_bDisplayShark = b; }
    bool isDisplayShark() const { return m_bDisplayShark; }

    const ExpiryDataPtr& getExpiryData() const { return m_spExpiryData; }
    const OptionTradingGridPtr& getOptionTradingGrid() const { return m_spOptionTradingGrid; }

    PnlTrackerPtr getPnlTracker();

protected:
    UnderlyingValues m_values[MAX_VALUES];
    MultiMarket m_multiMarket;
    MultiMarket m_lastDesiredMarket;
    MultiMarket m_currentMarket;

    // Executor callbacks (replace IOrderManager / ITrader)
    OTDOrderExecutor   m_orderExecutor;
    OTDCancelExecutor  m_cancelExecutor;
    OTDQuoteExecutor   m_quoteExecutor;
    PositionProvider m_positionProvider;

    bool m_bActive;

    std::shared_ptr<class OptionQuoteManager> m_quoteOM;

    // Market (replaces InstrumentMDContext / IBook)
    double m_bid = 0;
    double m_ask = 0;
    double m_contractSize = 1.0;
    double m_tickSize = 1.0;
    double m_feePct = 0;

    double m_fwd;
    double m_updateRank;

    QuoteMode m_quoteMode;
    bool m_bDisplayShark;
    std::string m_instr;
    ExpiryDataPtr m_spExpiryData;
    OptionTradingGridPtr m_spOptionTradingGrid;

    PnlTrackerPtr m_pnlTracker;

    // Counters
    int32_t m_numCancel = 0;
    int32_t m_numReject = 0;
    int32_t m_numFill   = 0;
};

using UnderlyingTradingDataPtr     = std::shared_ptr<UnderlyingTradingData>;
using UnderlyingTradingDataWeakPtr = std::weak_ptr<UnderlyingTradingData>;

} // namespace wt_option
