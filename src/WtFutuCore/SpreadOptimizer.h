/*!
 * \file SpreadOptimizer.h
 * \brief GLFT (Gueant-Lehalle-Tapia) Market Making Model
 * 
 * Implements the GLFT optimal market making framework:
 *   - Base spread from volatility and order book depth
 *   - Inventory skew for risk management
 *   - Integration with external Alpha signals
 * 
 * Core GLFT formulas:
 *   - Fair value: ŝ = s + η * α (mid-price + alpha adjustment)
 *   - Bid price: P_bid = ŝ - δ/2 - φ*q
 *   - Ask price: P_ask = ŝ + δ/2 - φ*q
 * 
 * Where:
 *   - s: current mid-price
 *   - α: alpha signal from MicroAlphaEngine
 *   - η: alpha sensitivity weight
 *   - δ: base spread (from volatility and depth)
 *   - φ: inventory penalty coefficient
 *   - q: current inventory position
 */
#pragma once

#include <string>
#include <cmath>
#include <cstdint>
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"
#include "../Share/RingBuffer.hpp"

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

namespace futu {

//==============================================================================
// GLFT Model Parameters
//==============================================================================

/// GLFT model configuration
struct GLFTParams
{
    // Base spread parameters
    double      base_spread;        ///< Base spread in ticks (minimum spread)
    double      tick_size;          ///< Minimum price increment
    double      vol_sensitivity;    ///< How volatility affects spread
    double      depth_sensitivity;  ///< How order book depth affects spread
    
    // Inventory skew parameters
    double      phi;                ///< Inventory penalty coefficient (Skew = φ * q)
    double      max_skew;           ///< Maximum skew in ticks
    
    // Portfolio-level inventory skew
    double      portfolio_skew_weight;  ///< Weight for portfolio-level skew (0-1)
    double      min_correlation;        ///< Minimum correlation to include in portfolio skew
    
    // 增强 skew 参数（由策略层统一设置）
    // 不在这里定义默认值，从策略配置获取
    
    // Spread bounds
    double      max_spread_mult;    ///< Maximum spread multiplier
    double      min_spread_mult;    ///< Minimum spread multiplier
    
    // Volatility estimation
    uint32_t    vol_window;         ///< Volatility calculation window (ticks)
    
    GLFTParams()
        : base_spread(2.0)
        , tick_size(0.2)
        , vol_sensitivity(1.0)
        , depth_sensitivity(0.5)
        , phi(0.01)              // Typical value: 0.01 - 0.1
        , max_skew(5.0)
        , portfolio_skew_weight(0.5)  // 50% weight on portfolio-level skew
        , min_correlation(0.5)        // Only consider correlations > 0.5
        , max_spread_mult(5.0)
        , min_spread_mult(0.5)
        , vol_window(100)
    {}
};

//==============================================================================
// Portfolio Inventory Context
//==============================================================================

/// Related contract inventory information
struct RelatedInventory
{
    std::string code;           ///< Contract code
    double      inventory;      ///< Net position (positive=long, negative=short)
    double      correlation;    ///< Correlation with current contract (-1 to 1)
    double      hedge_ratio;    ///< Hedge ratio (beta from regression)
    double      multiplier;     ///< Contract multiplier
    double      last_price;     ///< Last price
    
    RelatedInventory()
        : inventory(0), correlation(0), hedge_ratio(1.0), multiplier(1.0), last_price(1.0)
    {}
    
    RelatedInventory(const std::string& c, double inv, double corr, double hr = 1.0, double mult = 1.0, double px = 1.0)
        : code(c), inventory(inv), correlation(corr), hedge_ratio(hr), multiplier(mult), last_price(px)
    {}
};

/// Portfolio inventory context for multi-contract skew calculation
struct PortfolioContext
{
    double                      total_delta;     ///< Aggregate portfolio delta (Cash)
    double                      total_exposure;  ///< Total exposure (Cash)
    double                      current_multiplier;  ///< Current contract multiplier
    double                      current_hedge_ratio; ///< Current contract beta/hedge ratio
    double                      current_price;       ///< Current contract price
    std::vector<RelatedInventory> related;       ///< Related contract inventories
    
    PortfolioContext()
        : total_delta(0), total_exposure(0)
        , current_multiplier(1.0), current_hedge_ratio(1.0), current_price(1.0)
    {}
    
    /// Add a related contract
    void addRelated(const std::string& code, double inv, double corr, double hr = 1.0, double mult = 1.0, double px = 1.0)
    {
        related.emplace_back(code, inv, corr, hr, mult, px);
    }
    
    /// Clear all related contracts
    void clear() 
    { 
        total_delta = 0;
        total_exposure = 0;
        current_multiplier = 1.0;
        current_hedge_ratio = 1.0;
        current_price = 1.0;
        related.clear();
    }
};

//==============================================================================
// Market Microstructure
//==============================================================================

/// Market state snapshot
struct MarketSnapshot
{
    double      mid_price;          ///< Mid price
    double      spread;             ///< Observed bid-ask spread
    double      bid_vol;            ///< Bid volume at best
    double      ask_vol;            ///< Ask volume at best
    double      bid_depth;          ///< Total bid depth (weighted)
    double      ask_depth;          ///< Total ask depth (weighted)
    double      imbalance;          ///< Order book imbalance (-1 to 1)
    double      trade_flow;         ///< Net trade flow (buy - sell)
    uint64_t    timestamp;          ///< Timestamp (ms)
    
    MarketSnapshot()
        : mid_price(0), spread(0)
        , bid_vol(0), ask_vol(0)
        , bid_depth(0), ask_depth(0)
        , imbalance(0), trade_flow(0)
        , timestamp(0)
    {}
};

//==============================================================================
// GLFT Quote Result
//==============================================================================

/// Result from GLFT spread calculation
struct GLFTResult
{
    double      fair_value;         ///< Fair value (mid + alpha adjustment)
    double      bid_price;          ///< Optimal bid price
    double      ask_price;          ///< Optimal ask price
    double      base_spread;        ///< Calculated base spread (δ)
    double      inventory_skew;     ///< Inventory skew (φ * q)
    double      alpha_adjustment;   ///< Alpha adjustment (η * α)
    double      spread_mult;        ///< Spread multiplier applied
    bool        widen_spread;       ///< Should widen spread
    bool        pause_quoting;      ///< Should pause quoting
    
    GLFTResult()
        : fair_value(0), bid_price(0), ask_price(0)
        , base_spread(0), inventory_skew(0), alpha_adjustment(0)
        , spread_mult(1.0)
        , widen_spread(false), pause_quoting(false)
    {}
};

//==============================================================================
// GLFT Spread Optimizer
//==============================================================================

/// GLFT-based Spread Optimizer
class SpreadOptimizer
{
public:
    SpreadOptimizer();
    ~SpreadOptimizer() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setParams(const GLFTParams& params) { _params = params; }
    const GLFTParams& getParams() const { return _params; }
    
    void setContract(const std::string& code) { _code = code; }
    const std::string& getContract() const { return _code; }
    
    /// 设置增强 skew 参数（从策略统一配置传入）
    void setSkewEnhancement(double sensitivity, double aggressiveThreshold) {
        _skew_sensitivity = sensitivity;
        _aggressive_threshold = aggressiveThreshold;
    }
    
    //==========================================================================
    // Portfolio Context (for multi-contract market making)
    //==========================================================================
    
    /// Set portfolio context for cross-contract inventory skew
    void setPortfolioContext(const PortfolioContext& ctx) { _portfolio_ctx = ctx; }
    const PortfolioContext& getPortfolioContext() const { return _portfolio_ctx; }
    
    /// Clear portfolio context
    void clearPortfolioContext() { _portfolio_ctx.clear(); }
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// Update with new tick data
    void onTick(wtp::WTSTickData* tick);
    
    /// Update with trade information
    void onTrade(double price, double qty, bool isBuy);
    
    /// Update with fill information
    void onFill(double qty, bool wasCrossed);
    
    //==========================================================================
    // Volatility Estimation
    //==========================================================================
    
    /// Calculate realized volatility (standard deviation of returns)
    double getRealizedVolatility() const;
    
    /// Get volatility percentile (0-100)
    double getVolatilityPercentile() const;
    
    /// Check if volatility is elevated
    inline bool isVolatilityElevated() const
    {
        return getVolatilityPercentile() > 80.0;
    }
    
    //==========================================================================
    // GLFT Core Calculations
    //==========================================================================
    
    /// Compute optimal bid/ask prices using GLFT model (single contract)
    /// @param midPrice Current mid-price
    /// @param inventory Current inventory (positive=long, negative=short)
    /// @param alpha Alpha signal from MicroAlphaEngine (-1 to 1)
    /// @param alphaSensitivity Alpha sensitivity weight (η)
    GLFTResult computeOptimalQuote(
        double midPrice,
        double inventory,
        double alpha = 0.0,
        double alphaSensitivity = 0.0
    ) const;
    
    /// Compute optimal bid/ask with portfolio-level inventory skew
    /// This considers correlated contracts' positions for hedging-aware quotes
    /// @param midPrice Current mid-price
    /// @param singleInventory Current contract inventory
    /// @param portfolioCtx Portfolio context with related inventories
    /// @param alpha Alpha signal (-1 to 1)
    /// @param alphaSensitivity Alpha sensitivity (η)
    GLFTResult computeOptimalQuoteWithPortfolio(
        double midPrice,
        double singleInventory,
        const PortfolioContext& portfolioCtx,
        double alpha = 0.0,
        double alphaSensitivity = 0.0
    ) const;
    
    /// Calculate base spread (δ) from market conditions
    double computeBaseSpread() const;
    
    /// Calculate inventory skew (single contract)
    double computeInventorySkew(double inventory) const;
    
    /// Calculate portfolio-level inventory skew
    /// Formula: Skew_port = φ * (q_single + Σ ρ_i * q_related * hedge_ratio_i)
    double computePortfolioSkew(double singleInventory, const PortfolioContext& ctx) const;
    
    //==========================================================================
    // Individual Adjustment Factors
    //==========================================================================
    
    /// Get spread adjustment from volatility
    double getVolatilityAdjustment() const;
    
    /// Get spread adjustment from order book depth
    double getDepthAdjustment() const;
    
    //==========================================================================
    // Market State
    //==========================================================================
    
    const MarketSnapshot& getMarketSnapshot() const { return _current_market; }
    
    //==========================================================================
    // Statistics
    //==========================================================================
    
    double getAvgSpread() const;
    double getAvgTradeSize() const;
    uint32_t getTradeCount() const { return _trade_count; }
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void reset();
    
private:
    std::string _code;
    GLFTParams _params;
    PortfolioContext _portfolio_ctx;  ///< Portfolio context for multi-contract skew
    
    // 增强 skew 参数（从策略统一配置传入）
    double _skew_sensitivity;
    double _aggressive_threshold;
    
    // Price history for volatility - using RingBuffer for zero allocation
    static constexpr size_t BUFFER_SIZE = 128;
    RingBuffer<double, BUFFER_SIZE> _price_history;
    RingBuffer<double, BUFFER_SIZE> _return_history;
    
    // Market state
    MarketSnapshot _current_market;
    
    // Trade statistics
    uint32_t _trade_count;
    double _total_trade_size;
    double _net_trade_flow;
    
    // Fill statistics
    uint32_t _crossed_fills;
    uint32_t _total_fills;
    
    // Cached volatility
    mutable double _cached_vol;
    mutable bool _vol_dirty;
    
    void updateVolatilityCache() const;
};

} // namespace futu
