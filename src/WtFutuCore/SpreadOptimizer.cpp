/*!
 * \file SpreadOptimizer.cpp
 * \brief GLFT Market Making Model Implementation
 * 
 * GLFT Core Formulas:
 *   Fair Value:   ŝ = s + η * α
 *   Bid Price:    P_bid = ŝ - δ/2 - φ*q
 *   Ask Price:    P_ask = ŝ + δ/2 - φ*q
 * 
 * Where:
 *   s: mid-price, α: alpha signal, η: alpha sensitivity
 *   δ: base spread, φ: inventory penalty, q: inventory
 */
#include "SpreadOptimizer.h"
#include "../Includes/WTSDataDef.hpp"
#include <algorithm>
#include <numeric>

namespace futu {

SpreadOptimizer::SpreadOptimizer()
    : _trade_count(0)
    , _total_trade_size(0)
    , _net_trade_flow(0)
    , _crossed_fills(0)
    , _total_fills(0)
    , _cached_vol(0)
    , _vol_dirty(true)
    , _skew_sensitivity(2.0)       // 默认值，由策略统一设置
    , _aggressive_threshold(0.3)   // 默认值，由策略统一设置
{
}

void SpreadOptimizer::onTick(wtp::WTSTickData* tick)
{
    if (!tick) return;
    
    double bid = tick->bidprice(0);
    double ask = tick->askprice(0);
    if (bid <= 0 || ask <= 0) return;
    
    double mid = (bid + ask) / 2.0;
    
    // Update market snapshot
    _current_market.mid_price = mid;
    _current_market.spread = ask - bid;
    _current_market.bid_vol = tick->bidqty(0);
    _current_market.ask_vol = tick->askqty(0);
    _current_market.timestamp = tick->actiontime();
    
    // Calculate depth (sum of top 5 levels)
    _current_market.bid_depth = 0;
    _current_market.ask_depth = 0;
    for (int i = 0; i < 5; ++i)
    {
        _current_market.bid_depth += tick->bidqty(i);
        _current_market.ask_depth += tick->askqty(i);
    }
    
    // Calculate imbalance
    double total_vol = _current_market.bid_vol + _current_market.ask_vol;
    if (total_vol > 0)
    {
        _current_market.imbalance = 
            (_current_market.bid_vol - _current_market.ask_vol) / total_vol;
    }
    
    // Update price history
    _price_history.push(mid);
    
    // Calculate return and update return history
    if (_price_history.size() >= 2)
    {
        size_t idx = _price_history.size() - 1;
        double prev = _price_history[idx - 1];
        if (prev > 0)
        {
            double ret = (mid - prev) / prev;
            _return_history.push(ret);
        }
    }
    
    _vol_dirty = true;
}

void SpreadOptimizer::onTrade(double price, double qty, bool isBuy)
{
    _trade_count++;
    _total_trade_size += qty;
    
    // Update net trade flow
    double signed_qty = isBuy ? qty : -qty;
    _current_market.trade_flow += signed_qty;
    _net_trade_flow += signed_qty;
}

void SpreadOptimizer::onFill(double qty, bool wasCrossed)
{
    _total_fills++;
    if (wasCrossed)
        _crossed_fills++;
}

void SpreadOptimizer::updateVolatilityCache() const
{
    if (!_vol_dirty || _return_history.size() < 10)
    {
        _vol_dirty = false;
        return;
    }
    
    // Calculate standard deviation of returns
    double sum = 0, sum_sq = 0;
    for (size_t i = 0; i < _return_history.size(); ++i)
    {
        double r = _return_history[i];
        sum += r;
        sum_sq += r * r;
    }
    
    double n = static_cast<double>(_return_history.size());
    double mean = sum / n;
    double variance = (sum_sq / n) - (mean * mean);
    
    _cached_vol = std::sqrt(std::max(0.0, variance));
    _vol_dirty = false;
}

double SpreadOptimizer::getRealizedVolatility() const
{
    updateVolatilityCache();
    return _cached_vol;
}

double SpreadOptimizer::getVolatilityPercentile() const
{
    double vol = getRealizedVolatility();
    
    // Typical intraday return std for futures
    if (vol < 0.0003) return 10.0;
    if (vol < 0.0005) return 25.0;
    if (vol < 0.001) return 50.0;
    if (vol < 0.002) return 70.0;
    if (vol < 0.003) return 85.0;
    return 95.0;
}

double SpreadOptimizer::getVolatilityAdjustment() const
{
    double vol_pct = getVolatilityPercentile();
    
    // Higher volatility = wider spread
    // At 50th percentile, adjustment = 1.0
    return 1.0 + (vol_pct - 50.0) / 50.0 * _params.vol_sensitivity;
}

double SpreadOptimizer::getDepthAdjustment() const
{
    // Lower depth = wider spread (liquidity risk)
    double avg_depth = (_current_market.bid_depth + _current_market.ask_depth) / 2.0;
    
    if (avg_depth <= 0) return 1.5;  // No depth, widen spread
    
    // Normalize: typical depth is 50-200 lots
    double depth_ratio = avg_depth / 100.0;
    
    // Clamp: deeper market = tighter spread
    return 1.0 / (1.0 + depth_ratio * _params.depth_sensitivity * 0.2);
}

double SpreadOptimizer::computeBaseSpread() const
{
    // Base spread from parameters (in ticks)
    double base_spread = _params.base_spread;
    
    // Adjust for volatility
    double vol_adj = getVolatilityAdjustment();
    
    // Adjust for depth
    double depth_adj = getDepthAdjustment();
    
    // Combined spread
    double spread = base_spread * vol_adj * depth_adj;
    
    // Apply bounds
    spread = std::max(_params.base_spread * _params.min_spread_mult,
                      std::min(_params.base_spread * _params.max_spread_mult, spread));
    
    return spread;
}

double SpreadOptimizer::computeInventorySkew(double inventory) const
{
    // GLFT inventory skew: Skew_q = φ * q
    double skew = _params.phi * inventory;
    
    // Clamp to max skew
    skew = std::clamp(skew, -_params.max_skew, _params.max_skew);
    
    return skew;
}

GLFTResult SpreadOptimizer::computeOptimalQuote(
    double midPrice,
    double inventory,
    double alpha,
    double alphaSensitivity) const
{
    GLFTResult result;
    
    //==========================================================================
    // Step 1: Calculate fair value with alpha adjustment
    // ŝ = s + η * α
    //==========================================================================
    result.alpha_adjustment = alphaSensitivity * alpha;
    result.fair_value = midPrice + result.alpha_adjustment;
    
    //==========================================================================
    // Step 2: Calculate base spread (δ)
    //==========================================================================
    result.base_spread = computeBaseSpread();
    double half_spread = result.base_spread / 2.0;
    
    //==========================================================================
    // Step 3: Calculate inventory skew
    // Skew_q = φ * q
    //==========================================================================
    result.inventory_skew = computeInventorySkew(inventory);
    
    //==========================================================================
    // Step 4: Calculate bid and ask prices
    // P_bid = ŝ - δ/2 - Skew_q
    // P_ask = ŝ + δ/2 - Skew_q
    //==========================================================================
    
    // Note: inventory_skew is in ticks, convert to price
    double skew_price = result.inventory_skew * _params.tick_size;
    double half_spread_price = half_spread * _params.tick_size;
    
    result.bid_price = result.fair_value - half_spread_price - skew_price;
    result.ask_price = result.fair_value + half_spread_price - skew_price;
    
    // Round to tick size
    result.bid_price = std::floor(result.bid_price / _params.tick_size) * _params.tick_size;
    result.ask_price = std::ceil(result.ask_price / _params.tick_size) * _params.tick_size;
    
    // Ensure bid < ask (crossed quotes check)
    if (result.bid_price >= result.ask_price)
    {
        result.pause_quoting = true;
        result.bid_price = result.fair_value - half_spread_price;
        result.ask_price = result.fair_value + half_spread_price;
    }
    
    //==========================================================================
    // Step 5: Set flags for risk management
    //==========================================================================
    result.spread_mult = result.base_spread / _params.base_spread;
    result.widen_spread = (result.spread_mult > 1.5);
    result.pause_quoting = result.pause_quoting || (result.spread_mult >= _params.max_spread_mult * 0.9);
    
    return result;
}

//==============================================================================
// Portfolio-Level Inventory Skew
//==============================================================================

double SpreadOptimizer::computePortfolioSkew(double singleInventory, 
                                              const PortfolioContext& ctx) const
{
    // Single contract skew
    double single_skew = _params.phi * singleInventory;
    
    // Portfolio-level contribution from correlated contracts
    // Formula: Skew_port = Σ ρ_i * q_related * hedge_ratio_i
    double portfolio_contribution = 0.0;
    
    for (const auto& rel : ctx.related)
    {
        // Only consider significant correlations
        double abs_corr = std::abs(rel.correlation);
        if (abs_corr < _params.min_correlation)
            continue;
        
        // Weighted inventory contribution:
        // Convert related contract position to current contract lot equivalents
        double contract_lots = ctx.current_multiplier * ctx.current_price * ctx.current_hedge_ratio;
        if (contract_lots <= 0) contract_lots = 1.0;
        
        double relation_ratio = (rel.multiplier * rel.last_price * rel.hedge_ratio) / contract_lots;
        
        double weighted_inv = rel.correlation * rel.inventory * relation_ratio;
        portfolio_contribution += weighted_inv;
    }
    
    // Combine single and portfolio skews
    // portfolio_skew_weight controls how much portfolio context affects final skew
    double portfolio_skew = _params.phi * portfolio_contribution * _params.portfolio_skew_weight;
    
    // Total skew = single_skew + portfolio_adjustment
    double total_skew = single_skew + portfolio_skew;
    
    // ========================================================================
    // 增强的非线性 skew 调整（使用统一配置参数）
    // 当库存偏离严重时，使用指数增强使其更激进地回归中性
    // ========================================================================
    
    // 计算总库存偏离度
    double total_inventory_deviation = singleInventory + portfolio_contribution * _params.portfolio_skew_weight;
    
    // 当偏离超过阈值时，使用非线性增强
    // 阈值设为 max_skew 的 aggressive_threshold 比例对应的库存量
    double inventory_threshold = _params.max_skew / _params.phi * _aggressive_threshold;
    
    if (std::abs(total_inventory_deviation) > inventory_threshold)
    {
        // 计算增强因子
        double excess = std::abs(total_inventory_deviation) - inventory_threshold;
        double excess_ratio = excess / inventory_threshold;
        
        // 使用平方根增强（比线性更激进但不会过度），系数使用统一配置
        double enhancement = 1.0 + std::sqrt(excess_ratio) * _skew_sensitivity * 0.25;
        
        // 应用增强，保持方向
        total_skew *= enhancement;
    }
    
    // 当库存偏离非常严重时（超过 80% 的 max_skew），添加额外的方向性 skew
    double critical_threshold = _params.max_skew / _params.phi * 0.6;
    if (std::abs(total_inventory_deviation) > critical_threshold)
    {
        // 额外的方向性调整
        double critical_excess = (std::abs(total_inventory_deviation) - critical_threshold) / critical_threshold;
        double direction_skew = (total_inventory_deviation > 0 ? -1.0 : 1.0) * 
                                _params.max_skew * critical_excess * _skew_sensitivity * 0.15;
        total_skew += direction_skew;
    }
    
    // Clamp to max skew
    total_skew = std::clamp(total_skew, -_params.max_skew, _params.max_skew);
    
    return total_skew;
}

GLFTResult SpreadOptimizer::computeOptimalQuoteWithPortfolio(
    double midPrice,
    double singleInventory,
    const PortfolioContext& portfolioCtx,
    double alpha,
    double alphaSensitivity) const
{
    GLFTResult result;
    
    //==========================================================================
    // Step 1: Calculate fair value with alpha adjustment
    // ŝ = s + η * α
    //==========================================================================
    result.alpha_adjustment = alphaSensitivity * alpha;
    result.fair_value = midPrice + result.alpha_adjustment;
    
    //==========================================================================
    // Step 2: Calculate base spread (δ)
    //==========================================================================
    result.base_spread = computeBaseSpread();
    double half_spread = result.base_spread / 2.0;
    
    //==========================================================================
    // Step 3: Calculate portfolio-level inventory skew
    // Considers correlated contracts for hedging-aware quotes
    //==========================================================================
    result.inventory_skew = computePortfolioSkew(singleInventory, portfolioCtx);
    
    //==========================================================================
    // Step 4: Additional adjustment based on total portfolio delta
    // If portfolio delta is extreme, widen spread or pause quoting
    //==========================================================================
    double delta_adjustment = 0.0;
    if (portfolioCtx.total_exposure > 0)
    {
        // Normalize delta to exposure ratio
        double delta_ratio = portfolioCtx.total_delta / portfolioCtx.total_exposure;
        
        // If heavily long (delta_ratio > 0.5) or short (delta_ratio < -0.5)
        // add extra skew to encourage mean reversion
        if (std::abs(delta_ratio) > 0.5)
        {
            double normalize_factor = portfolioCtx.current_multiplier * portfolioCtx.current_price * portfolioCtx.current_hedge_ratio;
            if (normalize_factor <= 0) normalize_factor = 1.0;
            
            double normalized_lots = portfolioCtx.total_delta / normalize_factor;
            delta_adjustment = _params.phi * normalized_lots * 0.5;
        }
    }
    
    //==========================================================================
    // Step 5: Calculate bid and ask prices
    // P_bid = ŝ - δ/2 - Skew_total
    // P_ask = ŝ + δ/2 - Skew_total
    //==========================================================================
    
    // Combine inventory skew with delta adjustment
    double total_skew = result.inventory_skew + delta_adjustment;
    total_skew = std::clamp(total_skew, -_params.max_skew, _params.max_skew);
    
    // Save the combined clamped skew back to result so callers get the safe bounded value
    result.inventory_skew = total_skew;
    
    // Convert to price
    double skew_price = total_skew * _params.tick_size;
    double half_spread_price = half_spread * _params.tick_size;
    
    result.bid_price = result.fair_value - half_spread_price - skew_price;
    result.ask_price = result.fair_value + half_spread_price - skew_price;
    
    // Round to tick size
    result.bid_price = std::floor(result.bid_price / _params.tick_size) * _params.tick_size;
    result.ask_price = std::ceil(result.ask_price / _params.tick_size) * _params.tick_size;
    
    // Ensure bid < ask (crossed quotes check)
    if (result.bid_price >= result.ask_price)
    {
        result.pause_quoting = true;
        result.bid_price = result.fair_value - half_spread_price;
        result.ask_price = result.fair_value + half_spread_price;
    }
    
    //==========================================================================
    // Step 6: Set flags for risk management
    //==========================================================================
    result.spread_mult = result.base_spread / _params.base_spread;
    result.widen_spread = (result.spread_mult > 1.5);
    
    // Pause if spread too wide or portfolio risk too high
    result.pause_quoting = result.pause_quoting || 
                           (result.spread_mult >= _params.max_spread_mult * 0.9);
    
    return result;
}

double SpreadOptimizer::getAvgSpread() const
{
    return _current_market.spread;
}

double SpreadOptimizer::getAvgTradeSize() const
{
    if (_trade_count == 0) return 0;
    return _total_trade_size / _trade_count;
}

void SpreadOptimizer::reset()
{
    _price_history.clear();
    _return_history.clear();
    _trade_count = 0;
    _total_trade_size = 0;
    _net_trade_flow = 0;
    _crossed_fills = 0;
    _total_fills = 0;
    _cached_vol = 0;
    _vol_dirty = true;
    _current_market = MarketSnapshot();
}

} // namespace futu
