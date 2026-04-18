// src/WtFutuCore/SpreadOptimizer.cpp
#include "SpreadOptimizer.h"
#include "../WTSTools/WTSLogger.h"
#include <algorithm>
#include <cmath>

namespace futu {

SpreadOptimizer::SpreadOptimizer()
    : _skew_sensitivity(2.0)
    , _aggressive_threshold(0.3)
{
}

GLFTResult SpreadOptimizer::computeOptimalQuote(
    double midPrice,
    double inventory,
    const SignalContext& ctx,
    double alphaSensitivity,
    const PortfolioContext* pCtx,
    const CalibrationResult* calib
) const
{
    GLFTResult result;
    
    //==========================================================================
    // 1. Fair Value with Alpha (置信度加权)
    //==========================================================================
    double alpha = ctx.alpha.valid ? ctx.alpha.alpha : 0.0;
    double confidence = ctx.alpha.valid ? ctx.alpha.confidence : 0.0;
    
    // 置信度加权：低置信度信号影响小，高置信度信号影响大
    // weight = min + (max - min) * confidence
    result.confidence_weight = _params.confidence_weight_min + 
        (_params.confidence_weight_max - _params.confidence_weight_min) * confidence;
    
    result.alpha_adjustment = alphaSensitivity * alpha * result.confidence_weight * _params.tick_size;
    result.fair_value = midPrice + result.alpha_adjustment;
    
    //==========================================================================
    // 2. Base Spread with GLFT Enhancement
    //==========================================================================
    result.base_spread = computeBaseSpread(ctx);
    
    // GLFT 波动率贡献: 近似 γ × σ² × T
    // 用 vol_percentile 替代 σ²，T 近似为日内剩余时间的归一化值
    if (ctx.volatility.valid) {
        double sigma_sq = ctx.volatility.vol_percentile / 100.0;  // 归一化到 [0, 1]
        result.glft_vol_contrib = _params.phi * sigma_sq * 10.0;  // 时间尺度因子 10.0
        result.base_spread += result.glft_vol_contrib;
    }
    
    //==========================================================================
    // 3. Book Imbalance Skew Adjustment (盘口不平衡影响 skew)
    //==========================================================================
    if (ctx.book_imbalance.valid && 
        std::abs(ctx.book_imbalance.simple_imbalance) > _params.book_imbalance_threshold) {
        // 盘口不平衡时，向不平衡方向偏移报价
        // 例如：买单多 → book_imbalance > 0 → skew < 0 → 买价上调、卖价上调
        result.book_imbalance_skew = -ctx.book_imbalance.simple_imbalance *
            _params.book_imbalance_skew_factor * _params.max_skew;    }
    
    //==========================================================================
    // 5. Self-Trade Calibration Skew (自成交校准影响)
    //==========================================================================
    if (calib && calib->sample_size >= _params.calibration_min_samples) {
        // direction_bias > 0 表示买方毒性高（买后价格下跌）
        // 应避免激进买入 → 正向 skew（提高买价，降低卖价）
        // direction_bias < 0 表示卖方毒性高（卖后价格上涨）
        // 应避免激进卖出 → 负向 skew（降低买价，提高卖价）
        result.calibration_skew = calib->direction_bias * 
            _params.calibration_skew_factor * calib->confidence * _params.max_skew;
    }
    
    //==========================================================================
    // 6. Inventory + Portfolio Skew
    //==========================================================================
    double inv_skew = 0;
    double contract_max_delta = pCtx ? pCtx->contract_max_delta : 0;
    if (pCtx) {
        // Multi-contract / Portfolio-aware Skew
        double port_skew = computePortfolioSkew(inventory, *pCtx, ctx.volatility.vol_tier);
        // Single contract Skew (归一化到 contract_max_delta, 阻止单合约暴露无限制增加)
        double single_skew = computeInventorySkew(inventory, ctx, 0, contract_max_delta);
        
        // 取绝对值较大者，保证任一维度的风险都得到充分反映
        inv_skew = (std::abs(port_skew) > std::abs(single_skew)) ? port_skew : single_skew;
        
        inv_skew += computeDeltaAwareSkew(pCtx->total_delta);
    } else {
        // Single contract Skew (归一化到 contract_max_delta)
        inv_skew = computeInventorySkew(inventory, ctx, 0, contract_max_delta);
    }
    
    //==========================================================================
    // 4. 综合偏移计算
    //==========================================================================
    double total_skew = inv_skew + result.book_imbalance_skew + result.calibration_skew;
    
    // Soft clamp via tanh: 保留边际调节能力，避免 hard clamp 死区
    // tanh(x/max) * max 在 |x| 小时接近线性，|x| 大时渐近 ±max_skew
    total_skew = _params.max_skew * std::tanh(total_skew / _params.max_skew);
    result.inventory_skew = total_skew;
    
    //==========================================================================
    // 5. Bid/Ask Prices
    //==========================================================================
    double skew_price = total_skew * _params.tick_size;
    double half_spread = result.base_spread / 2.0;
    double half_spread_price = half_spread * _params.tick_size;
    
    result.bid_price = result.fair_value - half_spread_price - skew_price;
    result.ask_price = result.fair_value + half_spread_price - skew_price;
    
    // Round to tick
    result.bid_price = std::floor(result.bid_price / _params.tick_size) * _params.tick_size;
    result.ask_price = std::ceil(result.ask_price / _params.tick_size) * _params.tick_size;
    
    //==========================================================================
    // 6. Crossed Quote Protection
    //==========================================================================
    if (result.bid_price >= result.ask_price) {
        result.pause_quoting = true;
        // 恢复默认报价
        result.bid_price = result.fair_value - half_spread_price;
        result.ask_price = result.fair_value + half_spread_price;
        result.bid_price = std::floor(result.bid_price / _params.tick_size) * _params.tick_size;
        result.ask_price = std::ceil(result.ask_price / _params.tick_size) * _params.tick_size;
    }
    
    //==========================================================================
    // 7. Multipliers & Flags
    //==========================================================================
    result.spread_mult = result.base_spread / _params.base_spread;
    result.widen_spread = (result.spread_mult > 1.5);
    result.pause_quoting = result.pause_quoting || ctx.shouldPause() ||
                           (result.spread_mult >= _params.max_spread_mult * 0.9);
    
    //==========================================================================
    // 8. Debug Log
    //==========================================================================
    WTSLogger::debug("[QUOTE] mid={:.2f} | alpha={:.4f}(conf={:.2f},adj={:.2f}) | "
                     "skew={:.2f}(inv={:.2f},book={:.2f},cal={:.2f}) | "
                     "spread={:.2f}(glft={:.2f}) | bid={:.2f} ask={:.2f}",
                     midPrice,
                     alpha, confidence, result.alpha_adjustment,
                     total_skew, inv_skew, result.book_imbalance_skew, result.calibration_skew,
                     result.base_spread, result.glft_vol_contrib,
                     result.bid_price, result.ask_price);
    
    // 详细信号日志
    if (ctx.alpha.valid || ctx.book_imbalance.valid) {
        WTSLogger::debug("[SIGNAL] alpha={:.4f}(conf={:.2f}) | "
                         "book_imb={:.2f} | vol_tier={} | widen={}",
                         ctx.alpha.valid ? ctx.alpha.alpha : 0.0,
                         ctx.alpha.valid ? ctx.alpha.confidence : 0.0,
                         ctx.book_imbalance.valid ? ctx.book_imbalance.simple_imbalance : 0.0,
                         static_cast<int>(ctx.volatility.vol_tier),
                         result.widen_spread ? "Y" : "N");
    }
                           
    return result;
}

double SpreadOptimizer::computeBaseSpread(const SignalContext& ctx) const
{
    double vol_pct = ctx.volatility.vol_percentile;
    double vol_adj = 1.0 + (vol_pct - 50.0) / 50.0 * _params.vol_sensitivity;
    
    double avg_depth = (ctx.bid_depth + ctx.ask_depth) / 2.0;
    double depth_adj = (avg_depth <= 0) ? 1.5 : (1.0 / (1.0 + (avg_depth / 100.0) * _params.depth_sensitivity * 0.2));
    
    double spread = _params.base_spread * vol_adj * depth_adj;
    if (ctx.shouldWiden()) spread *= 1.25;
    
    return std::clamp(spread, _params.base_spread * _params.min_spread_mult, _params.base_spread * _params.max_spread_mult);
}

double SpreadOptimizer::computeInventorySkew(double inventory, const SignalContext& ctx, double delta, double contract_max_delta) const
{
    // 单合约 inventory skew 实际就是单合约 delta skew
    // 使用 contract_max_delta 归一化
    double inv_skew = 0.0;
    if (contract_max_delta > 0) {
        // 归一化库存到 [-1, 1] 范围
        double utilization = inventory / contract_max_delta;

        // skew = sign(utilization) * |utilization|^1.5 * max_skew
        // 去除 phi 的影响，因为 max_skew 已经定义了最大的偏离界限，phi 只会使单合约 skew 失去作用
        // 使用 1.5 次方曲线，使得持仓较小时 skew 较小，持仓逼近极限时迅速放大
        double direction = (utilization > 0) ? 1.0 : -1.0;
        inv_skew = direction * std::pow(std::abs(utilization), 1.5) * _params.max_skew;
    } else {
        // 兜底：无 contract_max_delta 时使用原逻辑
        double phi = _params.phi;
        switch (ctx.volatility.vol_tier) {
            case VolTier::EXTREME:  phi *= 1.5; break;
            case VolTier::ELEVATED: phi *= 1.25; break;
            case VolTier::LOW:      phi *= 0.9; break;
            default: break;
        }
        inv_skew = phi * inventory * 0.5;  // 放大兜底系数
    }

    // 加上组合级 delta skew
    if (delta != 0) {
        inv_skew += computeDeltaAwareSkew(delta);
    }

    return inv_skew;
}
double SpreadOptimizer::computeDeltaAwareSkew(double delta) const
{
    if (_params.portfolio_max_delta <= 0) return 0.0;
    double util = std::abs(delta) / _params.portfolio_max_delta;
    if (util <= _params.delta_skew_threshold) return 0.0;
    
    double excess = util - _params.delta_skew_threshold;
    double direction = (delta > 0) ? -1.0 : 1.0;
    return direction * _params.delta_skew_factor * std::pow(excess, 1.5) * _params.max_skew;
}

double SpreadOptimizer::computePortfolioSkew(double singleInventory, const PortfolioContext& ctx, VolTier tier) const
{
    double phi = _params.phi;
    switch (tier) {
        case VolTier::EXTREME:  phi *= 1.5; break;
        case VolTier::ELEVATED: phi *= 1.25; break;
        case VolTier::LOW:      phi *= 0.9; break;
        default: break;
    }

    double portfolio_contribution = 0.0;
    for (const auto& rel : ctx.related) {
        if (std::abs(rel.correlation) < _params.min_correlation) continue;
        double contract_lots = ctx.current_multiplier * ctx.current_price * ctx.current_hedge_ratio;
        if (contract_lots <= 0 || rel.last_price <= 0) continue;
        double relation_ratio = (rel.multiplier * rel.last_price * rel.hedge_ratio) / contract_lots;
        portfolio_contribution += rel.correlation * rel.inventory * relation_ratio;
    }
    
    double total_deviation = singleInventory + portfolio_contribution * _params.portfolio_skew_weight;
    double skew = phi * total_deviation;
    
    // Nonlinear enhancement
    double threshold = _params.max_skew / _params.phi * _aggressive_threshold;
    if (std::abs(total_deviation) > threshold) {
        double excess_ratio = (std::abs(total_deviation) - threshold) / threshold;
        skew *= (1.0 + std::sqrt(excess_ratio) * _skew_sensitivity * 0.25);
    }
    
    return skew;
}

} // namespace futu