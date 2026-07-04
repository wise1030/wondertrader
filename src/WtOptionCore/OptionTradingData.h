/*!
 * \file OptionTradingData.h
 * \brief Per-option trading state + order executor hooks
 *
 * Migrated from quantbox optioncore/OptionTradingData.h + .cc (129 + 201 lines).
 * Business logic preserved: quote mode (AUTO/ON/OFF/CLOSE/FLAT), updateRank,
 * willBeActive delta/size gating, str2qmode, onMarketsPriced no-op hook,
 * setActive clears multiMarket, enable/disable, updateOrders.
 *
 * Dependency replacements:
 *  - longbeach IOrderManager/IOrderManagerFactory/ITrader → std::function executors:
 *      OTDOrderExecutor  (single-leg new order)
 *      OTDCancelExecutor (cancel by orderId)
 *      OTDQuoteExecutor  (two-legged quote, returns orderId)
 *  - TradingContext/InstrumentContext/ITrader → deleted; executors set externally.
 *  - InstrumentMDContext/IBook → deleted; market now read from OptionData::getMarket().
 *  - trading::OrderPtr → no internal order objects; executor calls are fire-and-forget.
 *  - OptionRiskData dependency on getPosition() → optional external position provider.
 *  - instrument_t → std::string (the option code).
 *  - Subscription/IFillListener/Priority → deleted (WT uses its own callback path).
 */
#pragma once

#include "optioncoretypes.h"
#include "OptionValues.h"
#include "OptionData.h"

#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include <optional>

namespace wt_option {

// Executor callback types (replace longbeach IOrderManager / ITrader / ShfeOrder)
// These are per-instrument executors set on each OptionTradingData.
// isBuy=true → buy order at price for qty; returns true on successful submit.
using OTDOrderExecutor  = std::function<bool(bool isBuy, double price, uint32_t qty)>;
// Cancel an order by its id; returns true on successful cancel submit.
using OTDCancelExecutor = std::function<bool(uint32_t orderId)>;
// Submit a two-legged quote (bid price/qty, ask price/qty); returns an orderId (0 on failure).
using OTDQuoteExecutor  = std::function<uint32_t(double bidP, uint32_t bidQ, double askP, uint32_t askQ)>;
// External position provider (replaces InstrumentContext PositionProvider).
using PositionProvider = std::function<int32_t()>;

class OptionRiskData;
using OptionRiskDataPtr    = std::shared_ptr<OptionRiskData>;
using OptionRiskDataWeakPtr = std::weak_ptr<OptionRiskData>;

class OptionTradingData
{
public:
    OptionTradingData();

    // Identity
    const std::string& getCode() const { return m_instr; }
    uint32_t getExpiry() const;
    ExpiryDataPtr getExpiryData() const;
    OptionDataPtr getOptionData() const { return m_parent.lock(); }
    OptionRiskDataPtr getOptionRiskData() const { return m_spOptionRiskData.lock(); }

    /// get position with offsets (provided externally)
    int32_t getPosition() const;
    /// raw position (provided externally)
    int32_t getAbsolutePosition() const;

    // External position provider (replaces InstrumentContext PositionProvider)
    void setPositionProvider(PositionProvider pp) { m_positionProvider = std::move(pp); }

    /// active flag indicates whether this OptionTradingData is trading.
    /// Toggling does NOT affect live orders; call updateOrders() to realize.
    /// To be conservative, multiMarket() is cleared when set to false.
    bool isActive() const;
    void setActive(bool b);

    MultiMarket& multiMarket() { return m_multiMarket; }
    const MultiMarket& getLastDesiredMarket() const { return m_lastDesiredMarket; }
    const MultiMarket& getCurrentMarket() const { return m_currentMarket; }
    void setCurrentMarket(const MultiMarket& mkt) { m_currentMarket = mkt; }

    OptionValues& values(size_t idx = 0);
    const OptionValues& values(size_t idx = 0) const;

    // Executor hooks (replaces IOrderManager / ITrader)
    void setOrderExecutor(OTDOrderExecutor oe)   { m_orderExecutor  = std::move(oe); }
    void setCancelExecutor(OTDCancelExecutor ce) { m_cancelExecutor = std::move(ce); }
    void setQuoteExecutor(OTDQuoteExecutor qe)   { m_quoteExecutor  = std::move(qe); }

    // Per-contract OptionQuoteManager (Phase 2: complete order lifecycle)
    void setQuoteManager(std::shared_ptr<class OptionQuoteManager> om) { m_quoteOM = std::move(om); }
    std::shared_ptr<class OptionQuoteManager> getQuoteManager() const { return m_quoteOM; }

    /// Starts quoting: setActive(true). Executors must be set beforehand.
    void enable();
    /// Stops quoting: setActive(false) and clears multiMarket.
    void disable();
    bool enabled() const { return isActive(); }

    /// Realize desired market → executors. Returns number of transactions.
    /// If cancel_only=true, only cancels are issued (no new orders/quotes).
    int32_t updateOrders(bool cancel_only = false);

    // Counters (maintained by updateOrders; previously from IOrderManager)
    int32_t getNumCancel() const { return m_numCancel; }
    int32_t getNumReject() const { return m_numReject; }
    int32_t getNumFill() const   { return m_numFill; }

    void setUpdateRank(double r) { m_updateRank = r; }
    double getUpdateRank() const { return m_updateRank; }
    double getDelta() const;

    void setQuoteMode(QuoteMode m) { m_quoteMode = m; }
    QuoteMode getQuoteMode() const { return m_quoteMode; }

    bool willBeActive(double delta_min, double delta_max, int32_t big_size);

    static std::optional<QuoteMode> str2qmode(const std::string& s);

    // Initialization (replaces init(TradingContext, OptionData))
    void init(const OptionDataPtr& od);

protected:
    friend class OptionTradingGrid;
    virtual void onMarketsPriced(const OptionData& od, size_t index);

protected:
    bool m_bActive;

    std::string m_instr;
    OptionDataWeakPtr m_parent;
    OptionRiskDataWeakPtr m_spOptionRiskData;

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

    // Per-contract QuoteManager (Phase 2)
    std::shared_ptr<class OptionQuoteManager> m_quoteOM;

    double m_updateRank;
    QuoteMode m_quoteMode;
};

using OptionTradingDataPtr     = std::shared_ptr<OptionTradingData>;
using OptionTradingDataWeakPtr = std::weak_ptr<OptionTradingData>;

} // namespace wt_option
