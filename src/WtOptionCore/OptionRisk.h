/*!
 * \file OptionRisk.h
 * \brief Portfolio risk management for options
 * 
 * Migrated from longbeach/quantbox/strategy/optioncore/OptionRisk.h
 */

#pragma once

#include "OptionTypes.h"
#include "OptionGreeks.h"
#include "OptionData.h"
#include "OptionGrid.h"
#include <map>
#include <vector>
#include <memory>
#include <mutex>

namespace wtp { class TraderAdapter; }

namespace wt_option {

/**
 * @brief Risk data for a single option position
 */
struct OptionRiskData {
    OptionDataPtr option;
    int32_t position = 0;
    OptionGreeks positionGreeks;  // Greeks scaled by position
    double marketValue = 0;       // Current market value
    double theoValue = 0;         // Theoretical value
    double pnl = 0;               // Position P&L
    double fees = 0;              // accumulated trading fees
    double netPnl = 0;            // pnl - fees
    
    void update();
};

using OptionRiskDataPtr = std::shared_ptr<OptionRiskData>;



/**
 * @brief Per-expiry Greeks aggregation
 */
class ExpiryGreeks {
public:
    ExpiryGreeks(uint32_t expiry);
    
    uint32_t getExpiry() const { return m_expiry; }
    
    const OptionGreeks& getOptionGreeks() const { return m_optionGreeks; }
    double getUnderlierDelta() const { return m_underlierDelta; }
    double getPortfolioDelta() const;
    
    void setOptionGreeks(const OptionGreeks& greeks);
    void setUnderlierDelta(double delta) { m_underlierDelta = delta; }
    
    void reset();
    void accumulate(const OptionRiskData& data);
    
private:
    uint32_t m_expiry;
    OptionGreeks m_optionGreeks;
    double m_underlierDelta;
    double m_expiryFraction;  // Weight for expiry
};

using ExpiryGreeksPtr = std::shared_ptr<ExpiryGreeks>;

/**
 * @brief Hedge instrument data
 */
class HedgeData {
public:
    HedgeData(uint32_t expiry, const std::string& hedgeCode);
    
    const std::string& getCode() const { return m_hedgeCode; }
    uint32_t getExpiry() const { return m_expiry; }
    
    int32_t getPosition() const { return m_position; }
    void setPosition(int32_t pos);
    
    double getDeltaPosition() const;
    double getMultiplier() const { return m_multiplier; }
    void setMultiplier(double mult) { m_multiplier = mult; }
    
private:
    std::string m_hedgeCode;
    uint32_t m_expiry;
    int32_t m_position;
    double m_multiplier;
};

using HedgeDataPtr = std::shared_ptr<HedgeData>;

/**
 * @brief Risk data change listener
 */
class IOptionRiskListener {
public:
    virtual ~IOptionRiskListener() = default;
    
    virtual void onRiskUpdated(const class OptionRisk& risk) {}
    virtual void onPositionChanged(const OptionRiskData& data) {}
    virtual void onGreeksChanged(const ExpiryGreeks& greeks) {}
};

/**
 * @brief Portfolio risk manager
 * 
 * Tracks positions and aggregates Greeks across all options.
 */
class OptionRisk : public IOptionGridListener {
public:
    OptionRisk(OptionGridPtr grid);
    
    // Update risk calculations
    void update();
    
    // Option risk data access
    OptionRiskDataPtr getRiskData(const std::string& code);
    std::vector<OptionRiskDataPtr> getAllRiskData() const;
    std::vector<OptionRiskDataPtr> getNonZeroPositions() const;
    std::vector<OptionRiskDataPtr> getPositionsByExpiry(uint32_t expiry) const;
    
    // Greeks aggregation
    const OptionGreeks& getPositionGreeks() const { return m_positionGreeks; }
    const OptionGreeks& getOptionGreeks() const { return m_optionGreeks; }
    
    // Delta calculations
    double getDelta() const;              // Option delta only
    double getUnderlierDelta() const;     // Hedge delta
    double getPortfolioDelta() const;     // Total delta
    double getTotalDelta() const;         // Unmodified delta
    
    // Per-expiry Greeks
    ExpiryGreeksPtr getExpiryGreeks(uint32_t expiry);
    
    // Hedge management (Synced with UnderlyingTradingData)
    void addHedgeInstrument(uint32_t expiry, const std::string& hedgeCode);
    HedgeDataPtr getHedgeData(const std::string& code);
    const std::vector<HedgeDataPtr>& getHedgeInstruments() const { return m_hedges; }
    void updateHedgePosition(const std::string& code, int32_t position);
    
    // Position updates
    void syncPositionsFromTrader(wtp::TraderAdapter* trader);
    void setPosition(const std::string& code, int32_t position);
    void addFill(const std::string& code, int32_t qty, double price, double fee = 0.0);
    
    // Listeners
    void addListener(IOptionRiskListener* listener);
    void removeListener(IOptionRiskListener* listener);
    
    // IOptionGridListener
    void onOptionAdded(const OptionData* option) override;
    void onGridUpdated() override;
    
    // Session callbacks
    void onSessionBegin(uint32_t tradingDate);
    void onSessionEnd(uint32_t tradingDate);
    
private:
    void recalculateGreeks();
    void notifyRiskUpdated();
    void notifyPositionChanged(const OptionRiskData& data);
    
    OptionGridPtr m_grid;
    
    std::map<std::string, OptionRiskDataPtr> m_riskData;
    std::vector<OptionRiskDataPtr> m_riskDataByIndex; // Fast mapping
    std::map<uint32_t, ExpiryGreeksPtr> m_expiryGreeks;
    std::vector<HedgeDataPtr> m_hedges;
    
    OptionGreeks m_positionGreeks;   // Position-weighted Greeks
    OptionGreeks m_optionGreeks;     // Option Greeks only (no hedge)
    double m_underlierDelta;         // Hedge delta
    
    std::vector<IOptionRiskListener*> m_listeners;
    bool m_autoUpdateGreeks;
    
    // Thread safety for multi-threaded access
};

using OptionRiskPtr = std::shared_ptr<OptionRisk>;

} // namespace wt_option
