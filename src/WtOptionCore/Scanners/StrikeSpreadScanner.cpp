/*!
 * \file StrikeSpreadScanner.cpp
 * \brief Strike spread scanner implementation
 */

#include "StrikeSpreadScanner.h"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace wt_option {

//=============================================================================
// SpreadComboOrder implementation
//=============================================================================

SpreadComboOrder::SpreadComboOrder(const SpreadOrderInfo& info,
                                   const StrikeSpreadScannerConfig& config)
    : ComboOrder("SpreadOrder")
    , m_info(info)
    , m_config(config)
    , m_fill1(0)
    , m_fill2(0)
    , m_expectedFill2(0)
{
}

ComboOrder::SendResult SpreadComboOrder::sendOrders() {
    // Create orders for both legs
    // Leg1: Buy one, Leg2: Sell other
    return SendResult::Success;
}

void SpreadComboOrder::onFill(const OptionOrder& order, const FillEvent& fill) {
    ComboOrder::onFill(order, fill);
    
    // Track fills and update expected fills
    if (order.getCode() == m_info.option->getCode()) {
        m_fill1 += fill.fillQty;
        m_expectedFill2 = m_fill1;  // Should match leg 1
    } else {
        m_fill2 += fill.fillQty;
    }
}

bool SpreadComboOrder::checkDone(bool timeout) {
    if (m_fill1 > 0 && m_fill2 >= m_expectedFill2) {
        return true;
    }
    return ComboOrder::checkDone(timeout);
}

//=============================================================================
// GutsComboOrder implementation
//=============================================================================

GutsComboOrder::GutsComboOrder(const GutsOrderInfo& info,
                               const StrikeSpreadScannerConfig& config)
    : ComboOrder("GutsOrder")
    , m_info(info)
    , m_config(config)
    , m_fill1(0)
    , m_fill2(0)
{
}

ComboOrder::SendResult GutsComboOrder::sendOrders() {
    return SendResult::Success;
}

void GutsComboOrder::onFill(const OptionOrder& order, const FillEvent& fill) {
    ComboOrder::onFill(order, fill);
}

bool GutsComboOrder::checkDone(bool timeout) {
    return ComboOrder::checkDone(timeout);
}

//=============================================================================
// SynpairComboOrder implementation
//=============================================================================

SynpairComboOrder::SynpairComboOrder(const SynpairOrderInfo& info,
                                     const StrikeSpreadScannerConfig& config)
    : ComboOrder("SynpairOrder")
    , m_info(info)
    , m_config(config)
    , m_fills(4, 0)
    , m_expectedFills(4, 0)
    , m_timeout(false)
{
}

ComboOrder::SendResult SynpairComboOrder::sendOrders() {
    return SendResult::Success;
}

void SynpairComboOrder::onFill(const OptionOrder& order, const FillEvent& fill) {
    ComboOrder::onFill(order, fill);
}

bool SynpairComboOrder::checkDone(bool timeout) {
    m_timeout = timeout;
    return ComboOrder::checkDone(timeout);
}

bool SynpairComboOrder::sortByViolation() {
    // Sort legs by how far they are from target
    return true;
}

//=============================================================================
// StrikeSpreadScanner implementation
//=============================================================================

StrikeSpreadScanner::StrikeSpreadScanner(const StrikeSpreadScannerConfig& config)
    : IScanModule("StrikeSpreadScanner")
    , m_config(config)
    , m_spreadOpenSize(0)
    , m_gutsOpenSize(0)
    , m_synpairOpenSize(0)
    , m_lastCheckTime(0)
    , m_tickF(0.01)
{
}

void StrikeSpreadScanner::onStart() {
    setEnabled(true);
    m_spreadOpenSize = 0;
    m_gutsOpenSize = 0;
    m_synpairOpenSize = 0;
}

void StrikeSpreadScanner::onStop() {
    setEnabled(false);
    
    // Cancel all pending orders
    m_spreadOrders.clear();
    m_gutsOrders.clear();
    m_synpairOrders.clear();
}

void StrikeSpreadScanner::onTick(const OptionGrid* grid) {
    if (!isEnabled() || !grid) return;
    
    scanGrid();
}

void StrikeSpreadScanner::onOptionUpdate(OptionData* option) {
    if (!isEnabled() || !option) return;
    
    evalOption(option);
}

void StrikeSpreadScanner::onUnderlyingUpdate(double price) {
    // Update reference prices
}

void StrikeSpreadScanner::onOptionAdded(const OptionData* option) {
    if (!option) return;
    
    uint32_t expiry = option->getInfo().expiry;
    m_expiries.insert(expiry);
}

void StrikeSpreadScanner::onGridUpdated() {
    // Grid update callback
}

void StrikeSpreadScanner::scanGrid() {
    // Process scan list
    for (auto& expiryPair : m_scanList) {
        for (auto& option : expiryPair.second) {
            evalOption(option.get());
        }
    }
    
    // Clean up done orders
    m_spreadOrders.erase(
        std::remove_if(m_spreadOrders.begin(), m_spreadOrders.end(),
            [](const auto& order) { return order->checkDone(false); }),
        m_spreadOrders.end()
    );
    
    m_gutsOrders.erase(
        std::remove_if(m_gutsOrders.begin(), m_gutsOrders.end(),
            [](const auto& order) { return order->checkDone(false); }),
        m_gutsOrders.end()
    );
    
    m_synpairOrders.erase(
        std::remove_if(m_synpairOrders.begin(), m_synpairOrders.end(),
            [](const auto& order) { return order->checkDone(false); }),
        m_synpairOrders.end()
    );
}

void StrikeSpreadScanner::evalOption(OptionData* option) {
    if (!option) return;
    
    auto strike = option->getStrikeData();
    if (!strike) return;
    
    // Check open size limits
    int32_t totalOpenSize = m_spreadOpenSize + m_gutsOpenSize + m_synpairOpenSize;
    if (totalOpenSize >= m_config.maxOpenSize) {
        return;
    }
    
    // Evaluate opportunities
    std::vector<SpreadOrderInfo> spreadOrders;
    std::vector<GutsOrderInfo> gutsOrders;
    std::vector<SynpairOrderInfo> synpairOrders;
    
    if (m_config.enableSpread) {
        evalSpread(option, strike.get(), spreadOrders);
    }
    
    if (m_config.enableGuts) {
        evalGuts(option, strike.get(), gutsOrders);
    }
    
    if (m_config.enableSynpair) {
        evalSynpair(option, strike.get(), synpairOrders);
    }
    
    // Sort by profit and execute best opportunities
    std::sort(spreadOrders.begin(), spreadOrders.end());
    std::sort(gutsOrders.begin(), gutsOrders.end());
    std::sort(synpairOrders.begin(), synpairOrders.end());
    
    // Execute spread orders
    for (const auto& order : spreadOrders) {
        if (m_spreadOpenSize >= m_config.maxSpreadSize) break;
        
        auto combo = std::make_shared<SpreadComboOrder>(order, m_config);
        if (combo->sendOrders() == ComboOrder::SendResult::Success) {
            m_spreadOrders.push_back(combo);
            m_spreadOpenSize += order.size;
            
            // Fire scanner hit
            ScannerHitEvent event;
            event.option = option;
            event.signal = order.profit;
            event.reason = "Spread opportunity from " + getName();
            notifyHit(event);
        }
    }
    
    // Execute guts orders
    for (const auto& order : gutsOrders) {
        if (m_gutsOpenSize >= m_config.maxGutsSize) break;
        
        if (order.signalOnly || m_config.gutsSignalOnly) {
            // Signal only
            ScannerHitEvent event;
            event.option = option;
            event.signal = order.profit;
            event.reason = "Guts signal from " + getName();
            notifyHit(event);
        } else {
            auto combo = std::make_shared<GutsComboOrder>(order, m_config);
            if (combo->sendOrders() == ComboOrder::SendResult::Success) {
                m_gutsOrders.push_back(combo);
                m_gutsOpenSize += order.size;
            }
        }
    }
    
    // Execute synpair orders
    for (const auto& order : synpairOrders) {
        if (m_synpairOpenSize >= m_config.maxSynpairSize) break;
        
        auto combo = std::make_shared<SynpairComboOrder>(order, m_config);
        if (combo->sendOrders() == ComboOrder::SendResult::Success) {
            m_synpairOrders.push_back(combo);
            m_synpairOpenSize += order.totalSize;
        }
    }
}

bool StrikeSpreadScanner::evalSpread(OptionData* option, StrikeData* strike,
                                      std::vector<SpreadOrderInfo>& orders) {
    if (!option || !strike) return false;
    
    auto expiry = strike->getExpiryData();
    if (!expiry) return false;
    
    // Get reference price
    double refPx = getReferenceFutPx(option->getInfo().expiry);
    if (refPx <= 0) return false;
    
    // Look for spreads with adjacent strikes
    double strikePrice = strike->getStrike();
    bool isCall = (option->getInfo().right == OptionRight::Call);
    
    // Find adjacent strikes
    const auto& strikes = expiry->getStrikes();
    for (const auto& otherStrikePair : strikes) {
        StrikeData* otherStrike = otherStrikePair.second.get();
        if (otherStrike == strike) continue;
        
        double otherStrikePrice = otherStrike->getStrike();
        double strikeDiff = std::abs(otherStrikePrice - strikePrice);
        
        // Get counterpart option (same type at different strike)
        OptionDataPtr counterpartPtr = isCall ? otherStrike->call() : otherStrike->put();
        if (!counterpartPtr) continue;
        OptionData* counterpart = counterpartPtr.get();
        
        // Calculate spread profit
        double optBid = option->getBid();
        double optAsk = option->getAsk();
        double cpBid = counterpart->getBid();
        double cpAsk = counterpart->getAsk();
        
        if (optBid <= 0 || cpAsk <= 0) continue;
        
        // Bull spread: buy lower strike, sell higher strike
        // Bear spread: buy higher strike, sell lower strike
        double profit = 0;
        if (strikePrice < otherStrikePrice) {
            // Bull spread
            profit = cpBid - optAsk - strikeDiff * 0.5;  // Simplified calc
        } else {
            profit = optBid - cpAsk - strikeDiff * 0.5;
        }
        
        if (profit > m_config.spreadThreshold * refPx) {
            SpreadOrderInfo info;
            // Store OptionDataPtr from strike data
            auto optStrike = option->getStrikeData();
            info.option = isCall ? optStrike->call() : optStrike->put();
            info.counterpart = counterpartPtr;
            info.size = 1;
            info.profit = profit;
            info.strikeDiff = strikeDiff;
            info.bidPrice = optBid;
            info.askPrice = cpAsk;
            
            orders.push_back(info);
        }
    }
    
    return !orders.empty();
}

bool StrikeSpreadScanner::evalGuts(OptionData* option, StrikeData* strike,
                                    std::vector<GutsOrderInfo>& orders) {
    // Guts evaluation - strangle variations
    if (!option || !strike) return false;
    
    auto expiry = strike->getExpiryData();
    if (!expiry) return false;
    
    // For guts: buy OTM call + buy OTM put
    // This is simplified - full impl would check multiple strike combinations
    
    return !orders.empty();
}

bool StrikeSpreadScanner::evalSynpair(OptionData* option, StrikeData* strike,
                                       std::vector<SynpairOrderInfo>& orders) {
    // Synthetic pair evaluation
    // Looks for arbitrage in synthetic positions
    
    return !orders.empty();
}

double StrikeSpreadScanner::getReferenceFutPx(uint32_t expiry) {
    auto it = m_referenceFutPx.find(expiry);
    if (it != m_referenceFutPx.end()) {
        return it->second;
    }
    return 0;
}

bool StrikeSpreadScanner::isWithinDelta(OptionData* option) {
    if (!option) return false;
    
    double delta = std::abs(option->greeks().delta());
    return delta >= 0.1 && delta <= 0.9;
}

} // namespace wt_option
