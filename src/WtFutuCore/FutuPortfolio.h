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
#include <atomic>
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"
#include "../WTSTools/WTSLogger.h"

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

namespace futu {

/// Portfolio/inventory parameters
struct PortfolioParams
{
    double      hedge_ratio;        ///< Fraction of delta to hedge (0.0-1.0)
    double      portfolio_max_delta;  ///< 组合级 Delta 软限制，用于 skew 调制（非硬限制）
    double      hedge_delta_threshold; ///< 对冲触发Delta利用率阈值 (0.0-1.0, 默认0.8=80%时触发)
    uint32_t    hedge_cooldown_ms;   ///< 对冲冷却时间(ms)
    
    //==========================================================================
    // 风控硬限制（由 FutuRiskMonitor 读取，不属于组合管理范畴）
    //==========================================================================
    double      max_exposure;       ///< Maximum total exposure (硬限制)
    double      max_loss;           ///< Maximum daily loss tolerance (硬限制)
    
    PortfolioParams()
        : hedge_ratio(1.0)
        , portfolio_max_delta(50.0)
        , hedge_delta_threshold(0.8)
        , hedge_cooldown_ms(5000)
        , max_exposure(35000000.0)
        , max_loss(200000.0)
    {}
};

/// Per-contract state (unified from InventoryState + ContractState)
struct ContractState
{
    std::string code;           ///< Standard code (e.g. "CFFEX.IF.2503")
    double      multiplier;     ///< Contract multiplier
    double      tick_size;      ///< Minimum tick size
    double      hedge_ratio;    ///< Hedge ratio relative to anchor contract
    bool        hedge_ratio_initialized;  ///< 冷启动初始化标志：false=仍是默认 1.0，需要 on_tick 用纯货值比注入；true=已用货值比或 EMA β 初始化过
    
    // Position state
    double      position;       ///< Net position (+ long, - short)
    double      prev_position;  ///< Previous position for trade effect logging
    double      avg_cost;       ///< Average cost
    double      unrealized_pnl; ///< Unrealized P&L
    double      realized_pnl;   ///< Realized P&L (accumulated from closed positions)
    
    // Market data
    double      last_price;     ///< Latest mid/last price
    double      bid1;           ///< Best bid
    double      ask1;           ///< Best ask
    
    // Other
    double      daily_pnl;      ///< Daily P&L
    bool        is_active;      ///< Whether actively quoting
    uint64_t    last_update;    ///< Last update timestamp
    
    // Per-contract limits (硬限制)
    double      max_position;   ///< Max position for this contract (0 = no limit)
    double      target_position;///< Target position for this contract (default 0 = balanced)
    
    // Per-contract soft indicators (软指标)
    double      contract_max_delta;  ///< 单合约 delta 软限制，用于单合约 skew 计算 (0 = no limit)
    
    ContractState()
        : multiplier(1), tick_size(1), hedge_ratio(1.0), hedge_ratio_initialized(false)
        , position(0), prev_position(0), avg_cost(0), unrealized_pnl(0), realized_pnl(0)
        , last_price(0), bid1(0), ask1(0)
        , daily_pnl(0), is_active(true), last_update(0)
        , max_position(0), target_position(0), contract_max_delta(0)
    {}
    
    /// Compute delta contribution (等效手数)
    /// 风险管理角度：delta 回归最纯粹的手数概念，避免乘数带来的失真
    inline double delta() const
    {
        return position * hedge_ratio;
    }    
    /// Compute exposure (Exposure Cash) - 扣除目标持仓后的风险暴露
    /// 风险暴露 = |当前持仓 - 目标持仓| * 合约乘数 * 价格
    /// 目标持仓为 0 时退化为传统的 |position| * multiplier * price
    /// Returns 0 if last_price is invalid (<= 0)
    inline double exposure() const 
    { 
        if (last_price <= 0)
            return 0;
        // 扣除目标持仓后的风险暴露
        double excess_position = position - target_position;
        return std::abs(excess_position) * multiplier * last_price; 
    }
    
    /// Compute raw exposure without target adjustment (for reference)
    /// 原始暴露 = |当前持仓| * 合约乘数 * 价格
    inline double raw_exposure() const
    {
        if (last_price <= 0)
            return 0;
        return std::abs(position) * multiplier * last_price;
    }
    
    /// Check if position limit breached
    /// 注意：position恰好等于max_position时不算breach（合法持仓上限）
    inline bool isPositionLimitBreached() const 
    { 
        return max_position > 0 && std::abs(position) > max_position; 
    }
    
    /// Check if contract delta is high (软指标，用于单合约 skew 计算)
    /// 注意：这不是风控违规，只是用于判断是否需要增强 skew
    inline bool isContractDeltaHigh() const 
    { 
        return contract_max_delta > 0 && std::abs(delta()) > contract_max_delta * 0.8; 
    }
    
    /// Get contract delta utilization for skew calculation
    inline double getContractDeltaUtilization() const
    {
        if (contract_max_delta <= 0) return 0;
        return std::abs(delta()) / contract_max_delta;
    }
    
    /// Check if position exceeds target (need to reduce)
    /// @param threshold 超过目标持仓的比例阈值 (默认0.0表示超过即需平仓)
    /// @return true 表示需要减仓
    inline bool needsPositionReduction(double threshold = 0.0) const
    {
        if (target_position == 0)
        {
            // 目标为0，任何持仓偏离都需要减仓
            return std::abs(position) > threshold;
        }
        // 检查是否偏离目标持仓
        double deviation = std::abs(position - target_position);
        double allowed_deviation = std::abs(target_position) * threshold;
        return deviation > allowed_deviation;
    }
    
    /// Get position reduction quantity (positive = need to sell, negative = need to buy)
    /// @return 需要平仓的数量：正数表示需要卖出，负数表示需要买入
    inline double getPositionReductionQty() const
    {
        if (target_position == 0)
        {
            // 目标为0，返回当前持仓的相反数
            return position; // position>0 返回正数(需卖出)，position<0 返回负数(需买入)
        }
        // 返回超出目标的部分
        return position - target_position;
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
                     double hedgeRatio = 1.0, double maxPosition = 0, double maxDelta = 0, double targetPosition = 0);
    
    /// Remove a contract (O(1))
    void removeContract(const std::string& code);
    
    /// Get contract state - O(1) lookup
    inline ContractState* getContract(const std::string& code)
    {
        auto it = _code_to_state.find(code);
        return (it != _code_to_state.end()) ? &_contracts[it->second] : nullptr;
    }
    
    inline const ContractState* getContract(const std::string& code) const
    {
        auto it = _code_to_state.find(code);
        return (it != _code_to_state.end()) ? &_contracts[it->second] : nullptr;
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
    
    /// Update daily_pnl for a contract (daily_pnl = unrealized_pnl + realized_pnl)
    void updateDailyPnL(const std::string& code);
    
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
    
    /// Total portfolio delta (原始持仓 delta)
    /// NOT thread-safe: must be called from the same thread as onTick/onPositionUpdate
    inline double getTotalDelta() const
    {
        double delta = 0;
        for (const auto& c : _contracts)
            delta += c.delta();
        return delta;
    }
    
    /// Total portfolio delta 扣除目标持仓后的净 delta
    /// 用于 skew 计算和风险暴露评估
    inline double getNetDelta() const
    {
        double net_delta = 0;
        for (const auto& c : _contracts)
        {
            // 扣除目标持仓后的净 delta 贡献
            double excess_pos = c.position - c.target_position;
            net_delta += excess_pos * c.hedge_ratio;
        }
        return net_delta;
    }
    
    /// Total absolute exposure (扣除目标持仓，考虑多空对冲)
    /// 计算逻辑：分开统计多头暴露和空头暴露，取较大值
    /// 原因：多空方向相反可以相互对冲，净暴露风险更小
    inline double getTotalExposure() const
    {
        double long_exposure = 0;
        double short_exposure = 0;
        for (const auto& c : _contracts)
        {
            double exp = c.exposure();
            double net_pos = c.position - c.target_position;
            if (net_pos > 0)
                long_exposure += exp;
            else if (net_pos < 0)
                short_exposure += exp;
        }
        return std::max(long_exposure, short_exposure);
    }
    
    // 新增毛暴露计算，跨品种多空不能简单对冲
    // getTotalExposure取max低估了跨品种风险(如rb多头+I空头，品种不同无法对冲)
    // getTotalGrossExposure返回sum(long+short)，用于更严格的风控检查
    inline double getTotalGrossExposure() const
    {
        double total = 0;
        for (const auto& c : _contracts)
        {
            total += c.exposure();
        }
        return total;
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
    
    /// Net inventory deviation from target (扣除单合约目标持仓)
    /// 用于 skew 计算：偏离目标的程度
    inline double getInventoryDeviation() const
    {
        return getNetDelta();
    }
    
    /// Get position for specific contract - O(1)
    inline double getPosition(const std::string& code) const
    {
        const ContractState* cs = getContract(code);
        return cs ? cs->position : 0;
    }
    
    /// Get net position for specific contract (扣除目标持仓) - O(1)
    inline double getNetPosition(const std::string& code) const
    {
        const ContractState* cs = getContract(code);
        return cs ? (cs->position - cs->target_position) : 0;
    }
    
    //==========================================================================
    // Risk Limits
    //==========================================================================
    
    /// Check if portfolio delta is high (软指标，用于组合级 skew 计算)
    /// 不作为硬风控条件，仅用于判断是否需要加强 skew
    inline bool isPortfolioDeltaHigh() const 
    { 
        return _params.portfolio_max_delta > 0 && std::abs(getTotalDelta()) > _params.portfolio_max_delta * 0.8; 
    }
    
    /// Check if portfolio delta is critical (软指标，用于日志警告)
    inline bool isPortfolioDeltaCritical() const 
    { 
        return _params.portfolio_max_delta > 0 && std::abs(getTotalDelta()) > _params.portfolio_max_delta; 
    }
    
    /// Check if any single contract limit is breached (硬指标)
    inline bool isAnyContractLimitBreached() const
    {
        for (const auto& c : _contracts)
        {
            if (c.isPositionLimitBreached())
                return true;
        }
        return false;
    }
    
    /// Get the first contract that breached its POSITION limit (持仓手数限制，硬指标)
    inline const ContractState* getPositionBreachedContract() const
    {
        for (const auto& c : _contracts)
        {
            if (c.isPositionLimitBreached())
                return &c;
        }
        return nullptr;
    }
    
    /// Get position reduction quantity to bring position within limit
    /// @return 正数表示需要卖出，负数表示需要买入，0表示无需平仓
    /// 注意：返回类型为 double，避免 int32_t 截断和溢出风险
    inline double getPositionReductionToLimit(const ContractState& c) const
    {
        if (!c.isPositionLimitBreached())
            return 0;
        
        double pos = c.position;       // 保持 double，不截断
        double max_pos = c.max_position;
        
        if (pos > max_pos) {
            // 多头超限，需要卖出
            return pos - max_pos;
        } else if (pos < -max_pos) {
            // 空头超限，需要买入（返回负数）
            return pos + max_pos;
        }
        return 0;
    }
    
    /// Get all contracts that need position reduction (exceed target position)
    /// @param threshold 超过目标持仓的比例阈值
    /// @return 需要减仓的合约列表
    std::vector<const ContractState*> getContractsNeedingReduction(double threshold = 0.0) const;
    
    /// Check if any hard limit is breached (position, exposure, loss)
    inline bool isAnyLimitBreached() const
    {
        return isAnyContractLimitBreached();
    }
    
    /// Portfolio delta utilization based on net delta (扣除目标持仓)
    /// 用于组合级 skew 激进度计算
    inline double getPortfolioDeltaUtilization() const
    {
        if (_params.portfolio_max_delta <= 0) return 0;
        return std::abs(getNetDelta()) / _params.portfolio_max_delta;
    }
    
    /// Raw portfolio delta utilization (原始持仓)
    /// 用于组合级 skew 计算和对冲决策
    inline double getRawPortfolioDeltaUtilization() const
    {
        if (_params.portfolio_max_delta <= 0) return 0;
        return std::abs(getTotalDelta()) / _params.portfolio_max_delta;
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
        double util = getPortfolioDeltaUtilization();
        if (util < 0.5) return RiskLevel::NORMAL;
        if (util < 0.7) return RiskLevel::WARNING;
        if (util < 0.85) return RiskLevel::ELEVATED;
        if (util < 0.95) return RiskLevel::HIGH;
        return RiskLevel::CRITICAL;
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
    
    /// O(1) contract lookup map (stores index, not pointer, to avoid dangling pointer on vector resize)
    wtp::wt_hashmap<std::string, size_t> _code_to_state;
};

} // namespace futu