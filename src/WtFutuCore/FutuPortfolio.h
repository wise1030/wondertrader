/*!
 * \file FutuPortfolio.h
 * \brief Unified Portfolio Management for Futures Market Making
 * 
 * Merged from: InventoryManager + FutuPortfolio
 * 
 * Core responsibilities:
 *   - Track inventory across multiple contracts
 *   - Aggregate portfolio-level risk metrics (Delta, Exposure, P&L)
 *   - Compute quote skew based on inventory
 *   - Trigger auto-hedging when delta limits exceeded
 * 
 * Performance: All operations are O(1) using hash maps
 */
#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

namespace futu {

/// Portfolio/inventory parameters
struct PortfolioParams
{
    double      skew_factor;        ///< Skew intensity per unit of inventory
    double      max_skew;           ///< Maximum skew in price units
    double      delta_limit;        ///< Delta threshold for hedging
    double      hedge_ratio;        ///< Fraction of delta to hedge (0.0-1.0)
    double      target_inventory;   ///< Target inventory level (usually 0)
    double      max_delta;          ///< Maximum absolute portfolio delta
    double      max_exposure;       ///< Maximum total exposure
    double      max_loss;           ///< Maximum daily loss tolerance
    
    // 分级响应阈值 (占 max_delta 的比例)
    double      warn_threshold;     ///< 警告阈值 (默认 0.7 = 70%)
    double      reduce_threshold;   ///< 减仓阈值 (默认 0.85 = 85%)
    double      halt_threshold;     ///< 暂停阈值 (默认 0.95 = 95%)
    
    // 增强 skew 参数
    double      skew_sensitivity;   ///< Skew 灵敏度系数（增强非线性调整）
    double      aggressive_skew_threshold;  ///< 激进 skew 阈值 (delta 利用率)
    double      one_sided_threshold;        ///< 单向报价阈值 (delta 利用率)
                                    ///< 当利用率超过此值时，只报有利于减小库存的一边
    
    PortfolioParams()
        : skew_factor(0.0001)
        , max_skew(5.0)
        , delta_limit(50.0)
        , hedge_ratio(0.5)
        , target_inventory(0)
        , max_delta(100.0)
        , max_exposure(500.0)
        , max_loss(100000.0)
        , warn_threshold(0.7)
        , reduce_threshold(0.85)
        , halt_threshold(0.95)
        , skew_sensitivity(2.0)         // 增强灵敏度
        , aggressive_skew_threshold(0.5) // 50% 利用率开始激进 skew
        , one_sided_threshold(0.8)       // 80% 利用率开始单向报价
    {}
};

/// Per-contract state (unified from InventoryState + ContractState)
struct ContractState
{
    std::string code;           ///< Standard code (e.g. "CFFEX.IF.2503")
    double      multiplier;     ///< Contract multiplier
    double      tick_size;      ///< Minimum tick size
    double      hedge_ratio;    ///< Hedge ratio relative to anchor contract
    
    // Position state
    double      position;       ///< Net position (+ long, - short)
    double      avg_cost;       ///< Average cost
    double      unrealized_pnl; ///< Unrealized P&L
    
    // Market data
    double      last_price;     ///< Latest mid/last price
    double      bid1;           ///< Best bid
    double      ask1;           ///< Best ask
    
    // Other
    double      daily_pnl;      ///< Daily P&L
    bool        is_active;      ///< Whether actively quoting
    uint64_t    last_update;    ///< Last update timestamp
    
    // Per-contract limits
    double      max_position;   ///< Max position for this contract (0 = no limit)
    double      max_delta;      ///< Max delta for this contract (0 = no limit)
    
    ContractState()
        : multiplier(1), tick_size(1), hedge_ratio(1.0)
        , position(0), avg_cost(0), unrealized_pnl(0)
        , last_price(0), bid1(0), ask1(0)
        , daily_pnl(0), is_active(true), last_update(0)
        , max_position(0), max_delta(0)
    {}
    
    /// Compute delta contribution (Delta Cash)
    /// Returns 0 if last_price is invalid (<= 0) to avoid incorrect calculations
    inline double delta() const 
    { 
        if (last_price <= 0)
            return 0;  // 行情无效时返回 0，避免使用 fallback 值导致计算错误
        return position * multiplier * hedge_ratio * last_price; 
    }
    
    /// Compute exposure (Exposure Cash)
    /// Returns 0 if last_price is invalid (<= 0)
    inline double exposure() const 
    { 
        if (last_price <= 0)
            return 0;
        return std::abs(position) * multiplier * last_price; 
    }
    
    /// Check if position limit breached
    inline bool isPositionLimitBreached() const 
    { 
        return max_position > 0 && std::abs(position) > max_position; 
    }
    
    /// Check if delta limit breached
    inline bool isDeltaLimitBreached() const 
    { 
        return max_delta > 0 && std::abs(delta()) > max_delta; 
    }
};

/// Hedge action recommendation
struct HedgeAction
{
    std::string code;           ///< Contract to trade
    double      qty;            ///< Quantity (+ buy, - sell)
    double      price;          ///< Suggested price (0 = market)
    bool        is_urgent;      ///< Urgency flag
    
    HedgeAction() : qty(0), price(0), is_urgent(false) {}
};

/// Unified Portfolio Manager
class FutuPortfolio
{
public:
    FutuPortfolio();
    ~FutuPortfolio() {}
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setParams(const PortfolioParams& params) { _params = params; }
    const PortfolioParams& getParams() const { return _params; }
    
    void setAnchorContract(const std::string& code) { _anchor_code = code; }
    const std::string& getAnchorContract() const { return _anchor_code; }
    
    //==========================================================================
    // Contract Management
    //==========================================================================
    
    /// Add a contract to the portfolio (O(1))
    void addContract(const std::string& code, double multiplier, double tickSize, 
                     double hedgeRatio = 1.0, double maxPosition = 0, double maxDelta = 0);
    
    /// Remove a contract (O(1))
    void removeContract(const std::string& code);
    
    /// Get contract state - O(1) lookup
    inline ContractState* getContract(const std::string& code)
    {
        auto it = _code_to_state.find(code);
        return (it != _code_to_state.end()) ? it->second : nullptr;
    }
    
    inline const ContractState* getContract(const std::string& code) const
    {
        auto it = _code_to_state.find(code);
        return (it != _code_to_state.end()) ? it->second : nullptr;
    }
    
    /// Get all contracts
    const std::vector<ContractState>& getAllContracts() const { return _contracts; }
    
    //==========================================================================
    // Market Data Updates
    //==========================================================================
    
    /// Update from tick (O(1))
    void onTick(const char* stdCode, wtp::WTSTickData* tick);
    
    /// Mark to market with last price
    void markToMarket(const std::string& code, double lastPrice);
    
    //==========================================================================
    // Position Updates
    //==========================================================================
    
    /// Update position (O(1))
    void onPositionUpdate(const char* stdCode, double newPos);
    
    /// Update position with average cost
    void updatePosition(const std::string& code, double position, double avgCost = 0);
    
    //==========================================================================
    // Portfolio Risk Metrics (inline, no allocation)
    //==========================================================================
    
    /// Total portfolio delta
    inline double getTotalDelta() const
    {
        if (!_delta_dirty)
            return _cached_delta;
        
        _cached_delta = 0;
        for (const auto& c : _contracts)
            _cached_delta += c.delta();
        _delta_dirty = false;
        return _cached_delta;
    }
    
    /// Total absolute exposure
    inline double getTotalExposure() const
    {
        double exposure = 0;
        for (const auto& c : _contracts)
            exposure += c.exposure();
        return exposure;
    }
    
    /// Total unrealized P&L
    inline double getTotalUnrealizedPnL() const
    {
        double pnl = 0;
        for (const auto& c : _contracts)
            pnl += c.unrealized_pnl;
        return pnl;
    }
    
    /// Total daily P&L
    inline double getTotalPnL() const
    {
        double pnl = 0;
        for (const auto& c : _contracts)
            pnl += c.daily_pnl;
        return pnl;
    }
    
    /// Net inventory deviation from target
    inline double getInventoryDeviation() const
    {
        return getTotalDelta() - _params.target_inventory;
    }
    
    /// Get position for specific contract - O(1)
    inline double getPosition(const std::string& code) const
    {
        const ContractState* cs = getContract(code);
        return cs ? cs->position : 0;
    }
    
    //==========================================================================
    // Risk Limits
    //==========================================================================
    
    inline bool isDeltaBreached() const 
    { 
        return std::abs(getTotalDelta()) > _params.delta_limit; 
    }
    
    inline bool isExposureBreached() const
    {
        return getTotalExposure() > _params.max_exposure;
    }
    
    inline bool isLossBreached() const
    {
        return getTotalPnL() < -_params.max_loss;
    }
    
    /// Check if any single contract limit is breached
    inline bool isAnyContractLimitBreached() const
    {
        for (const auto& c : _contracts)
        {
            if (c.isPositionLimitBreached() || c.isDeltaLimitBreached())
                return true;
        }
        return false;
    }
    
    /// Get the first contract that breached its POSITION limit (持仓手数限制)
    inline const ContractState* getPositionBreachedContract() const
    {
        for (const auto& c : _contracts)
        {
            if (c.isPositionLimitBreached())
                return &c;
        }
        return nullptr;
    }
    
    /// Get the first contract that breached its DELTA limit (Delta Cash 限制)
    inline const ContractState* getDeltaBreachedContract() const
    {
        for (const auto& c : _contracts)
        {
            if (c.isDeltaLimitBreached())
                return &c;
        }
        return nullptr;
    }
    
    /// Get the first contract that breached any limit (for backward compatibility)
    /// @deprecated Use getPositionBreachedContract() or getDeltaBreachedContract() instead
    inline const ContractState* getBreachedContract() const
    {
        for (const auto& c : _contracts)
        {
            if (c.isPositionLimitBreached() || c.isDeltaLimitBreached())
                return &c;
        }
        return nullptr;
    }
    
    inline bool isAnyLimitBreached() const
    {
        return isDeltaBreached() || isExposureBreached() || isLossBreached() || isAnyContractLimitBreached();
    }
    
    inline double getDeltaUtilization() const
    {
        if (_params.delta_limit <= 0) return 1;
        return std::abs(getTotalDelta()) / _params.delta_limit;
    }
    
    //==========================================================================
    // Tiered Risk Response (分级响应机制)
    //==========================================================================
    
    /// Risk level enumeration for tiered response
    enum class RiskLevel : uint8_t
    {
        NORMAL = 0,       ///< Normal operations (< 50% utilization)
        WARNING = 1,    ///< Warning level (50-70% utilization)
        ELEVATED = 2,    ///< Elevated risk (70-85% utilization)
        HIGH = 3,        ///< High risk (85-95% utilization)
        CRITICAL = 4    ///< Critical risk (>= 95% utilization)
    };
    
    /// Get current risk level based on delta utilization
    inline RiskLevel getRiskLevel() const
    {
        double util = getDeltaUtilization();
        if (util < 0.5) return RiskLevel::NORMAL;
        if (util < 0.7) return RiskLevel::WARNING;
        if (util < 0.85) return RiskLevel::ELEVATED;
        if (util < 0.95) return RiskLevel::HIGH;
        return RiskLevel::CRITICAL;
    }
    
    /// Get spread multiplier based on risk level
    inline double getSpreadMultiplierByRisk(double max_spread_mult = 3.0) const
    {
        switch (getRiskLevel())
        {
            case RiskLevel::NORMAL:    return 1.0;
            case RiskLevel::WARNING:   return 1.0 + (max_spread_mult - 1.0) * 0.25;
            case RiskLevel::ELEVATED: return 1.0 + (max_spread_mult - 1.0) * 0.5;
            case RiskLevel::HIGH:     return 1.0 + (max_spread_mult - 1.0) * 0.75;
            case RiskLevel::CRITICAL: return max_spread_mult;
            default: return 1.0;
        }
    }
    
    /// Get quote size multiplier based on risk level
    inline double getQtyMultiplierByRisk() const
    {
        switch (getRiskLevel())
        {
            case RiskLevel::NORMAL:    return 1.0;
            case RiskLevel::WARNING:   return 0.9;
            case RiskLevel::ELEVATED: return 0.7;
            case RiskLevel::HIGH:     return 0.5;
            case RiskLevel::CRITICAL: return 0.3;
            default: return 1.0;
        }
    }
    
    /// Check if should pause quoting based on risk level
    inline bool shouldPauseQuoting() const
    {
        return getRiskLevel() >= RiskLevel::CRITICAL;
    }
    
    /// Check if should reduce quoting (partial pause)
    inline bool shouldReduceQuoting() const
    {
        return getRiskLevel() >= RiskLevel::HIGH;
    }
    
    //==========================================================================
    // Quote Adjustment (Legacy - deprecated)
    //==========================================================================
    
    /// Calculate quote skew based on inventory
    /// @deprecated Use SpreadOptimizer::computePortfolioSkew instead
    /// This method is kept for backward compatibility but will be removed
    double computeQuoteSkew(const std::string& code = "") const;
    
    //==========================================================================
    // Enhanced Quote Adjustment
    //==========================================================================
    
    /// 计算增强的 skew - 使用非线性调整，更激进地减小库存
    /// @param code 合约代码（可选，用于单合约 skew）
    /// @return 偏移量（正数向上，负数向下）
    double computeEnhancedSkew(const std::string& code = "") const;
    
    /// 检查是否应该只报有利于减仓的一边
    /// @param is_buy_side 是否是买边（true=报买单，false=报卖单）
    /// @return true 表示应该报这一边，false 表示应该跳过
    bool shouldQuoteSide(bool is_buy_side) const;
    
    /// 获取单向报价建议
    /// @return 0=双边报价，1=只报买（减空头），-1=只报卖（减多头）
    int getOneSidedQuoteSuggestion() const;
    
    //==========================================================================
    // Hedging
    //==========================================================================
    
    /// Check if hedging is needed
    bool needsHedging() const;
    
    /// Compute hedge action
    HedgeAction computeHedge(double target_delta = 0) const;
    
private:
    PortfolioParams _params;
    std::string _anchor_code;
    
    std::vector<ContractState> _contracts;
    
    /// O(1) contract lookup map
    wtp::wt_hashmap<std::string, ContractState*> _code_to_state;
    
    // Cached delta
    mutable double _cached_delta;
    mutable bool _delta_dirty;
    
    inline void invalidateCache() { _delta_dirty = true; }
};

} // namespace futu