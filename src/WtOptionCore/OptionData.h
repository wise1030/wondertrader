/*!
 * \file OptionData.h
 * \brief Single option contract data: market + computed values
 *
 * Migrated from quantbox optioncore/OptionData.h (116 lines).
 * Preserves: OptionValues m_valuesTable[MAX_VALUES] multi-value table + double buffering.
 * Replaces: MarketDataContext/IBook → updateMarket(struct OptionMarket) passive update.
 */
#pragma once

#include "optioncoretypes.h"
#include "OptionGreeks.h"
#include "OptionValues.h"

#include <memory>
#include <string>
#include <atomic>
#include <array>
#include <cstdint>

namespace wt_option {

class StrikeData;
class ExpiryData;
class OptionTradingData;
using StrikeDataPtr = std::shared_ptr<StrikeData>;
using StrikeDataWeakPtr = std::weak_ptr<StrikeData>;
using ExpiryDataPtr = std::shared_ptr<ExpiryData>;
using ExpiryDataWeakPtr = std::weak_ptr<ExpiryData>;
using OptionTradingDataPtr = std::shared_ptr<OptionTradingData>;
using OptionTradingDataWeakPtr = std::weak_ptr<OptionTradingData>;

// Market data for an option (replaces longbeach IBook/PriceSize)
struct OptionMarket {
    double bid = 0;
    double ask = 0;
    double last = 0;
    double bidSize = 0;
    double askSize = 0;
    double underlyingPrice = 0;
    uint64_t updateTime = 0;
};

// Option contract info (replaces longbeach instrument_t)
struct OptionInfo {
    std::string code;       // stdCode, e.g. "SHFE.cu2502C50000"
    std::string product;    // product, e.g. "cu"
    uint32_t expiry = 0;    // YYYYMM
    strike_t strike = 0;
    OptionRight right = OR_Call;
    double multiplier = 1.0;
    double tickSize = 0;
};

class OptionData {
public:
    static const int32_t MAX_VALUES = 5;

    OptionData(const OptionInfo& info);

    // Identity
    const std::string& getCode() const { return m_info.code; }
    const OptionInfo& getInfo() const { return m_info; }
    uint32_t getExpiry() const { return m_info.expiry; }
    strike_t getStrike() const { return m_info.strike; }
    OptionRight getRight() const { return m_info.right; }
    double getMultiplier() const { return m_info.multiplier; }
    double getTickSize() const { return m_info.tickSize; }

    // Market data
    const OptionMarket& getMarket() const { return m_market; }
    void updateMarket(const OptionMarket& market) { m_market = market; }
    double getBid() const { return m_market.bid; }
    double getAsk() const { return m_market.ask; }
    double getMid() const { return m_market.bid > 0 && m_market.ask > 0 ? (m_market.bid + m_market.ask) * 0.5 : m_market.last; }
    double getLast() const { return m_market.last; }

    // Computed values (multi-value table with double buffering)
    // Primary values (index 0) use atomic swap for lock-free reads
    OptionValues& values(size_t idx = 0) { return m_valuesTable[idx]; }
    const OptionValues& values(size_t idx = 0) const { return m_valuesTable[idx]; }

    // Lock-free read of primary values (double buffered)
    const OptionValues& activeValues() const { return m_valuesTable[m_activeIndex.load(std::memory_order_acquire)]; }
    OptionValues& beginUpdateValues() { return m_valuesTable[1 - m_activeIndex.load(std::memory_order_relaxed)]; }
    void commitUpdateValues() { m_activeIndex.store(1 - m_activeIndex.load(std::memory_order_relaxed), std::memory_order_release); }

    // Greeks shortcuts (from primary values)
    const OptionGreeks& greeks() const { return activeValues().greeks(); }
    double getTheoPrice() const { return activeValues().theo(); }
    double getImpliedVol() const { return activeValues().impliedVol(); }

    // Parent references
    void setStrikeData(StrikeDataPtr s) { m_strikeData = s; }
    StrikeDataPtr getStrikeData() const { return m_strikeData.lock(); }
    void setExpiryData(ExpiryDataPtr e) { m_expiryData = e; }
    ExpiryDataPtr getExpiryData() const { return m_expiryData.lock(); }

    // Position (for risk aggregation)
    double getPosition() const { return m_position; }
    void setPosition(double pos) { m_position = pos; }

    // Active flag (controlled by CompositeOptionPricer)
    bool isActive() const { return m_active; }
    void setActive(bool b) { m_active = b; }

    // Trading data (OptionTradingData, created by OptionTradingGrid)
    void setTradingData(OptionTradingDataPtr otd) { m_tradingData = otd; }
    OptionTradingDataPtr getTradingData() const { return m_tradingData.lock(); }

    // Current market (last quote sent to exchange — for check_markets diff)
    const MultiMarket& currentMarket() const { return m_currentMarket; }
    void setCurrentMarket(const MultiMarket& mkt) { m_currentMarket = mkt; }

    // Fast array indexing
    uint32_t getInternalId() const { return m_internalId; }
    void setInternalId(uint32_t id) { m_internalId = id; }

private:
    OptionInfo m_info;
    OptionMarket m_market;
    OptionValues m_valuesTable[MAX_VALUES];
    std::atomic<uint32_t> m_activeIndex{0};
    StrikeDataWeakPtr m_strikeData;
    ExpiryDataWeakPtr m_expiryData;
    OptionTradingDataWeakPtr m_tradingData;
    double m_position = 0;
    bool m_active = false;
    MultiMarket m_currentMarket;  // last quote sent (for diff)
    uint32_t m_internalId = 0;
};

using OptionDataPtr = std::shared_ptr<OptionData>;

} // namespace wt_option
