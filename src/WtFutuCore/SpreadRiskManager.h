/*!
 * \file SpreadRiskManager.h
 * \brief Risk Management for Spread Arbitrage
 * 
 * Provides comprehensive risk controls:
 *   - Position limits and exposure management
 *   - Correlation breakdown detection
 *   - Liquidity risk assessment
 *   - VaR calculation
 *   - Convergence risk monitoring
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include "SpreadArbitrageTypes.h"
#include "../Share/RingBuffer.hpp"
#include "../Includes/FasterDefs.h"
#include <memory>
#include <vector>

namespace futu {

//==============================================================================
// Risk Manager Configuration
//==============================================================================

struct SpreadRiskConfig
{
    // Position limits
    double max_total_position;      ///< Maximum total spread position
    double max_single_pair;         ///< Maximum position per pair
    double max_leg_exposure;        ///< Maximum exposure per leg
    
    // Risk thresholds
    double max_var_99;              ///< Maximum 99% VaR
    double max_correlation_break;   ///< Correlation threshold for break
    double min_correlation;         ///< Minimum acceptable correlation
    
    // Liquidity
    double min_liquidity_score;     ///< Minimum liquidity score
    uint32_t min_daily_volume;      ///< Minimum daily volume
    
    // Convergence risk
    double max_divergence_zscore;   ///< Maximum Z-Score divergence
    uint32_t max_divergence_time;   ///< Maximum divergence time (seconds)
    
    // Expiry risk
    uint32_t min_days_to_expiry;    ///< Minimum days to expiry
    uint32_t warning_days_to_expiry;///< Warning threshold
    
    // Stop loss
    double portfolio_stop_loss;     ///< Portfolio-level stop loss
    double pair_stop_loss;          ///< Per-pair stop loss
    
    SpreadRiskConfig()
        : max_total_position(50.0)
        , max_single_pair(20.0)
        , max_leg_exposure(30.0)
        , max_var_99(100000.0)
        , max_correlation_break(0.3)
        , min_correlation(0.6)
        , min_liquidity_score(0.5)
        , min_daily_volume(1000)
        , max_divergence_zscore(5.0)
        , max_divergence_time(7200)
        , min_days_to_expiry(3)
        , warning_days_to_expiry(7)
        , portfolio_stop_loss(50000.0)
        , pair_stop_loss(10000.0)
    {}
};

//==============================================================================
// Risk Alert
//==============================================================================

struct RiskAlert
{
    enum class Level : uint8_t
    {
        INFO,       ///< Informational
        WARNING,    ///< Warning - action recommended
        CRITICAL,   ///< Critical - immediate action required
        EMERGENCY   ///< Emergency - force close required
    };
    
    enum class Type : uint8_t
    {
        POSITION_LIMIT,     ///< Position limit exceeded
        VAR_LIMIT,          ///< VaR limit exceeded
        CORRELATION_BREAK,  ///< Correlation breakdown
        LIQUIDITY_LOW,      ///< Low liquidity
        DIVERGENCE,         ///< Spread divergence
        EXPIRY_WARNING,     ///< Near expiry
        STOP_LOSS,          ///< Stop loss triggered
        CONVERGENCE_FAIL    ///< Convergence failure
    };
    
    std::string pair_id;            ///< Affected pair (empty = portfolio-wide)
    Level level;                    ///< Alert level
    Type type;                      ///< Alert type
    std::string message;            ///< Human-readable message
    double value;                   ///< Alert value
    double threshold;               ///< Threshold value
    uint64_t timestamp;             ///< Alert timestamp
    
    RiskAlert()
        : level(Level::INFO)
        , type(Type::POSITION_LIMIT)
        , value(0)
        , threshold(0)
        , timestamp(0)
    {}
};

//==============================================================================
// Portfolio Risk Summary
//==============================================================================

struct PortfolioRiskSummary
{
    double total_position;          ///< Total spread position
    double total_exposure;          ///< Total market exposure
    double var_99;                  ///< Portfolio VaR (99%)
    double max_drawdown;            ///< Current drawdown
    
    uint32_t active_pairs;          ///< Number of active pairs
    uint32_t pairs_at_risk;         ///< Pairs with risk warnings
    
    double avg_correlation;         ///< Average pair correlation
    double min_correlation;         ///< Minimum correlation
    uint32_t correlation_breaks;    ///< Number of correlation breaks
    
    double liquidity_score;         ///< Portfolio liquidity score
    
    bool has_stop_loss;             ///< Is stop loss triggered
    bool has_critical_alert;        ///< Is there a critical alert
    
    PortfolioRiskSummary()
        : total_position(0)
        , total_exposure(0)
        , var_99(0)
        , max_drawdown(0)
        , active_pairs(0)
        , pairs_at_risk(0)
        , avg_correlation(0)
        , min_correlation(0)
        , correlation_breaks(0)
        , liquidity_score(1)
        , has_stop_loss(false)
        , has_critical_alert(false)
    {}
};

//==============================================================================
// Spread Risk Manager
//==============================================================================

/// Callback type for getting contract expiry date
using ExpiryDateCallback = std::function<uint32_t(const std::string& code)>;

class SpreadRiskManager
{
public:
    SpreadRiskManager();
    ~SpreadRiskManager() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const SpreadRiskConfig& config) { _config = config; }
    const SpreadRiskConfig& getConfig() const { return _config; }
    
    /// Set callback for getting contract expiry date
    void setExpiryDateCallback(ExpiryDateCallback callback) { _expiry_callback = callback; }
    
    /// Set current trading date (YYYYMMDD format) for expiry calculation
    void setCurrentDate(uint32_t current_date) { _current_date = current_date; }
    
    //==========================================================================
    // State Management
    //==========================================================================
    
    /// Update state for a spread pair
    void updatePairState(const std::string& pair_id, const SpreadState& state);
    
    /// Update portfolio PnL
    void updatePortfolioPnL(double unrealized_pnl, double realized_pnl);
    
    //==========================================================================
    // Risk Assessment
    //==========================================================================
    
    /// Calculate risk metrics for a pair
    SpreadRiskMetrics calculatePairRisk(const std::string& pair_id) const;
    
    /// Calculate portfolio risk summary
    PortfolioRiskSummary calculatePortfolioRisk() const;
    
    /// Calculate VaR for a position
    double calculateVaR(const std::string& pair_id, double confidence = 0.99) const;
    
    //==========================================================================
    // Risk Checks
    //==========================================================================
    
    /// Check if new position is allowed
    bool canOpenPosition(const std::string& pair_id, double size) const;
    
    /// Check for correlation breakdown
    bool checkCorrelationBreak(const std::string& pair_id) const;
    
    /// Check for convergence failure
    bool checkConvergenceFailure(const std::string& pair_id) const;
    
    /// Generate risk alerts
    std::vector<RiskAlert> generateAlerts() const;
    
    //==========================================================================
    // Position Limits
    //==========================================================================
    
    /// Get allowed position size for a pair
    double getAllowedPositionSize(const std::string& pair_id) const;
    
    /// Get current position for a pair
    double getCurrentPosition(const std::string& pair_id) const;
    
    //==========================================================================
    // Management
    //==========================================================================
    
    void reset();
    
    /// Get active alerts
    const std::vector<RiskAlert>& getActiveAlerts() const { return _active_alerts; }
    
    /// Clear acknowledged alerts
    void clearAlerts() { _active_alerts.clear(); }
    
private:
    //==========================================================================
    // Internal Methods
    //==========================================================================
    
    void updateAlerts();
    double calculatePortfolioVaR(double confidence) const;
    bool checkPositionLimits(const std::string& pair_id, double size) const;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    SpreadRiskConfig _config;
    
    //==========================================================================
    // State
    //==========================================================================
    
    wtp::wt_hashmap<std::string, SpreadState> _pair_states;
    wtp::wt_hashmap<std::string, SpreadRiskMetrics> _pair_risks;
    
    // Portfolio-level state
    double _portfolio_unrealized_pnl;
    double _portfolio_realized_pnl;
    double _portfolio_peak_pnl;
    double _current_drawdown;
    
    // Alert management
    std::vector<RiskAlert> _active_alerts;
    uint64_t _last_alert_time;
    
    // Expiry date management
    ExpiryDateCallback _expiry_callback;
    uint32_t _current_date;         ///< Current trading date (YYYYMMDD)
    
    //==========================================================================
    // Internal Helper Methods
    //==========================================================================
    
    /// Calculate days between two dates (YYYYMMDD format)
    static int32_t calculateDaysBetween(uint32_t date1, uint32_t date2);
    
    /// Get days to expiry for a contract
    int32_t getDaysToExpiry(const std::string& code) const;
};

} // namespace futu
