/*!
 * \file BaseOrder.cpp
 * \brief Standard underlying directional base order implementation
 */

#include "BaseOrder.h"
#include "../Share/TimeUtils.hpp"

namespace wt_option {

BaseOrder::BaseOrder(uint32_t orderId, const std::string& code, 
                     OrderDir dir, double price, uint32_t qty)
    : m_orderId(orderId)
    , m_code(code)
    , m_direction(dir)
    , m_price(price)
    , m_quantity(qty)
    , m_filledQty(0)
    , m_totalFillValue(0)
    , m_state(OrderState::Pending)
    , m_isLateFill(false)
    , m_orderType(0)
    , m_sendTime(0)
{
    m_createTime = TimeUtils::getLocalTimeNow();
}

void BaseOrder::setState(OrderState state) {
    // Basic state transition validation could be added here
    m_state = state;
}

bool BaseOrder::isActive() const {
    return m_state == OrderState::Pending || 
           m_state == OrderState::Sent || 
           m_state == OrderState::PartialFill;
}

void BaseOrder::addFill(double fillPrice, uint32_t fillQty, uint64_t fillTime) {
    if (fillQty == 0) return;
    
    m_filledQty += fillQty;
    m_totalFillValue += (fillPrice * fillQty);
    
    FillEvent event;
    event.orderId = m_orderId;
    event.fillPrice = fillPrice;
    event.fillQty = fillQty;
    event.cumQty = m_filledQty;
    event.fillTime = fillTime;
    event.isLateFill = m_isLateFill;
    
    m_fills.push_back(event);
    
    if (m_filledQty >= m_quantity) {
        m_state = OrderState::Filled;
    } else {
        m_state = OrderState::PartialFill;
    }
}

double BaseOrder::getAvgFillPrice() const {
    if (m_filledQty == 0) return 0.0;
    return m_totalFillValue / m_filledQty;
}

} // namespace wt_option
