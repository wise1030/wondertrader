#pragma once

#include "optioncoretypes.h"
#include "OptionOrder.h"
#include <functional>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace wt_option {

class OptionData;

// Order execution callback - bridges to IHftStraCtx stra_buy/sell
struct ComboExecContext {
    // Send a single-leg order, returns localid (0 = failed)
    std::function<uint32_t(const std::string& code, bool isBuy, double price, uint32_t qty)> sendOrder;
    // Cancel an order by localid
    std::function<bool(uint32_t localid)> cancelOrder;
    // Get current time in seconds (for timeout)
    std::function<double()> getTime;
};

// ============================================================================
// SpreadComboOrder - 2-leg spread (buy leg1, sell leg2 or vice versa)
// Legs execute sequentially: leg1 first, then leg2 on leg1 fill.
// ============================================================================
class SpreadComboOrder : public ComboOrder {
public:
    SpreadComboOrder(const std::string& name, uint32_t totalSize,
                      double tickSize, ComboExecContext* ctx);

    // Setup legs: leg1 is sent first (most mispriced), leg2 is the hedge
    void setupLegs(OptionData* leg1, bool leg1IsBuy,
                   OptionData* leg2, bool leg2IsBuy,
                   double leg1Price, double leg2Price);

    SendResult sendOrders() override;
    void onFill(const OptionOrder& order, const FillEvent& fill) override;
    bool checkDone(bool timeout) override;

    // Check timeout - returns true if timed out and needs cancel
    bool checkTimeout();

    // Cancel all unfilled legs
    void cancelAll();

    bool isActive() const { return m_active; }
    bool isDone() const { return m_done; }

private:
    void sendLeg2();

    ComboExecContext* m_execCtx;
    double m_tickSize;
    bool m_leg1Sent = false;
    bool m_leg2Sent = false;
    bool m_active = false;
    bool m_done = false;
    double m_sendTime = 0;
    double m_timeoutSec = 0.130;  // 130ms timeout (matching original)

    // Leg info for execution
    std::string m_leg1Code, m_leg2Code;
    bool m_leg1IsBuy = false, m_leg2IsBuy = false;
    double m_leg1Price = 0, m_leg2Price = 0;
    uint32_t m_leg1LocalId = 0, m_leg2LocalId = 0;
    uint32_t m_totalSize = 0;
};

// ============================================================================
// SynComboOrder - 3-leg synthetic future (buy call + sell put + sell future)
// Legs execute sequentially: leg1 (option) first, then leg2 (option), then leg3 (future hedge)
// ============================================================================
class SynComboOrder : public ComboOrder {
public:
    SynComboOrder(const std::string& name, uint32_t totalSize,
                  double tickSize, int32_t optionVsFutureRatio,
                  ComboExecContext* ctx);

    // Setup: buy call, sell put, sell future (or reverse)
    void setupLegs(OptionData* call, OptionData* put, const std::string& futureCode,
                   bool buyCall, bool buyPut, bool buyFuture,
                   double callPrice, double putPrice, double futurePrice);

    SendResult sendOrders() override;
    void onFill(const OptionOrder& order, const FillEvent& fill) override;
    bool checkDone(bool timeout) override;

    bool checkTimeout();
    void cancelAll();

    bool isActive() const { return m_active; }
    bool isDone() const { return m_done; }

private:
    void sendNextLeg();

    ComboExecContext* m_execCtx;
    double m_tickSize;
    int32_t m_optFutRatio;  // option vs future ratio (e.g. 2:1 for SHFE options)
    bool m_active = false;
    bool m_done = false;
    double m_sendTime = 0;
    double m_timeoutSec = 0.130;

    int m_nextLegToSend = 0;  // 0=leg1, 1=leg2, 2=leg3(future)

    struct LegExec {
        std::string code;
        bool isBuy = false;
        double price = 0;
        uint32_t localId = 0;
        bool sent = false;
        bool filled = false;
        uint32_t filledQty = 0;
        uint32_t desiredQty = 0;
    };
    std::vector<LegExec> m_legExecs;
};

using SpreadComboOrderPtr = std::shared_ptr<SpreadComboOrder>;
using SynComboOrderPtr = std::shared_ptr<SynComboOrder>;

} // namespace wt_option
