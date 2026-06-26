// src/WtFutuCore/SpreadOptimizer.cpp
#include "SpreadOptimizer.h"
#include "../WTSTools/WTSLogger.h"
#include <algorithm>
#include <cmath>

namespace futu {

SpreadOptimizer::SpreadOptimizer(const std::string& code)
    : _code(code)
{
}

GLFTResult SpreadOptimizer::computeOptimalQuote(
    double midPrice,
    double contractDelta,
    const SignalContext& ctx,
    double alphaSensitivity,
    const PortfolioContext* pCtx
) const
{
    GLFTResult result;
    
    //==========================================================================
    // 1. Base Spread with GLFT Enhancement
    //==========================================================================
    result.base_spread = computeBaseSpread(ctx);
    
    //==========================================================================
    // 2. Spread Multiplier (统一管理所有非 delta 风险的 spread 扩大)
    //    - 毒性: 减少成交概率
    //    - 自成交校准: 高毒性时保护性扩大
    //    - 低置信度: 信号不确定时保护性扩大
    //==========================================================================
    double spread_mult = 1.0;
    
    double alpha = ctx.alpha.valid ? ctx.alpha.alpha : 0.0;
    double confidence = ctx.alpha.valid ? ctx.alpha.confidence : 0.0;
    
    // 2a. 毒性扩大 spread (全市场毒性，来自 PredictiveToxicity)
    //     仅当 toxicity_score 足过最小阈值时才应用，过滤噪声
    bool toxic_active = false;
    if (ctx.toxicity.valid && ctx.toxicity.toxic_detected && ctx.toxicity.toxicity_score > 0.05) {
        double tox_mult = 1.0 + ctx.toxicity.toxicity_score * _params.toxicity_spread_factor;
        spread_mult *= tox_mult;
        result.toxicity_adjustment = tox_mult - 1.0;
        toxic_active = true;
    }

    // 2b. 低置信度保护: 仅在 alpha 信号有效时生效
    //     无效信号(confidence=0 because alpha.valid=false)不应触发保护性扩大
    if (ctx.alpha.valid && confidence < _params.low_confidence_threshold) {
        double low_conf_mult = 1.0 + (_params.low_confidence_threshold - confidence) / _params.low_confidence_threshold * _params.low_confidence_spread_factor;
        spread_mult *= low_conf_mult;
    }

    // 2c. EMA 平滑 spread_mult，避免 1 tick 内 spread 跳变
    //     当无毒性事件且 raw spread_mult==1.0 时，加速向 1.0 衰减 (mean-reversion)
    constexpr double spread_mult_ema_alpha = 0.30;   // 0.15→0.30: 更快响应
    constexpr double spread_mult_decay_alpha = 0.50;  // 无风险时加速衰减
    if (spread_mult <= 1.0 && !toxic_active) {
        // 无风险事件: 加速向 1.0 收敛
        _smoothed_spread_mult = spread_mult_decay_alpha * spread_mult + (1.0 - spread_mult_decay_alpha) * _smoothed_spread_mult;
        // 额外 mean-reversion: 每tick向1.0拉回5%
        _smoothed_spread_mult += 0.05 * (1.0 - _smoothed_spread_mult);
    } else {
        _smoothed_spread_mult = spread_mult_ema_alpha * spread_mult + (1.0 - spread_mult_ema_alpha) * _smoothed_spread_mult;
    }

    // B-1 fix: 限制每tick最大变化率，防止毒性开关导致spread_mult震荡
    // 上行最大+10%/tick，下行最大-15%/tick（下行允许更快收缩以恢复报价竞争力）
    constexpr double max_up_rate = 0.10;
    constexpr double max_down_rate = 0.15;
    double prev = _smoothed_spread_mult;  // EMA后的值（赋值前）
    // 注意: 此时 _smoothed_spread_mult 已更新，但 spread_mult 还是旧值
    // 需要用上一次的 _smoothed_spread_mult 作为基准
    // 重新计算: prev_smoothed 是赋值前的值
    double new_smoothed = _smoothed_spread_mult;
    // 用上一次输出的 spread_mult 作为基准（即上一tick的最终值）
    double max_up = _last_output_spread_mult * (1.0 + max_up_rate);
    double max_down = _last_output_spread_mult * (1.0 - max_down_rate);
    _smoothed_spread_mult = std::max(max_down, std::min(max_up, new_smoothed));

    // 首tick初始化
    if (_last_output_spread_mult < 0.5) {
        _smoothed_spread_mult = new_smoothed;
    }

    spread_mult = _smoothed_spread_mult;
    _last_output_spread_mult = spread_mult;
    
    // 应用 spread multiplier
    result.base_spread *= spread_mult;
    
    //==========================================================================
    // 3. Fair Value with Alpha (置信度加权, 截断不超过 half_spread)
    //==========================================================================
    result.confidence_weight = _params.confidence_weight_min + 
        (_params.confidence_weight_max - _params.confidence_weight_min) * confidence;
    
    result.alpha_adjustment = alphaSensitivity * alpha * result.confidence_weight * _params.tick_size;
    
    double half_spread_price = (result.base_spread / 2.0) * _params.tick_size;
    if (std::abs(result.alpha_adjustment) > half_spread_price) {
        result.alpha_adjustment = (result.alpha_adjustment > 0 ? 1.0 : -1.0) * half_spread_price;
    }
    
    result.fair_value = midPrice + result.alpha_adjustment;
    
    //==========================================================================
    // 4. Delta Skew (统一用 delta)
    //    - 单合约 delta skew: 防止单合约头寸过度累积
    //    - 组合 delta skew: 防止组合整体头寸过度累积
    //==========================================================================
    double contractMaxDelta = pCtx ? pCtx->contract_max_delta : 0;
    double totalDelta = pCtx ? pCtx->total_delta : 0;
    
    double contract_skew = computeContractDeltaSkew(contractDelta, contractMaxDelta);
    double portfolio_skew = computePortfolioDeltaSkew(totalDelta);
    
    // v3 双维 skew：从"取较大者"改为加权求和（权重在 GLFTParams）
    // - portfolio_skew_weight=0.5: portfolio 维度（控总敞口，温和影响）
    // - contract_skew_weight=1.0:  contract 维度（控单合约+pos，主导力）
    // 旧路径(max)保留：若两个权重之和<=0则退回 max 模式（向前兼容）
    double delta_skew;
    if (_params.portfolio_skew_weight + _params.contract_skew_weight > 1e-9) {
        delta_skew = _params.portfolio_skew_weight * portfolio_skew 
                   + _params.contract_skew_weight * contract_skew;
    } else {
        delta_skew = (std::abs(portfolio_skew) > std::abs(contract_skew)) 
                     ? portfolio_skew : contract_skew;
    }
    
    //==========================================================================
    // 5. 综合偏移计算
    //    skew 最终截断上限为 half_spread
    //    这样高持仓时 skew 可以偏移到 half_spread，使一侧报价贴到 fair_value
    //==========================================================================
    double total_skew = delta_skew;
    
    double half_spread = result.base_spread / 2.0;
    // 修正1: tanh→clamp (线性截断)
    // tanh截断太弱: |tanh(x)|<1, 满仓时ask=+0.238永远贴不到mid
    // clamp让skew_raw=-1.0时total_skew=-1.0, ask精确贴mid
    total_skew = std::max(-half_spread, std::min(half_spread, total_skew));
    result.inventory_skew = total_skew;
    
    //==========================================================================
    // 7. Bid/Ask Prices
    //==========================================================================
    // skew_price乘以spread_mult是有意为之的设计（用户确认不得解耦）：
    // 毒性高时，库存偏斜应被放大（更积极地减仓），而不仅仅是加宽价差。
    // spread_mult扩大了half_spread_price（保护性），同时也放大skew（进攻性），
    // 两者协同才能在毒性环境下快速出清库存。
    double skew_price = total_skew * _params.tick_size * spread_mult;
    
    // 第二次截断上限改为 half_spread_price * spread_mult
    // 原代码截断到half_spread_price，当spread_mult>1时skew_price必然被截断，
    // 导致spread_mult放大skew的设计意图被抵消（skew被截断回half_spread_price）。
    // 修复后截断上限=half_spread_price*spread_mult，与skew_price的最大值一致：
    //   total_skew最大=half_spread, skew_price最大=half_spread*tick_size*spread_mult=half_spread_price*spread_mult
    double skew_limit = half_spread_price * spread_mult;
    if (std::abs(skew_price) > skew_limit) {
        skew_price = (skew_price > 0 ? 1.0 : -1.0) * skew_limit;
    }
    
    result.bid_price = result.fair_value - half_spread_price + skew_price;
    result.ask_price = result.fair_value + half_spread_price + skew_price;
    
    result.bid_price = std::floor(result.bid_price / _params.tick_size) * _params.tick_size;
    result.ask_price = std::ceil(result.ask_price / _params.tick_size) * _params.tick_size;
    
    //==========================================================================
    // 8. Crossed Quote Protection
    //==========================================================================
    if (result.bid_price >= result.ask_price) {
        result.pause_quoting = true;
        result.bid_price = result.fair_value - half_spread_price;
        result.ask_price = result.fair_value + half_spread_price;
        result.bid_price = std::floor(result.bid_price / _params.tick_size) * _params.tick_size;
        result.ask_price = std::ceil(result.ask_price / _params.tick_size) * _params.tick_size;
    }
    
    //==========================================================================
    // 9. Multipliers & Flags
    //==========================================================================
    result.spread_mult = result.base_spread / _params.base_spread;
    result.pause_quoting = result.pause_quoting || ctx.shouldPause() ||
                           (result.spread_mult >= _params.max_spread_mult * _params.pause_spread_mult_ratio);
    
    //==========================================================================
    // 10. Debug Log
    //==========================================================================
    WTSLogger::debug("[QUOTE] {} mid={:.2f} | alpha={:.4f}(conf={:.2f},adj={:.2f}) | "
                     "skew={:.2f}(d_skew={:.2f}) | "
                     "spread={:.2f}(mult={:.2f}) | bid={:.2f} ask={:.2f}",
                     _code, midPrice,
                     alpha, confidence, result.alpha_adjustment,
                     total_skew, delta_skew,
                     result.base_spread, spread_mult,
                     result.bid_price, result.ask_price);
    
    // if (ctx.alpha.valid || ctx.book_imbalance.valid) {
    //     WTSLogger::debug("[SIGNAL] {} alpha={:.4f}(conf={:.2f}) | "
    //                      "book_imb={:.2f} | vol_tier={} | widen={}",
    //                      _code,
    //                      ctx.alpha.valid ? ctx.alpha.alpha : 0.0,
    //                      ctx.alpha.valid ? ctx.alpha.confidence : 0.0,
    //                      ctx.book_imbalance.valid ? ctx.book_imbalance.simple_imbalance : 0.0,
    //                      static_cast<int>(ctx.volatility.vol_tier),
    //                      result.widen_spread ? "Y" : "N");
    // }
                           
    return result;
}

double SpreadOptimizer::computeBaseSpread(const SignalContext& ctx) const
{
    double avg_depth = (ctx.bid_depth + ctx.ask_depth) / 2.0;
    double depth_adj = (avg_depth <= 0) ? _params.no_depth_spread_mult : (1.0 / (1.0 + (avg_depth / _params.depth_normalization) * _params.depth_sensitivity * _params.depth_sensitivity_scale));
    
    double spread = _params.base_spread * depth_adj;
    
    if (ctx.volatility.valid) {
        double sigma_sq = ctx.volatility.vol_percentile / _params.vol_percentile_scale;
        spread += _params.phi * sigma_sq * _params.vol_scale;
    }
    
    return std::clamp(spread, _params.base_spread * _params.min_spread_mult, _params.base_spread * _params.max_spread_mult);
}

double SpreadOptimizer::computeContractDeltaSkew(double contractDelta, double contractMaxDelta) const
{
    if (contractMaxDelta <= 0) return 0.0;
    
    double utilization = contractDelta / contractMaxDelta;
    
    double direction = (utilization > 0) ? -1.0 : 1.0;
    // 修正2: 乘以inventory_skew_scale增强中持仓skew力度
    // scale=2.0时util=0.6→skew=-0.930(接近-half_base=-1.0), ask从+0.566降至+0.070
    return direction * std::pow(std::abs(utilization), _params.delta_skew_power) * _params.inventory_skew_scale;
}

double SpreadOptimizer::computePortfolioDeltaSkew(double totalDelta) const
{
    if (_params.portfolio_max_delta <= 0) return 0.0;
    
    double util = std::abs(totalDelta) / _params.portfolio_max_delta;
    if (util <= _params.delta_skew_threshold) return 0.0;
    
    double excess = util - _params.delta_skew_threshold;
    double direction = (totalDelta > 0) ? -1.0 : 1.0;
    return direction * _params.delta_skew_factor * std::pow(excess, _params.delta_skew_power);
}

} // namespace futu
