/*!
 * \file OptionQuoteManager.h
 * \brief Per-contract order lifecycle manager
 *
 * Migrated from quantbox QuoteOrderManager(760L) + DefaultOrderManager(1607L).
 * Enhanced with: incremental diffing, cancel throttle, cautious flipping,
 * scale factor, wait-for-cancels, PositionGuard/PositionOffsetMgr integration.
 *
 * OQM is framework-agnostic: all exchange operations must be injected through
 * OrderGateway sender functions by OptionTradingGrid.
 */
#pragma once

#include "OptionValues.h"    // MultiMarket, PriceSize
#include "RiskFilterChain.h"
#include "QuoteStatistics.h"

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <memory>

namespace wt_option {
class PositionOffsetMgr;
class PositionGuard;
using PositionOffsetMgrPtr = std::shared_ptr<PositionOffsetMgr>;
using PositionGuardPtr = std::shared_ptr<PositionGuard>;
}

namespace wt_option {

class OptionQuoteManager {
public:
    struct Config {
        // --- 基础报单参数 ---
        uint32_t max_side_orders = 1;
        uint32_t max_position = 50;
        double time_in_force_ms = 45000;
        bool enable_quote_api = true;
        bool avoid_trade = false;
        std::string exchange = "SHFE";
        double tick_size = 0.5;

        // --- DefaultOM 功能补齐 ---
        bool check_potential_position = false;
        bool leave_outer_orders = true;
        bool wait_for_cancels = false;
        double order_delay_ms = 0;
        // Cancel-ack watchdog: a lost cancel callback must not freeze a side forever.
        double cancel_ack_timeout_ms = 2000;   // 0 = disabled
        uint32_t cancel_max_retries = 3;       // re-sends before declaring stuck

        // --- 撤单限制 (三层) ---
        int32_t max_cancels_allowed = 0;      // hard limit (0=unlimited)
        int32_t cancel_buffer = 0;            // buffer below hard (true max = max - buffer)
        int32_t cancel_soft_max = 0;           // soft limit (warn only)
        int32_t hard_flat_after_n_fills = 0;
        int32_t reject_max_new_orders = -1;

        // --- 增强 (借鉴 DefaultOrderManager) ---
        double min_intra_update_period_ms = 0; // min ms between updateOrders (0=no limit)
        bool cautious_flipping = false;        // block orders that would flip position
        double scale_factor = 1.0;             // runtime order size multiplier (0-1)
        bool enable_ioc = false;                // IOC orders for quick close

        // --- A4: exchange quoting style (quantbox QuoteOrderManager absorption) ---
        enum QuoteStyle { QS_PAIRED = 0, QS_BUYSELL = 1 };
        QuoteStyle quote_style = QS_PAIRED;
        // SHFE/INE: close-side orders are capped by closeable volume
        bool enable_offset_guard = false;

        // B3: right flag for OptionsShortLimitFilter (0=Put, 1=Call, 2=N/A)
        uint8_t right_flag = 2;
    };

    using GetTimeFn = std::function<double()>;

    OptionQuoteManager(const std::string& code, const Config& cfg);
    ~OptionQuoteManager() = default;

    // --- Core: update desired market -> send orders ---
    int32_t updateOrders(const MultiMarket& desired, bool cancel_only = false);

    // --- Callbacks (from strategy on_order/on_trade) ---
    void onOrderStatusChange(uint32_t localid, bool isLong,
                              double totalQty, double leftQty,
                              double price, bool isCanceled);
    // P1-1: entrust failure (WT sends NO terminal on_order afterwards) —
    // clears the phantom OrderState and arms the reject retry, otherwise the
    // price level stops quoting forever.  Returns true if this OQM owned it.
    bool onOrderRejected(uint32_t localid);
    void onFill(uint32_t localid, bool isLong, double fill_px, uint32_t fill_qty);

    // --- Position ---
    void setPosition(int32_t pos) { m_position = pos; }
    int32_t getPosition() const { return m_position; }

    // --- External module setters ---
    void setRiskFilterChain(RiskFilterChainPtr chain) { m_filterChain = chain; }
    void setPositionOffsetMgr(PositionOffsetMgrPtr mgr) { m_positionOffset = mgr; }
    void setPositionGuard(PositionGuardPtr guard) { m_positionGuard = guard; }
    void setQuoteStatistics(QuoteStatistics* qs) { m_quoteStats = qs; }
    void setTickTimestampUs(uint64_t ts) { m_tickTimestampUs = ts; }
    void setCurrentTime(uint32_t hhmm, uint32_t secInMin) { m_timeHHMM = hhmm; m_secInMin = secInMin; }

    /// A2: pre-trade limit checker (RiskLimitsEx integration), runs BEFORE the
    /// filter chain. Return false to block the side. qty may be adjusted down.
    using PreTradeCheckFn = std::function<bool(const std::string& code, bool isBuy,
                                                double price, uint32_t& qty,
                                                int32_t currentPosition,
                                                std::string& reason)>;
    void setPreTradeCheckFn(PreTradeCheckFn fn) { m_preTradeCheck = std::move(fn); }

    /// A4: injectable single-leg sender (default: stra_buy/stra_sell via ctx).
    /// Injectable for unit tests.
    using SendSingleFn = std::function<uint32_t(bool isBuy, double price, uint32_t qty)>;
    void setSendSingleFn(SendSingleFn fn) { m_sendSingle = std::move(fn); }

    /// Injectable paired-quote sender.  Production supplies the strategy
    /// gateway; tests may supply a deterministic sender.
    using SendQuoteFn = std::function<std::pair<uint32_t, uint32_t>(
        double bidP, uint32_t bidQ, double askP, uint32_t askQ)>;
    void setSendQuoteFn(SendQuoteFn fn) { m_sendQuote = std::move(fn); }

    /// Injectable cancel endpoints.  Production routes these through the same
    /// strategy gateway as new orders.
    using CancelOrderFn = std::function<bool(uint32_t localid)>;
    using CancelAllFn = std::function<void()>;
    void setCancelOrderFn(CancelOrderFn fn) { m_cancelOrder = std::move(fn); }
    void setCancelAllFn(CancelAllFn fn) { m_cancelAll = std::move(fn); }

    /// Called for every localid that enters OQM tracking.  The strategy uses
    /// this to mirror production OQM orders into the single
    /// IssuedOrderRegistry (anomaly protection + fill-deviation inputs).
    using OrderIssuedFn = std::function<void(bool isBuy, double price,
                                             uint32_t qty, uint32_t localid)>;
    void setOrderIssuedFn(OrderIssuedFn fn) { m_orderIssued = std::move(fn); }

    // --- Runtime param adjustment ---
    void setScaleFactor(double scale);
    void setMaxPosition(uint32_t maxPos) { m_cfg.max_position = maxPos; }

    /// B07: clear lifetime counters (cancel/new/fill/reject + hard-flat latch).
    /// MUST be called at each session begin — otherwise MaxCancel /
    /// MaxNewOrders / hard_flat_after_n_fills eventually lock all quoting.
    void resetCounters();

    /// C5: append one CSV line with this contract's lifetime counters.
    /// Called at session end for exchange-report reconciliation.
    void dumpCountersCsv(const std::string& path) const;

    // --- Active state ---
    bool isActive() const { return m_active; }
    void setActive(bool b) { m_active = b; }
    bool isGuardOK() const;

    // --- Reject retry ---
    bool isRetryPending() const { return m_retryPending; }
    double getRetryDelayRemaining() const;

    // --- Queries ---
    const MultiMarket& getCurrentMarket() const { return m_orderMarketTracker; }
    const MultiMarket& getLastDesired() const { return m_lastDesired; }
    int32_t getNumCancel() const { return m_numCancel; }
    int32_t getNumReject() const { return m_numReject; }
    int32_t getNumFill() const { return m_numFill; }
    int32_t getNumNewOrders() const { return m_numNewOrders; }
    int32_t getNumLateFills() const { return m_numLateFills; }
    bool isHardFlatMode() const { return m_hardFlatMode; }
    /// True when cancel retries exhausted without a terminal callback; new
    /// orders on this instrument are suppressed until the order goes terminal.
    bool isCancelStuck() const { return m_cancelStuck; }
    const std::string& getCode() const { return m_code; }
    int32_t getTrueMaxCancels() const;

    void setGetTimeFn(GetTimeFn fn) { m_getTime = std::move(fn); }

private:
    // --- Send via WT API ---
    std::pair<uint32_t, uint32_t> sendQuote(double bidP, uint32_t bidQ,
                                              double askP, uint32_t askQ);
    uint32_t sendSingle(bool isBuy, double price, uint32_t qty);  // A4
    bool sendCancelById(uint32_t localid);
    void scanCancelTimeouts(double now);
    void sendCancelAll();
    void applyOffsetGuard(uint32_t& qty, bool isBuy);             // A1 guard

    // --- Cancel helpers ---
    int32_t cancelSide(bool isBuy);   // returns cancels actually sent this call (B22a)
    void cancelByPrice(bool isBuy, double price);
    void rebuildOrderMarketTracker();

    PreTradeCheckFn m_preTradeCheck;   // A2
    SendSingleFn m_sendSingle;         // A4 test seam
    SendQuoteFn m_sendQuote;           // gateway seam
    CancelOrderFn m_cancelOrder;       // gateway seam
    CancelAllFn m_cancelAll;           // gateway seam
    OrderIssuedFn m_orderIssued;       // production registration hook

    // --- Incremental diffing ---
    int32_t getMissingPriceLevelSize(double price, uint32_t desiredSize, bool isBuy) const;
    int32_t updateSide(bool isBuy, const PriceSize& desired);   // txns issued this call
    void trackOrder(bool isBuy, double px, uint32_t sz, uint32_t localid);  // B21

    // --- STP ---
    bool is_crossed(double bidP, double askP) const;

    // --- Cancel throttle ---
    bool canCancel() const;
    bool isCancelSoftWarn() const;

    // --- Per-contract order state ---
    struct OrderState {
        enum class Phase : uint8_t { New, Live, CancelPending, Dead };

        uint32_t localid = 0;
        bool isBuy = false;
        double price = 0;
        uint32_t qty = 0;
        uint32_t filled = 0;
        bool active = false;
        bool cancelPending = false;
        double cancelSentTime = 0;   // last cancel send (seconds)
        uint32_t cancelRetry = 0;    // cancel re-sends
        double issueTime = 0;
        bool acknowledged = false;  // B24: set on first exchange ack
        Phase phase = Phase::New;   // C4: explicit lifecycle phase

        static const char* phaseName(Phase p) {
            switch (p) {
            case Phase::New: return "New";
            case Phase::Live: return "Live";
            case Phase::CancelPending: return "CancelPending";
            case Phase::Dead: return "Dead";
            }
            return "?";
        }
    };

    std::string m_code;
    Config m_cfg;
    GetTimeFn m_getTime;

    bool m_active = false;
    int32_t m_position = 0;
    int32_t m_numCancel = 0;
    int32_t m_numReject = 0;
    int32_t m_numFill = 0;
    int32_t m_numNewOrders = 0;
    int32_t m_numLateFills = 0;
    double m_lastUpdateCycleTime = 0;
    double m_lastUpdateTime = 0;
    static constexpr double LATE_FILL_THRESHOLD = 1.0;
    bool m_hardFlatMode = false;
    bool m_cancelStuck = false;
    double m_lastCancelStuckLog = 0;

    // Runtime scale factor (separate from config for runtime adjustment)
    double m_runtimeScale = 1.0;

    std::vector<OrderState> m_bidOrders;
    std::vector<OrderState> m_askOrders;

    MultiMarket m_orderMarketTracker;
    MultiMarket m_lastDesired;

    // Enhancement modules
    RiskFilterChainPtr m_filterChain;
    PositionOffsetMgrPtr m_positionOffset;
    PositionGuardPtr m_positionGuard;

    // Quote statistics (actual order execution stats)
    QuoteStatistics* m_quoteStats = nullptr;  // non-owning, owned by HftOptionStrategy
    uint64_t m_tickTimestampUs = 0;           // last tick arrival time (for latency)
    uint32_t m_timeHHMM = 0;                  // current time HHMM (for session-based stats)
    uint32_t m_secInMin = 0;                   // seconds within minute [0,59]

    /// Push posted market snapshot to QuoteStatistics
    void pushQuoteStats();

    // Reject retry
    uint32_t m_rejectRetryCount = 0;
    uint32_t m_rejectMaxRetries = 3;
    double m_rejectRetryDelayMs = 400;
    double m_lastRejectTime = 0;
    bool m_retryPending = false;
};

using OptionQuoteManagerPtr = std::shared_ptr<OptionQuoteManager>;

} // namespace wt_option
