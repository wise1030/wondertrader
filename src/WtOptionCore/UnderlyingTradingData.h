/*!
 * \file UnderlyingTradingData.h
 * \brief Trading data for underlying instrument (Spot/Future)
 * 
 * Migrated from longbeach/quantbox/strategy/optioncore/UnderlyingTradingData.h
 */

#pragma once

#include "OptionTypes.h"
#include "BaseOrder.h"
#include <string>
#include <memory>
#include <vector>
#include <atomic>

namespace wt_option {

struct UnderlyingMarket {
    double bid = 0;
    double ask = 0;
    double last = 0;
    int32_t bidSize = 0;
    int32_t askSize = 0;
    uint64_t updateTime = 0;
    
    double mid() const { return (bid > 0 && ask > 0) ? (bid + ask) * 0.5 : last; }
};

struct UnderlyingValues {
    double theoreticalPrice = 0;
    double delta = 1.0; // Always 1 for underlying
    
    // Market Making / Execution
    double ourBid = 0;
    double ourAsk = 0;
    int32_t ourBidSize = 0;
    int32_t ourAskSize = 0;
};

class IOrderManager; // Forward decl

class UnderlyingTradingData {
public:
    UnderlyingTradingData(const std::string& code);
    
    const std::string& getCode() const { return m_code; }
    
    // Market Data
    void updateMarket(const UnderlyingMarket& market);
    const UnderlyingMarket& getMarket() const { return m_market; }
    double getMid() const { return m_market.mid(); }
    
    // Values (Double-Buffered)
    // Read from ACTIVE buffer
    const UnderlyingValues& values() const { return m_values[m_activeIndex.load(std::memory_order_acquire)]; } 
    
    // Writer interface for Lock-Free update
    UnderlyingValues& beginUpdateValues() { return m_values[1 - m_activeIndex.load(std::memory_order_relaxed)]; }
    void commitUpdateValues() { 
        m_activeIndex.store(1 - m_activeIndex.load(std::memory_order_relaxed), std::memory_order_release); 
    }
    
    // Position
    void setPosition(int32_t pos) { m_position = pos; }
    int32_t getPosition() const { return m_position; }
    void addFill(int32_t qty, double price);
    
    // Orders
    // We now use BaseOrder for underlying to avoid the overhead of OptionOrder (which carries Greeks and IV)
    
    // Quote Mode
    enum class QuoteMode {
        Off,
        Fixed,
        Join,
        Penny,
        Shark
    };
    void setQuoteMode(QuoteMode mode) { m_quoteMode = mode; }
    QuoteMode getQuoteMode() const { return m_quoteMode; }
    
private:
    std::string m_code;
    UnderlyingMarket m_market;
    std::array<UnderlyingValues, 2> m_values;
    std::atomic<uint32_t> m_activeIndex{0};
    int32_t m_position;
    QuoteMode m_quoteMode;
};

using UnderlyingTradingDataPtr = std::shared_ptr<UnderlyingTradingData>;

} // namespace wt_option
