/*!
 * \file OptionQuoteManager.h
 * \brief Per-contract order lifecycle manager
 *
 * Migrated from quantbox QuoteOrderManager(760L) + DefaultOrderManager(1607L).
 * Complete order lifecycle: send→cancel→orderStatus→fill tracking.
 * Uses WT stra_quote/stra_cancel/stra_cancel_all/stra_buy/sell as底层API.
 *
 * Per-contract state:
 *   m_bidOrders/m_askOrders — active order list with localid/price/qty/filled
 *   m_orderMarketTracker — actual posted market (rebuilt from order callbacks)
 *   m_position — contract position
 *
 * Key methods:
 *   updateQuoteOrders — desired→cancel old→send new (with is_crossed STP)
 *   cancelAll/cancelById — precise cancel
 *   onOrderStatusChange/onFill — callback from strategy on_order/on_trade
 */
#pragma once

#include "OptionValues.h"  // MultiMarket, PriceSize

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

// Forward declaration — IUftStraCtx defined in WT Includes
namespace wtp { class IUftStraCtx; }

namespace wt_option {

class OptionQuoteManager {
public:
    struct Config {
        // --- 基础报单参数 ---
        uint32_t max_side_orders = 1;      // max orders per side (multi-level)
        uint32_t max_position = 50;        // max position (absolute)
        double time_in_force_ms = 45000;   // order TTL (0=never expire)
        bool enable_quote_api = true;      // use stra_quote (single level), else stra_buy/sell
        bool avoid_trade = false;          // skip update if desired==current
        std::string exchange = "SHFE";
        double tick_size = 0.5;

        // --- DefaultOM 功能补齐 ---
        bool check_potential_position = false;  // pre-trade position check
        bool leave_outer_orders = true;         // keep outer orders when updating
        bool wait_for_cancels = false;          // wait for cancel confirm before resending
        double order_delay_ms = 0;              // delay orders (testing)

        // --- 撤单限制 ---
        int32_t max_cancels_allowed = 0;        // hard cancel limit (0=unlimited)
        int32_t cancel_soft_max = 0;            // soft cancel limit (warn)
        int32_t hard_flat_after_n_fills = 0;    // force flat after N fills (0=disabled)
        int32_t reject_max_new_orders = -1;     // reject after N new orders (-1=disabled)
    };

    using GetTimeFn = std::function<double()>;

    OptionQuoteManager(const std::string& code, const Config& cfg, wtp::IUftStraCtx* ctx);
    ~OptionQuoteManager() = default;

    // --- Core: update desired market → send orders ---
    int32_t updateOrders(const MultiMarket& desired, bool cancel_only = false);

    // --- Callbacks (from strategy on_order/on_trade) ---
    void onOrderStatusChange(uint32_t localid, bool isLong,
                              double totalQty, double leftQty,
                              double price, bool isCanceled);
    void onFill(uint32_t localid, bool isLong, double fill_px, uint32_t fill_qty);

    // --- Position ---
    void setPosition(int32_t pos) { m_position = pos; }
    int32_t getPosition() const { return m_position; }

    // --- Active state ---
    bool isActive() const { return m_active; }
    void setActive(bool b) { m_active = b; }

    //--- Queries ---
    const MultiMarket& getCurrentMarket() const { return m_orderMarketTracker; }
    const MultiMarket& getLastDesired() const { return m_lastDesired; }
    int32_t getNumCancel() const { return m_numCancel; }
    int32_t getNumReject() const { return m_numReject; }
    int32_t getNumFill() const { return m_numFill; }
    int32_t getNumNewOrders() const { return m_numNewOrders; }
    bool isHardFlatMode() const { return m_hardFlatMode; }
    const std::string& getCode() const { return m_code; }

    void setGetTimeFn(GetTimeFn fn) { m_getTime = std::move(fn); }

private:
    // Send via WT API
    std::pair<uint32_t, uint32_t> sendQuote(double bidP, uint32_t bidQ,
                                              double askP, uint32_t askQ);
    bool sendCancelById(uint32_t localid);
    void sendCancelAll();

    // Cancel helpers
    void cancelSide(bool isBuy);
    void rebuildOrderMarketTracker();

    // STP: prevent self-crossing
    bool is_crossed(double bidP, double askP) const;

    // Per-contract order state
    struct OrderState {
        uint32_t localid = 0;
        bool isBuy = false;
        double price = 0;
        uint32_t qty = 0;
        uint32_t filled = 0;
        bool active = false;
        bool cancelPending = false;
        double issueTime = 0;  // seconds
    };

    std::string m_code;
    Config m_cfg;
    wtp::IUftStraCtx* m_ctx;
    GetTimeFn m_getTime;

    bool m_active = false;
    int32_t m_position = 0;
    int32_t m_numCancel = 0;
    int32_t m_numReject = 0;
    int32_t m_numFill = 0;
    int32_t m_numNewOrders = 0;
    bool m_hardFlatMode = false;

    std::vector<OrderState> m_bidOrders;
    std::vector<OrderState> m_askOrders;

    MultiMarket m_orderMarketTracker;
    MultiMarket m_lastDesired;
};

using OptionQuoteManagerPtr = std::shared_ptr<OptionQuoteManager>;

} // namespace wt_option
