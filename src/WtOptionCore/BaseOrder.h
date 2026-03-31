/*!
 * \file BaseOrder.h
 * \brief Standard underlying directional base order structure
 */

#pragma once

#include "OptionTypes.h"
#include <memory>
#include <string>
#include <vector>

namespace wt_option {

/**
 * @brief Order direction
 */
enum class OrderDir {
    Buy = 1,
    Sell = -1
};

/**
 * @brief Order state
 */
enum class OrderState {
    Pending,        // Order created but not sent
    Sent,           // Order sent to market
    PartialFill,    // Partially filled
    Filled,         // Completely filled
    Cancelled,      // Cancelled
    Rejected,       // Rejected by exchange
    Error           // Error state
};

/**
 * @brief Fill event data
 */
struct FillEvent {
    uint32_t orderId;
    double fillPrice;
    uint32_t fillQty;
    uint32_t cumQty;
    uint64_t fillTime;
    bool isLateFill;            // Fill after expected time
};

/**
 * @brief Lightweight base order with tracking information
 */
class BaseOrder {
public:
    BaseOrder(uint32_t orderId, const std::string& code, 
              OrderDir dir, double price, uint32_t qty);
    virtual ~BaseOrder() = default;
    
    // Order identification
    uint32_t getOrderId() const { return m_orderId; }
    const std::string& getCode() const { return m_code; }
    
    // Order parameters
    OrderDir getDirection() const { return m_direction; }
    double getPrice() const { return m_price; }
    uint32_t getQuantity() const { return m_quantity; }
    uint32_t getFilledQty() const { return m_filledQty; }
    uint32_t getRemainingQty() const { return m_quantity - m_filledQty; }
    
    // Order state
    OrderState getState() const { return m_state; }
    virtual void setState(OrderState state);
    bool isActive() const;
    bool isFilled() const { return m_state == OrderState::Filled; }
    bool isCancelled() const { return m_state == OrderState::Cancelled; }
    
    // Fill handling
    void addFill(double fillPrice, uint32_t fillQty, uint64_t fillTime);
    double getAvgFillPrice() const;
    const std::vector<FillEvent>& getFills() const { return m_fills; }
    
    // Late fill detection
    bool isLateFill() const { return m_isLateFill; }
    void setLateFill(bool late) { m_isLateFill = late; }
    
    // Order type classification
    int32_t getOrderType() const { return m_orderType; }
    void setOrderType(int32_t type) { m_orderType = type; }
    
    // Timestamps
    uint64_t getCreateTime() const { return m_createTime; }
    void setSendTime(uint64_t time) { m_sendTime = time; }
    uint64_t getSendTime() const { return m_sendTime; }
    
    // Exchange / Broker Order ID mapping
    const std::string& getEntrustNo() const { return m_entrustNo; }
    void setEntrustNo(const std::string& entrustNo) { m_entrustNo = entrustNo; }
    
protected:
    uint32_t m_orderId;
    std::string m_code;
    
    OrderDir m_direction;
    double m_price;
    uint32_t m_quantity;
    uint32_t m_filledQty;
    double m_totalFillValue;
    
    OrderState m_state;
    std::vector<FillEvent> m_fills;
    
    bool m_isLateFill;
    int32_t m_orderType;  // 0 = market making, 1 = scanner, etc.
    
    std::string m_entrustNo; // For tracking actual exchange IDs across restarts
    
    uint64_t m_createTime;
    uint64_t m_sendTime;
};

using BaseOrderPtr = std::shared_ptr<BaseOrder>;

/**
 * @brief Base Order event listener
 */
class IBaseOrderListener {
public:
    virtual ~IBaseOrderListener() = default;
    
    virtual void onOrderSent(const BaseOrder& order) {}
    virtual void onOrderFill(const BaseOrder& order, const FillEvent& fill) {}
    virtual void onOrderCancelled(const BaseOrder& order) {}
    virtual void onOrderRejected(const BaseOrder& order, const std::string& reason) {}
};

} // namespace wt_option
