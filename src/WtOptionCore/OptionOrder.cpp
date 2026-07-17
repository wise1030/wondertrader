/*!
 * \file OptionOrder.cpp
 * \brief Option order implementation
 */

#include "OptionOrder.h"
#include "OptionData.h"
#include "StrikeData.h"
#include "ExpiryData.h"
#include <chrono>
#include <cmath>

namespace wt_option {

//=============================================================================
// OptionOrder implementation
//=============================================================================

OptionOrder::OptionOrder(uint32_t orderId, OptionDataPtr option,
                         OrderDir dir, double price, uint32_t qty)
    : BaseOrder(orderId, option ? option->getCode() : "", dir, price, qty)
{
    m_option = option;
}

OptionOrder::OptionOrder(uint32_t orderId, const std::string& code,
                         OrderDir dir, double price, uint32_t qty)
    : BaseOrder(orderId, code, dir, price, qty)
{
}



void OptionOrder::captureValuesAtIssue() {
    auto opt = m_option.lock();
    if (!opt) return;
    
    m_valuesAtIssue.theoPrice = opt->getTheoPrice();
    m_valuesAtIssue.impliedVol = opt->getImpliedVol();
    m_valuesAtIssue.midPrice = opt->getMid();
    m_valuesAtIssue.underlyingPrice = opt->getMarket().underlyingPrice;
    m_valuesAtIssue.greeks = opt->greeks();
    
    using namespace std::chrono;
    m_valuesAtIssue.issueTime = duration_cast<microseconds>(
        system_clock::now().time_since_epoch()
    ).count();
    
    // ATM vol is stored on the pricer, not ExpiryData; set to 0 for now
    // (this is a snapshot field for PnL analysis, not critical for execution)
    m_valuesAtIssue.atmVol = 0;
}



double OptionOrder::getRealizedPnL() const {
    if (m_filledQty == 0) return 0;
    
    double avgFill = getAvgFillPrice();
    double theoAtIssue = m_valuesAtIssue.theoPrice;
    
    // P&L = (Fill - Theo) * Qty * Direction
    double pnl = (avgFill - theoAtIssue) * m_filledQty;
    
    // For sells, profit is positive if fill > theo
    // For buys, profit is positive if theo > fill
    if (m_direction == OrderDir::Sell) {
        return pnl;
    } else {
        return -pnl;
    }
}

double OptionOrder::getUnrealizedPnL(double currentMid) const {
    uint32_t remaining = getRemainingQty();
    if (remaining == 0) return 0;
    
    double entryPrice = m_price;
    double pnl = (currentMid - entryPrice) * remaining;
    
    if (m_direction == OrderDir::Buy) {
        return pnl;
    } else {
        return -pnl;
    }
}

//=============================================================================
// ComboOrder implementation
//=============================================================================

ComboOrder::ComboOrder(const std::string& name)
    : m_name(name)
    , m_totalSize(0)
    , m_unitProfit(0)
{
}

void ComboOrder::addLeg(OptionOrderPtr order, int32_t ratio) {
    Leg leg;
    leg.order = order;
    leg.ratio = ratio;
    leg.expectedFill = 0;
    m_legs.push_back(leg);
}

void ComboOrder::onFill(const OptionOrder& order, const FillEvent& fill) {
    // Find the leg and update expected fills for other legs
    for (auto& leg : m_legs) {
        if (leg.order->getOrderId() == order.getOrderId()) {
            // This leg was filled
            int32_t multiplier = fill.fillQty / std::abs(leg.ratio);
            
            // Update expected fills for other legs
            for (auto& otherLeg : m_legs) {
                if (&otherLeg != &leg) {
                    otherLeg.expectedFill += multiplier * std::abs(otherLeg.ratio);
                }
            }
            break;
        }
    }
}

bool ComboOrder::checkDone(bool timeout) {
    // Check if all legs are filled
    for (const auto& leg : m_legs) {
        if (!leg.order->isFilled() && !leg.order->isCancelled()) {
            return false;
        }
    }
    return true;
}

} // namespace wt_option
