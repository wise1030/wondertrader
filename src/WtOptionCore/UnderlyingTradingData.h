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
#include "OptionValues.h"                 // MultiMarket, EMAFilter, PriceSize
#include "InstrumentTradingDataBase.h"    // executor hooks, markets, OQM, counters
#include "PnlTracker.h"

#include <cstdint>
#include <cstring>
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

class UnderlyingTradingData : public InstrumentTradingDataBase
{
public:
    static const int32_t MAX_VALUES = 5;

public:
    UnderlyingTradingData(const std::string& code,
                          const ExpiryDataPtr& ed,
                          const OptionTradingGridPtr& otg);

    uint32_t getExpiry() const;
    int32_t getPosition() const;

    // Market access (replaces InstrumentMDContext / IBook)
    double getBid() const { return m_bid; }
    double getAsk() const { return m_ask; }
    double getMid() const { return (m_bid > 0 && m_ask > 0) ? (m_bid + m_ask) * 0.5 : 0.0; }
    void setMarket(double bid, double ask) { m_bid = bid; m_ask = ask; }
    void setMarket(double bid, double ask, uint64_t updateTime) {
        m_bid = bid; m_ask = ask;
        if (updateTime != 0) m_marketUpdateTime = updateTime;
    }
    uint64_t getMarketUpdateTime() const { return m_marketUpdateTime; }
    bool isMarketFresh(uint64_t nowMs, uint64_t staleTimeoutMs) const {
        if (staleTimeoutMs == 0) return true;
        if (m_marketUpdateTime == 0) return false;
        if (nowMs < m_marketUpdateTime) return true;
        return nowMs - m_marketUpdateTime <= staleTimeoutMs;
    }

    double getContractSize() const { return m_contractSize; }
    void setContractSize(double cs) {
        m_contractSize = cs;
        // Keep the lazy base-class PnlTracker in sync (it captures the
        // multiplier at creation — call before the first getPnlTracker()).
        setPnlMultiplier(cs);
    }
    void setFeePct(double feepct) { m_feePct = feepct; }
    double getFees(double price) const;

    double getTickSize() const { return m_tickSize; }
    void setTickSize(double ts) { m_tickSize = ts; }

    const MultiMarket& ourMarket() const { return values(0).ourMarket(); }
    MultiMarket& ourMarket() { return values(0).ourMarket(); }

    void setFwd(double v) { m_fwd = v; }
    double getFwd() const { return m_fwd; }

    double getDelta() const { return 1; }

    UnderlyingValues& values(int32_t i = 0) { return m_values[i]; }
    const UnderlyingValues& values(int32_t i = 0) const { return m_values[i]; }

    void setDisplayShark(bool b) { m_bDisplayShark = b; }
    bool isDisplayShark() const { return m_bDisplayShark; }

    const ExpiryDataPtr& getExpiryData() const { return m_spExpiryData; }
    /// Non-owning back-reference: OTG owns UTD, so a strong shared_ptr here
    /// creates an unloadable ownership cycle.
    OptionTradingGridPtr getOptionTradingGrid() const { return m_wpOptionTradingGrid.lock(); }

    // PnlTracker: inherited virtual from InstrumentTradingDataBase; the
    // contract-size multiplier is fed via setContractSize/setPnlMultiplier.

protected:
    UnderlyingValues m_values[MAX_VALUES];

    // Market (replaces InstrumentMDContext / IBook)
    double m_bid = 0;
    double m_ask = 0;
    uint64_t m_marketUpdateTime = 0;  // epoch ms, 0 = unknown
    double m_contractSize = 1.0;
    double m_tickSize = 1.0;
    double m_feePct = 0;

    double m_fwd = 0;

    bool m_bDisplayShark = false;
    ExpiryDataPtr m_spExpiryData;
    OptionTradingGridWeakPtr m_wpOptionTradingGrid;
};

using UnderlyingTradingDataPtr     = std::shared_ptr<UnderlyingTradingData>;
using UnderlyingTradingDataWeakPtr = std::weak_ptr<UnderlyingTradingData>;

} // namespace wt_option
