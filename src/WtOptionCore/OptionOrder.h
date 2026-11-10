/*!
 * \file OptionOrder.h
 * \brief Option order tracking with values at issue
 * 
 * Migrated from longbeach/quantbox/strategy/optioncore/OptionOrderInfo.h
 */

#pragma once

#include "optioncoretypes.h"
#include "OptionGreeks.h"
#include <memory>
#include <string>
#include <functional>
#include <map>
#include <vector>
#include "BaseOrder.h"

namespace wt_option {

// Forward declarations
class OptionData;
using OptionDataPtr = std::shared_ptr<OptionData>;

/**
 * @brief Values snapshot at order issue time
 */
struct ValuesAtIssue {
    double theoPrice = 0;       // Theoretical price
    double impliedVol = 0;      // Implied volatility
    double atmVol = 0;          // ATM volatility
    double midPrice = 0;        // Mid price
    double underlyingPrice = 0; // Underlying price
    OptionGreeks greeks;        // Greeks at issue
    uint64_t issueTime = 0;     // Issue timestamp
};



/**
 * @brief Option order with tracking information
 * 
 * Tracks order lifecycle from creation to fill/cancel,
 * including values snapshot at issue time for P&L analysis.
 */
class OptionOrder : public BaseOrder {
public:
    // NOTE: OptionOrder is always allocated via std::make_shared (see
    // OrderManager / WtOptContext), which uses the global allocator. A prior
    // custom boost::object_pool operator new/delete was removed: it was unsafe
    // (operator new fell back to ::operator new for size mismatches while
    // operator delete unconditionally returned memory to the pool, risking
    // heap corruption for any derived type) and never actually exercised
    // because make_shared bypasses class-level operator new.

    OptionOrder(uint32_t orderId, OptionDataPtr option, 
                OrderDir dir, double price, uint32_t qty);
    OptionOrder(uint32_t orderId, const std::string& code, 
                OrderDir dir, double price, uint32_t qty);
    
    // Option identification
    OptionDataPtr getOption() const { return m_option.lock(); }
    
    // Values at issue
    const ValuesAtIssue& getValuesAtIssue() const { return m_valuesAtIssue; }
    void captureValuesAtIssue();
    
    // P&L calculation
    double getRealizedPnL() const;
    double getUnrealizedPnL(double currentMid) const;
    
private:
    std::weak_ptr<OptionData> m_option;
    ValuesAtIssue m_valuesAtIssue;
};

using OptionOrderPtr = std::shared_ptr<OptionOrder>;

/**
 * @brief Order event listener
 */
class IOrderListener {
public:
    virtual ~IOrderListener() = default;
    
    virtual void onOrderSent(const OptionOrder& order) {}
    virtual void onOrderFill(const OptionOrder& order, const FillEvent& fill) {}
    virtual void onOrderCancelled(const OptionOrder& order) {}
    virtual void onOrderRejected(const OptionOrder& order, const std::string& reason) {}
};

/**
 * @brief Multi-leg order (combo order)
 */
class ComboOrder {
public:
    enum class SendResult {
        Success,
        PartialSend,
        Failed
    };
    
    ComboOrder(const std::string& name);
    virtual ~ComboOrder() = default;
    
    void addLeg(OptionOrderPtr order, int32_t ratio = 1);
    
    virtual SendResult sendOrders() = 0;
    virtual void onFill(const OptionOrder& order, const FillEvent& fill);
    virtual bool checkDone(bool timeout);
    
    const std::string& getName() const { return m_name; }
    size_t getLegCount() const { return m_legs.size(); }

    /// P2: who initiated the cancel — set by the owner before cancelAll() on
    /// the timeout path so the FAILED log can distinguish "our own timeout
    /// (leg never traded)" from "external cancel (risk/session stop, entrust
    /// reject, or the replay's random-cancel simulation)".
    const char* cancelReason = nullptr;

    // ------------------------------------------------------------------
    // P1-d/W5: per-leg order records (BaseOrder/OptionOrder state machine).
    // The concrete combos keep their fast localid-based routing untouched;
    // these records mirror every send/fill/terminal transition so the
    // previously-uninstantiated order state machine carries a real audit
    // trail (avg fill price, per-leg state history) for post-trade analysis.
    // ------------------------------------------------------------------
    void trackLegOrder(uint32_t localid, const std::string& code,
                       bool isBuy, double price, uint32_t qty);
    void legOrderFilled(uint32_t localid, double price, uint32_t qty, uint64_t timeMs);
    void legOrderTerminal(uint32_t localid, bool rejected);
    OptionOrderPtr getLegOrder(uint32_t localid) const;
    const std::map<uint32_t, OptionOrderPtr>& getLegOrders() const { return m_legOrderRecords; }

protected:
    struct Leg {
        OptionOrderPtr order;
        int32_t ratio;
        int32_t expectedFill;
    };
    
    std::string m_name;
    std::vector<Leg> m_legs;
    int32_t m_totalSize;
    double m_unitProfit;

    // P1-d/W5: localid -> order record (audit mirror of the fast routing)
    std::map<uint32_t, OptionOrderPtr> m_legOrderRecords;
};

using ComboOrderPtr = std::shared_ptr<ComboOrder>;

} // namespace wt_option
