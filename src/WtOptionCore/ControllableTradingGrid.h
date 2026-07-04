/*!
 * \file ControllableTradingGrid.h
 * \brief Execution scheduling grid: refresh → combineMarkets → rank → TPS limit → updateOrders
 *
 * Migrated from quantbox optiontrading/ControllableTradingGrid (157h+976cc).
 * Preserves: refresh loop, rank-based priority sorting, TPS throttling, cancel_only mode.
 * Replaces: OptionTradingGrid inheritance → holds OptionGridPtr,
 *   CommandServicesHelper/IPositionListener/IOrderStatusListener → removed,
 *   notifiable<T> → plain T, TradingContext → OptionTraderContext struct.
 */
#pragma once

#include "optioncoretypes.h"
#include "IOptionGridListener.h"
#include "check_markets.h"

#include <string>
#include <vector>
#include <set>
#include <map>
#include <memory>
#include <functional>
#include <cstdint>

namespace wt_option {

class OptionGrid;
class OptionTradingGrid;
class OptionData;
class OptionTradingData;
class UnderlyingTradingData;
class IScanModule;
struct OptionTraderContext;

// Simplified trading context (replaces longbeach OptionTraderContext)
struct OptionTraderContext {
    bool enabled = false;
    bool panicked = false;
    std::function<double()> getTimeFn;
    double getTime() const { return getTimeFn ? getTimeFn() : 0; }
};

using OptionTraderContextPtr = std::shared_ptr<OptionTraderContext>;

// Executor callbacks (replaces trading::Order infrastructure)
// Quote request for async submission
struct PendingQuote {
    std::string code;
    double bidP = 0;
    uint32_t bidQ = 0;
    double askP = 0;
    uint32_t askQ = 0;
    bool isCancel = false;  // true = cancel only
};

using OrderExecutor = std::function<int32_t(const std::string& code, bool isBuy, double price, uint32_t qty)>;
using CancelExecutor = std::function<int32_t(const std::string& code)>;
using QuoteExecutor = std::function<int32_t(const std::string& code, double bidP, uint32_t bidQ, double askP, uint32_t askQ)>;

class ControllableTradingGrid : public OptionGridListener {
public:
    ControllableTradingGrid(OptionGridPtr grid, OptionTraderContextPtr ctx);
    virtual ~ControllableTradingGrid();

    // Set executors for order placement
    void setOrderExecutor(OrderExecutor exec) { m_orderExec = std::move(exec); }
    void setCancelExecutor(CancelExecutor exec) { m_cancelExec = std::move(exec); }
    void setQuoteExecutor(QuoteExecutor exec) { m_quoteExec = std::move(exec); }

    // TPS control
    int32_t getMaxTransactionsPerSec() const { return m_maxTransactionsPerSec; }
    void setMaxTransactionsPerSec(int32_t tps) { m_maxTransactionsPerSec = tps; }
    int32_t getTransactionCount() const { return m_txCount; }

    // Async quote submission — drain pending quotes (called from worker thread)
    void drainPendingQuotes();

    // Main refresh loop — called on each compute cycle
    void refresh();

    // GridListener interface
    virtual void onComputeValuesCompleted(const IOptionGrid* grid) override;
    virtual void onAddOption(const OptionDataPtr& od) override;
    virtual void onAddExpiry(const ExpiryDataPtr& ed) override;

    // Scanner registration
    void addScanner(std::shared_ptr<IScanModule> scanner);
    void removeScanner(std::shared_ptr<IScanModule> scanner);

    // Option hit callback (from scanners)
    void onOptionHit(OptionData* od, int32_t index);

    // Phase 8: Operations
    void tradingStopMidDay();
    void resumeTrading();
    bool onSetQMode(const std::string& code, const std::string& modeStr);

private:
    void onRefresh();
    int32_t updateOrders(
        const std::vector<std::shared_ptr<OptionTradingData>>& optList,
        const std::vector<std::shared_ptr<UnderlyingTradingData>>& udlList);

    int32_t rankOption(const std::shared_ptr<OptionTradingData>& otd, UPDATE_TYPE utype);
    int32_t rankFuture(const std::shared_ptr<UnderlyingTradingData>& utd, UPDATE_TYPE utype);

    // Combine desired markets from pricer with current order state
    void combineMarkets(const OptionData& od,
        std::vector<std::shared_ptr<OptionTradingData>>& outList);

    OptionGridPtr m_grid;
    OptionTraderContextPtr m_ctx;
    OptionTradingGrid* m_otg = nullptr;  // raw ptr to OTG (non-owning, set by strategy)

public:
    void setOTG(OptionTradingGrid* otg) { m_otg = otg; }
    OptionTradingGrid* getOTG() const { return m_otg; }

    // Executors
    OrderExecutor m_orderExec;
    CancelExecutor m_cancelExec;
    QuoteExecutor m_quoteExec;

    // TPS control
    int32_t m_maxTransactionsPerSec = 50;
    int32_t m_maxPanicTPS = 10;
    int32_t m_txCount = 0;
    double m_lastTransactionUpdateTime = 0;
    int32_t m_txDrop = 0;

    // Pending quote queue (collected in refresh, drained by worker)
    std::vector<PendingQuote> m_pendingQuotes;

    // Scanners
    std::vector<std::shared_ptr<IScanModule>> m_scanners;

    // Update tracking
    std::set<std::string> m_optUpdateSet;
    std::set<std::string> m_udlUpdateSet;
    std::map<std::string, bool> m_bUpdatedMap;

    int32_t m_traceLevel = 0;
};

using ControllableTradingGridPtr = std::shared_ptr<ControllableTradingGrid>;

} // namespace wt_option
