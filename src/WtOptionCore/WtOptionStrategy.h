/*!
 * \file WtOptionStrategy.h
 * \brief Complete WonderTrader HFT strategy for option trading
 * 
 * Fully integrated with OptionRisk, OrderManager, CurveFitter, and P&L tracking.
 */

#pragma once

#include "OptionTypes.h"
#include "OptionGrid.h"
#include "OptionOrder.h"
#include "OrderManager.h"
#include "OptionRisk.h"
#include "CurveFitter.h"
#include "Scanners/IScanModule.h"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>

#include "OptStrategy.h"

namespace wt_option {


/**
 * @brief Strategy risk limits
 */
struct RiskLimits {
    double maxDelta = 1000;             // Maximum delta exposure
    double maxGamma = 100;              // Maximum gamma exposure
    double maxVega = 10000;             // Maximum vega exposure
    double maxPositionPerOption = 100;  // Max position per option
    double maxTotalPosition = 1000;     // Max total position
    double maxLossPerDay = 100000;      // Daily loss limit
    double panicThreshold = 0.05;       // Panic threshold (5% move)
};

/**
 * @brief P&L tracking
 */
struct PnLTracker {
    double realizedPnL = 0;             // Realized P&L (from fills)
    double unrealizedPnL = 0;           // Unrealized P&L (mark-to-market)
    double totalPnL = 0;                // Total P&L
    double dayStartPnL = 0;             // P&L at start of day
    double dayPnL = 0;                  // Today's P&L
    
    double optionPnL = 0;               // P&L from options
    double hedgePnL = 0;                // P&L from hedges
    
    void update(double realized, double unrealized) {
        realizedPnL = realized;
        unrealizedPnL = unrealized;
        totalPnL = realized + unrealized;
        dayPnL = totalPnL - dayStartPnL;
    }
    
    void onNewDay() {
        dayStartPnL = totalPnL;
        dayPnL = 0;
    }
};

/**
 * @brief Option strategy configuration
 */
struct OptionStrategyConfig {
    std::string underlyingCode;
    std::string exchangeCode;
    std::string hedgeCode;              // Hedge instrument code
    
    double defaultVolatility = 0.2;
    double riskFreeRate = 0.03;
    int32_t traceLevel = 0;
    
    std::string pricerType = "composite"; // "composite" or "gammascalp"
    bool useCompositePricer = true; // Legacy, better to use pricerType
    
    RiskLimits riskLimits;
    
    // Curve fitter config
    bool enableCurveFitting = true;
    uint64_t curveFitPeriod = 60000000;  // 1 minute
    
    // Scanner configurations
    std::vector<ScannerConfigPtr> scannerConfigs;
};

/**
 * @brief Strategy state
 */
enum class StrategyState {
    Idle,           // Not started
    Running,        // Normal trading
    Paused,         // Paused (no new orders)
    Panicked,       // Panic mode (close all)
    Shutdown        // Shutting down
};

/**
 * @brief Trading context with state management
 */
class OptionTraderContext {
public:
    OptionTraderContext();
    
    // State management
    StrategyState getState() const { return m_state; }
    void setState(StrategyState state);
    
    bool isEnabled() const { return m_state == StrategyState::Running; }
    bool isPanicked() const { return m_state == StrategyState::Panicked; }
    bool canTrade() const { return m_state == StrategyState::Running; }
    
    // Time
    uint64_t getCurrentTime() const { return m_currentTime; }
    void setCurrentTime(uint64_t time) { m_currentTime = time; }
    uint32_t getCurrentDate() const { return m_currentDate; }
    void setCurrentDate(uint32_t date);
    
    // Trace
    int32_t getTraceLevel() const { return m_traceLevel; }
    void setTraceLevel(int32_t level) { m_traceLevel = level; }
    
    // Day events
    bool isNewDay() const { return m_isNewDay; }
    void clearNewDay() { m_isNewDay = false; }
    
private:
    StrategyState m_state;
    int32_t m_traceLevel;
    uint64_t m_currentTime;
    uint32_t m_currentDate;
    uint32_t m_prevDate;
    bool m_isNewDay;
};

using OptionTraderContextPtr = std::shared_ptr<OptionTraderContext>;



/**
 * @brief Smart Option Market Making Strategy
 * 
 * Refactored to use standard WonderTrader Engine/Context architecture.
 */
class WtOptionStrategy : public wtp::OptStrategy, 
                         public IScannerListener,
                         public IOrderListener {
public:
    WtOptionStrategy(const std::string& name, const OptionStrategyConfig& config);
    WtOptionStrategy(const OptionStrategyConfig& config);
    virtual ~WtOptionStrategy();
    
    // OptStrategy interface (wtp namespace)
    virtual void on_init(wtp::IOptStraCtx* ctx) override;
    virtual void on_session_begin(wtp::IOptStraCtx* ctx, uint32_t uTDate) override;
    virtual void on_session_end(wtp::IOptStraCtx* ctx, uint32_t uTDate) override;
    virtual void on_tick(wtp::IOptStraCtx* ctx, const char* stdCode, wtp::WTSTickData* newTick) override;
    virtual void on_tick_batch(wtp::IOptStraCtx* ctx) override;
    virtual void on_calculate(wtp::IOptStraCtx* ctx, uint32_t curDate, uint32_t curTime) override;

    //=========================================================================
    // Convenience methods (for standalone / external usage)
    //=========================================================================
    
    bool initialize();
    void shutdown();
    // Standalone onTick wrapper
    void onTick(const char* code, double price, double bid, double ask, uint64_t timestamp);
    void onFill(const char* code, uint32_t orderId, double price, uint32_t qty, bool isBuy);
    void onOrderResponse(uint32_t orderId, const char* code, bool success, const char* message);
    
    // Timer & Event handlers (internal)
    void onTimer(uint64_t time);
    void onBeginOfDay(uint32_t date);
    void onEndOfDay(uint32_t date);

protected:
    void updateMarketData(const char* code, double price, double bid, double ask, uint64_t timestamp);
    void runStrategyLogic(const char* code, double price, uint64_t timestamp);



    //=========================================================================
    // Configuration Loading (Static)
    //=========================================================================
    
    static OptionStrategyConfig loadConfig(const std::string& filepath);
    static std::vector<ScannerConfigPtr> loadScannerConfigs(const std::string& filepath);
    static RiskLimits loadRiskLimits(const std::string& filepath);




    //=========================================================================
    // Order Interface (for scanners to call)
    //=========================================================================
public:
    uint32_t sendOrder(const std::string& code, bool isBuy, double price, uint32_t qty);
    uint32_t sendQuote(const std::string& code, double bidPrice, uint32_t bidQty, double askPrice, uint32_t askQty);
    bool cancelOrder(uint32_t orderId);
    void cancelAllOrders();
    
protected:
    //=========================================================================
    // State Control
    //=========================================================================
    
    void start();
    void pause();
    void resume();
    void panic();
    void recover();
    
    StrategyState getState() const;
    bool isEnabled() const;
    
    //=========================================================================
    // Access
    //=========================================================================
public:
    OptionGrid* getOptionGrid() { return m_grid; }
    OptionRisk* getOptionRisk() { return m_risk; }
    GridOrderManager* getOrderManager() { return m_orderManager; }
    CurveFitter* getCurveFitter() { return m_curveFitter.get(); }
    OptionTraderContextPtr getContext() { return m_tradeContext; }
    
    const PnLTracker& getPnL() const { return m_pnl; }
    const RiskLimits& getRiskLimits() const { return m_config.riskLimits; }
    
protected:
    
    // Listeners
    //=========================================================================
    
    // IScannerListener
    void onScannerHit(const ScannerHitEvent& event) override;
    
    // IOrderListener
    void onOrderSent(const OptionOrder& order) override;
    void onOrderFill(const OptionOrder& order, const FillEvent& fill) override;
    void onOrderCancelled(const OptionOrder& order) override;
    void onOrderRejected(const OptionOrder& order, const std::string& reason) override;
    
public:
    // Trade Shock Access
    double getLastBuyFillPrice(const std::string& code) const {
        auto it = m_lastBuyFillPrice.find(code);
        return it != m_lastBuyFillPrice.end() ? it->second : 0.0;
    }
    
    double getLastSellFillPrice(const std::string& code) const {
        auto it = m_lastSellFillPrice.find(code);
        return it != m_lastSellFillPrice.end() ? it->second : 0.0;
    }
    
    uint64_t getLastFillTime(const std::string& code) const {
        auto it = m_lastFillTime.find(code);
        return it != m_lastFillTime.end() ? it->second : 0;
    }
    
    //=========================================================================
    // Scanner Management
    //=========================================================================
    
    void addScanner(IScanModulePtr scanner);
    void removeScanner(const std::string& name);
    const std::vector<IScanModulePtr>& getScanners() const { return m_scanners; }
    
protected:
    //=========================================================================
    // Override Points
    //=========================================================================
    
    virtual void onStrategyInit() {}
    virtual void onStrategyStart() {}
    virtual void onStrategyStop() {}
    virtual void onOptionHit(OptionData* option, double signal, const std::string& reason);
    virtual void onPanicTriggered() {}
    virtual void onRiskLimitBreached(const std::string& limitName, double value, double limit);
    virtual void onDayStart(uint32_t date) {}
    virtual void onDayEnd(uint32_t date) {}
    
    //=========================================================================
    // Internal
    //=========================================================================
    
    void computeAllValues();
    void updateRisk();
    void checkRiskLimits();
    void checkPanic();
    void updatePnL();
    void processOrders();
    
private:
    OptionStrategyConfig m_config;
    
    // Core components (Owned by WtOptContext)
    OptionGrid*       m_grid;
    OptionRisk*       m_risk;
    GridOrderManager* m_orderManager;
    
    // Components Owned by Strategy
    CurveFitterPtr    m_curveFitter;
    IOptionPricerPtr  m_pricer;
    
    // Strategy State Context
    OptionTraderContextPtr m_tradeContext;
    
    // Scanners
    std::vector<IScanModulePtr> m_scanners;
    
    // P&L tracking
    PnLTracker m_pnl;
    
    // State
    bool m_initialized;
    uint64_t m_lastComputeTime;
    uint64_t m_lastRiskCheckTime;
    double m_lastPanicPrice;
    // Trade Shock trackers
    std::unordered_map<std::string, double> m_lastBuyFillPrice;
    std::unordered_map<std::string, double> m_lastSellFillPrice;
    std::unordered_map<std::string, uint64_t> m_lastFillTime;
};

using WtOptionStrategyPtr = std::shared_ptr<WtOptionStrategy>;

} // namespace wt_option
