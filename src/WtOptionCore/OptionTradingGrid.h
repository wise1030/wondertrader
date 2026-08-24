/*!
 * \file OptionTradingGrid.h
 * \brief Trading object lifecycle manager (middle layer of 3-tier architecture)
 *
 * Migrated from quantbox optioncore/OptionTradingGrid.h (184 lines).
 * Business logic preserved: onAddOption creates OTD/ETD/UTD, manages OptionRisk,
 * delegates computeValues to OptionGrid.
 *
 * Architecture (composition replaces quantbox inheritance):
 *   OptionGrid (data) ← held by → OptionTradingGrid (trading objects)
 *   OptionTradingGrid ← held by → ControllableTradingGrid (execution)
 *
 * Key responsibilities:
 * 1. onAddOption: create OptionTradingData + bind executors + bind RiskData
 * 2. onAddExpiry: create ExpiryTradingData + UnderlyingTradingData
 * 3. Hold OptionRisk reference
 * 4. Delegate computeValues to OptionGrid
 * 5. Provide accessors: getTradingData, getUnderlyingTradingData, getExpiryTradingData
 */
#pragma once

#include "optioncoretypes.h"
#include "OptionGrid.h"
#include "OptionData.h"
#include "OptionTradingData.h"
#include "UnderlyingTradingData.h"
#include "ExpiryTradingData.h"
#include "IOptionGridListener.h"
#include "IOptionPricer.h"
#include "OptionQuoteManager.h"

// Forward declaration — IHftStraCtx defined in WT Includes
namespace wtp { class IHftStraCtx; }

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <functional>

namespace wt_option {

class OptionRisk;
using OptionRiskPtr = std::shared_ptr<OptionRisk>;

// Executor types (shared with CTG, set on OTD/UTD at creation)
using QuoteExecutor  = std::function<int32_t(const std::string& code, double bidP, uint32_t bidQ, double askP, uint32_t askQ)>;
using OrderExecutor  = std::function<int32_t(const std::string& code, bool isBuy, double price, uint32_t qty)>;
using CancelExecutor = std::function<int32_t(const std::string& code)>;

class OptionTradingGrid : public IOptionGridListener, public std::enable_shared_from_this<OptionTradingGrid>
{
public:
    OptionTradingGrid(OptionGridPtr grid);
    ~OptionTradingGrid() override = default;

    // --- IOptionGridListener (receives events from OptionGrid) ---
    void onAddOption(const OptionDataPtr& od) override;
    void onAddExpiry(const ExpiryDataPtr& ed) override;
    void onComputeValuesCompleted(const IOptionGrid* grid) override;

    // --- Executor binding (called by strategy before ticks arrive) ---
    void setQuoteExecutor(QuoteExecutor exec)   { m_quoteExec  = std::move(exec); }
    void setOrderExecutor(OrderExecutor exec)   { m_orderExec  = std::move(exec); }
    void setCancelExecutor(CancelExecutor exec) { m_cancelExec = std::move(exec); }
    void setExchange(const std::string& exchg)  { m_exchange = exchg; }
    void setHftCtx(wtp::IHftStraCtx* ctx)            { m_hftCtx = ctx; }

    // P11: Per-instrument-type OQM config (option vs future)
    void setOptionOQMConfig(const OptionQuoteManager::Config& cfg) { m_optionOqmCfg = cfg; }
    void setFutureOQMConfig(const OptionQuoteManager::Config& cfg) { m_futureOqmCfg = cfg; }
    void setHedgeOverride(uint32_t exp, const std::string& hedgeCode) { m_hedgeOverrides[exp] = hedgeCode; }
    void setQuoteStatistics(wt_option::QuoteStatistics* qs) { m_quoteStats = qs; }

    /// B11: position source for OTD::getPosition(). Injected by the strategy
    /// (reads its worker-thread-owned position map). Without it, QM_AUTO↔
    /// QM_CLOSE and auto-close logic see a permanent zero.
    using PositionSourceFn = std::function<int32_t(const std::string& code)>;
    void setPositionSourceFn(PositionSourceFn fn) { m_positionSource = std::move(fn); }

    /// A2: pre-trade limit checker applied to every OQM at creation time
    void setPreTradeCheckFn(OptionQuoteManager::PreTradeCheckFn fn) { m_preTradeFn = std::move(fn); }
    /// B5: FIFO PnL matching mode for newly created trackers
    void setFifoPnlMode(bool on) { m_fifoPnl = on; }

    /// B07: reset all OQM lifetime counters (call on session begin)
    void onSessionBegin();
    /// C5: dump all OQM lifetime counters to CSV (call on session end)
    void dumpCountersCsv(const std::string& path) const;

    // --- OptionRisk ---
    const OptionRiskPtr& getPositionRisk() const { return m_spPositionRisk; }
    void setPositionRisk(OptionRiskPtr risk) { m_spPositionRisk = std::move(risk); }

    // --- Accessors ---
    OptionGridPtr getOptionGrid() const { return m_spGrid; }

    OptionTradingDataPtr getTradingData(const std::string& code) const;
    OptionTradingDataPtr getTradingData(uint32_t exp, strike_t stk, OptionRight right) const;

    UnderlyingTradingDataPtr getUnderlyingTradingData(const std::string& code) const;
    const std::map<std::string, UnderlyingTradingDataPtr>& getAllUnderlyingTradingData() const { return m_tblUnderlyingTradingData; }
    UnderlyingTradingDataPtr getFrontMonthTradingData() const { return m_spFrontMonthTradingData; }
    void setFrontMonthTradingData(UnderlyingTradingDataPtr utd) { m_spFrontMonthTradingData = std::move(utd); }

    ExpiryTradingDataPtr getExpiryTradingData(uint32_t exp) const;

    // --- Compute delegation ---
    void computeValues(IOptionPricer* pricer);

    // --- Enable/disable all trading data ---
    void enableAll();
    void disableAll();

private:
    ExpiryTradingDataPtr __createExpiryTradingData(const ExpiryDataPtr& ed);

    OptionGridPtr m_spGrid;
    OptionRiskPtr m_spPositionRisk;

    // Executor hooks (from strategy, bound to each OTD/UTD at creation)
    QuoteExecutor  m_quoteExec;
    OrderExecutor  m_orderExec;
    CancelExecutor m_cancelExec;
    std::string    m_exchange;
    wtp::IHftStraCtx*   m_hftCtx = nullptr;
    // P11: Per-instrument-type OQM config
    OptionQuoteManager::Config m_optionOqmCfg;
    OptionQuoteManager::Config m_futureOqmCfg;
    wt_option::QuoteStatistics* m_quoteStats = nullptr;  // non-owning
    PositionSourceFn m_positionSource;                    // B11
    OptionQuoteManager::PreTradeCheckFn m_preTradeFn;     // A2
    bool m_fifoPnl = false;                               // B5

    // Trading object tables
    typedef std::map<std::string, UnderlyingTradingDataPtr> UnderlyingTable;
    UnderlyingTable m_tblUnderlyingTradingData;

    typedef std::map<uint32_t, ExpiryTradingDataPtr> ExpiryTable;
    ExpiryTable m_tblExpiryTradingData;
    std::map<uint32_t, std::string> m_hedgeOverrides;

    UnderlyingTradingDataPtr m_spFrontMonthTradingData;
};

using OptionTradingGridPtr = std::shared_ptr<OptionTradingGrid>;

} // namespace wt_option
