/*!
 * \file OptionOrder.h
 * \brief Option order tracking with values at issue
 * 
 * Migrated from longbeach/quantbox/strategy/optioncore/OptionOrderInfo.h
 */

#pragma once

#include "OptionTypes.h"
#include "OptionGreeks.h"
#include <memory>
#include <string>
#include <functional>
#include <vector>
#include <boost/pool/object_pool.hpp>
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
    // Memory pooling optimizations
    static void* operator new(size_t size);
    static void operator delete(void* ptr);

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
};

using ComboOrderPtr = std::shared_ptr<ComboOrder>;

} // namespace wt_option
