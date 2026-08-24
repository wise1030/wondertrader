// src/WtFutuCore/SpreadOptimizer.cpp
#include "SpreadOptimizer.h"
#include "../../WTSTools/WTSLogger.h"
#include <algorithm>
#include <cmath>

namespace futu
{

// perf#9: 常用幂次特化, 避免 libm pow (~40ns/次). power 来自配置,
// 默认 1.5; == 比较安全 (配置加载的字面量精确表示).
// 加固(2026-08-24② A3): x<0 时非整数幂 = NaN -- 现有两条调用路径均有 x>=0 守卫
// (contract util>=0 :271 / portfolio excess>=0 :297), 此处兜底防御未来新调用点.
static inline double fastPow(double x, double power)
{
    if (x < 0.0)
        return 0.0;
    if (power == 1.5)
        return x * std::sqrt(x);
    if (power == 2.0)
        return x * x;
    if (power == 1.0)
        return x;
    if (power == 0.5)
        return std::sqrt(x);
    return std::pow(x, power);
}

SpreadOptimizer::SpreadOptimizer(const std::string& code) : _code(code) {}

GLFTResult SpreadOptimizer::computeOptimalQuote(double midPrice,
                                                double contractDelta,
                                                const SignalContext& ctx,
                                                double alphaSensitivity,
                                                const PortfolioContext* pCtx) const
{
    const GLFTParams params = snapshotParams(); // F20: seqlock 一致性快照, 防热更新撕裂读
    GLFTResult result;
    (void)contractDelta; // API 兼容保留; skew 输入由 pCtx->contract_delta_util 注入

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
    //     B4: 门槛参数化 (原硬编码 0.05), 键 toxicityMinScore
    bool toxic_active = false;
    if (ctx.toxicity.valid && ctx.toxicity.toxic_detected && ctx.toxicity.toxicity_score > params.toxicity_min_score) {
        double tox_mult = 1.0 + ctx.toxicity.toxicity_score * params.toxicity_spread_factor;
        spread_mult *= tox_mult;
        result.toxicity_adjustment = tox_mult - 1.0;
        toxic_active = true;
    }

    // 2b. 低置信度保护: 仅在 alpha 信号有效时生效
    //     无效信号(confidence=0 because alpha.valid=false)不应触发保护性扩大
    if (ctx.alpha.valid && confidence < params.low_confidence_threshold) {
        double low_conf_mult = 1.0 + (params.low_confidence_threshold - confidence) / params.low_confidence_threshold *
                                         params.low_confidence_spread_factor;
        spread_mult *= low_conf_mult;
    }

    // 2c. EMA 平滑 spread_mult，避免 1 tick 内 spread 跳变
    //     当无毒性事件且 raw spread_mult==1.0 时，加速向 1.0 衰减 (mean-reversion)
    constexpr double spread_mult_ema_alpha = 0.30;   // 0.15→0.30: 更快响应
    constexpr double spread_mult_decay_alpha = 0.50; // 无风险时加速衰减
    constexpr double mean_reversion_alpha = 0.05;    // 每 tick 向 1.0 额外拉回 5%
    if (spread_mult <= 1.0 && !toxic_active) {
        // 无风险事件: 加速向 1.0 收敛
        _smoothed_spread_mult =
            spread_mult_decay_alpha * spread_mult + (1.0 - spread_mult_decay_alpha) * _smoothed_spread_mult;
        // 额外 mean-reversion: 每tick向1.0拉回5%
        _smoothed_spread_mult += mean_reversion_alpha * (1.0 - _smoothed_spread_mult);
    } else {
        _smoothed_spread_mult =
            spread_mult_ema_alpha * spread_mult + (1.0 - spread_mult_ema_alpha) * _smoothed_spread_mult;
    }

    // B-1 fix: 限制每tick最大变化率，防止毒性开关导致spread_mult震荡
    // 上行最大+10%/tick，下行最大-15%/tick（下行允许更快收缩以恢复报价竞争力）
    // 速率基准 = 上一 tick 的最终输出值 (_last_output_spread_mult), 而非本轮 EMA 值
    // 首 tick (_last_output 初值 0): 跳过限幅直接采用 EMA 值 (原 `<0.5` 魔数等价改写)
    constexpr double max_up_rate = 0.10;
    constexpr double max_down_rate = 0.15;
    const double ema_value = _smoothed_spread_mult; // EMA 后、限幅前
    const double max_up = _last_output_spread_mult * (1.0 + max_up_rate);
    const double max_down = _last_output_spread_mult * (1.0 - max_down_rate);
    _smoothed_spread_mult = std::max(max_down, std::min(max_up, ema_value));

    if (!_mult_initialized) {
        _mult_initialized = true;
        _smoothed_spread_mult = ema_value;
    }

    spread_mult = _smoothed_spread_mult;
    _last_output_spread_mult = spread_mult;

    // 应用 spread multiplier
    result.base_spread *= spread_mult;

    //==========================================================================
    // 3. Fair Value with Alpha (置信度加权, 截断不超过 half_spread)
    //==========================================================================
    result.confidence_weight =
        params.confidence_weight_min + (params.confidence_weight_max - params.confidence_weight_min) * confidence;

    result.alpha_adjustment = alphaSensitivity * alpha * result.confidence_weight * params.tick_size;

    double half_spread_price = (result.base_spread / 2.0) * params.tick_size;
    if (std::abs(result.alpha_adjustment) > half_spread_price) {
        result.alpha_adjustment = (result.alpha_adjustment > 0 ? 1.0 : -1.0) * half_spread_price;
    }

    result.fair_value = midPrice + result.alpha_adjustment;

    //==========================================================================
    // 4. Delta Skew (统一 delta 口径, 2026-08-19 语义边界原则)
    //    - 单合约 delta skew: 防止单合约头寸过度累积
    //      (C2: 输入 = position×hedge_ratio/contract_max_delta 已实现口径,
    //       pending 投影仅用于 force_obligation/qty 衰减 —— skew 响应已实现库存)
    //    - 组合 delta skew: 防止组合整体头寸过度累积
    //==========================================================================
    double half_spread = result.base_spread / 2.0;

    // v7.1 连续控制: 已实现 delta_util ≥ 1.0 时授权减仓侧穿越 mid
    //   (C2: 从 projected 切换到 realized —— 主动减仓不被未成交挂单预授权)
    bool cross_authorized = pCtx && pCtx->contract_realized_delta_util_valid &&
                            std::abs(pCtx->contract_realized_delta_util) >= 1.0;

    double totalDelta = pCtx ? pCtx->total_delta : 0;

    // 单合约 skew: C2 起统一已实现口径 (单一路径);
    //       未注入 (contract_max_delta<=0) 时单合约 skew 为 0, 仅组合维度生效
    double contract_skew =
        (pCtx && pCtx->contract_realized_delta_util_valid)
            ? computeContractDeltaSkew(pCtx->contract_realized_delta_util, half_spread, params.skew_cross_max_ticks)
            : 0.0;
    // C1(2026-08-24②): 组合分量量纲统一到 ticks (×half_spread) ——
    //   此前返回无纲量被隐式当 ticks 用, 与 contract 分量(ticks)相加时相对力度随价差宽度漂移。
    double portfolio_skew = computePortfolioDeltaSkew(totalDelta, half_spread);

    // v3 双维 skew：从"取较大者"改为加权求和（权重在 GLFTParams）
    // - portfolio_skew_weight=0.5: portfolio 维度（控总敞口，温和影响）
    // - contract_skew_weight=1.0:  contract 维度（控单合约 delta，主导力）
    // 旧路径(max)保留：若两个权重之和<=0则退回 max 模式（向前兼容）
    double delta_skew;
    if (params.portfolio_skew_weight + params.contract_skew_weight > 1e-9) {
        delta_skew = params.portfolio_skew_weight * portfolio_skew + params.contract_skew_weight * contract_skew;
    } else {
        delta_skew = (std::abs(portfolio_skew) > std::abs(contract_skew)) ? portfolio_skew : contract_skew;
    }

    //==========================================================================
    // 5. 综合偏移计算
    //    skew 截断上限为 half_spread; v7.1: util≥1.0 时扩展到
    //    half_spread + skew_cross_max_ticks, 授权减仓侧穿越 mid 主动减仓
    //==========================================================================
    double total_skew = delta_skew;

    double clamp_limit = cross_authorized ? half_spread + params.skew_cross_max_ticks : half_spread;
    total_skew = std::max(-clamp_limit, std::min(clamp_limit, total_skew));
    result.inventory_skew = total_skew;

    //==========================================================================
    // 7. Bid/Ask Prices
    //==========================================================================
    // skew_price乘以spread_mult是有意为之的设计（用户确认不得解耦）：
    // 毒性高时，库存偏斜应被放大（更积极地减仓），而不仅仅是加宽价差。
    // spread_mult扩大了half_spread_price（保护性），同时也放大skew（进攻性），
    // 两者协同才能在毒性环境下快速出清库存。
    double skew_price = total_skew * params.tick_size * spread_mult;

    // 第二次截断上限 = (half_spread_price + 穿越授权扩展) * spread_mult
    // 截断上限与 total_skew 的最大值一致 (含 v7.1 穿越权限扩展),
    // 否则 spread_mult>1 时 skew 被截回, 放大设计意图被抵消
    double skew_limit =
        (half_spread_price + (cross_authorized ? params.skew_cross_max_ticks * params.tick_size : 0.0)) * spread_mult;
    if (std::abs(skew_price) > skew_limit) {
        skew_price = (skew_price > 0 ? 1.0 : -1.0) * skew_limit;
    }

    result.bid_price = result.fair_value - half_spread_price + skew_price;
    result.ask_price = result.fair_value + half_spread_price + skew_price;

    result.bid_price = std::floor(result.bid_price / params.tick_size) * params.tick_size;
    result.ask_price = std::ceil(result.ask_price / params.tick_size) * params.tick_size;

    //==========================================================================
    // 8. Crossed Quote Protection
    //==========================================================================
    if (result.bid_price >= result.ask_price) {
        result.pause_quoting = true;
        result.bid_price = result.fair_value - half_spread_price;
        result.ask_price = result.fair_value + half_spread_price;
        result.bid_price = std::floor(result.bid_price / params.tick_size) * params.tick_size;
        result.ask_price = std::ceil(result.ask_price / params.tick_size) * params.tick_size;
    }

    //==========================================================================
    // 9. Multipliers & Flags
    //==========================================================================
    result.spread_mult = result.base_spread / params.base_spread;
    result.pause_quoting = result.pause_quoting || ctx.shouldPause() ||
                           (result.spread_mult >= params.max_spread_mult * params.pause_spread_mult_ratio);

    //==========================================================================
    // 10. Debug Log
    //==========================================================================
    WTSLogger::debug("[QUOTE] {} mid={:.2f} | alpha={:.4f}(conf={:.2f},adj={:.2f}) | "
                     "skew={:.2f}(d_skew={:.2f}) | "
                     "spread={:.2f}(mult={:.2f}) | bid={:.2f} ask={:.2f}",
                     _code,
                     midPrice,
                     alpha,
                     confidence,
                     result.alpha_adjustment,
                     total_skew,
                     delta_skew,
                     result.base_spread,
                     spread_mult,
                     result.bid_price,
                     result.ask_price);

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
    const GLFTParams params = snapshotParams(); // F20
    double avg_depth = (ctx.bid_depth + ctx.ask_depth) / 2.0;
    double depth_adj = (avg_depth <= 0) ? params.no_depth_spread_mult
                                        : (1.0 / (1.0 + (avg_depth / params.depth_normalization) *
                                                            params.depth_sensitivity * params.depth_sensitivity_scale));

    double spread = params.base_spread * depth_adj;

    if (ctx.volatility.valid) {
        double sigma_sq = ctx.volatility.vol_percentile / params.vol_percentile_scale;
        spread += params.phi * sigma_sq * params.vol_scale;
    }

    return std::clamp(spread, params.base_spread * params.min_spread_mult, params.base_spread * params.max_spread_mult);
}

double
SpreadOptimizer::computeContractDeltaSkew(double signed_delta_util, double half_spread_ticks, double cross_max_ticks) const
{
    const GLFTParams params = snapshotParams(); // F20
    if (half_spread_ticks <= 0)
        return 0.0;

    double util = std::abs(signed_delta_util);
    if (util <= 1e-9)
        return 0.0;

    double direction = (signed_delta_util > 0) ? -1.0 : 1.0;
    // v7.1 归一化库存 skew (delta 口径): skew_norm = util^power × gain
    //   norm=1.0 → 减仓侧贴 mid; gain=1.0, power=1.5 时:
    //   util=0.5→0.35×half, util=0.8→0.72×half, util=1.0→贴mid
    double norm = fastPow(util, params.delta_skew_power) * params.inventory_skew_gain;
    // 穿越权限: util≥1.0 时允许 norm>1.0 (减仓侧穿越 mid 主动减仓),
    //   上限 = 1 + cross_max/half; util<1.0 时 cap=1.0 保证义务合规
    double cap = 1.0;
    if (util >= 1.0 && cross_max_ticks > 0)
        cap = 1.0 + cross_max_ticks / half_spread_ticks;
    norm = std::min(norm, cap);
    return direction * norm * half_spread_ticks;
}

double SpreadOptimizer::computePortfolioDeltaSkew(double totalDelta, double half_spread_ticks) const
{
    const GLFTParams params = snapshotParams(); // F20
    if (params.portfolio_max_delta <= 0)
        return 0.0;
    if (half_spread_ticks <= 0)
        return 0.0;

    double util = std::abs(totalDelta) / params.portfolio_max_delta;
    if (util <= params.delta_skew_threshold)
        return 0.0;

    double excess = util - params.delta_skew_threshold;
    double direction = (totalDelta > 0) ? -1.0 : +1.0;
    // C1(2026-08-24②): ×half_spread_ticks 归一到 ticks —— 与单合约分量同量纲,
    //   加权权重(portfolioSkewWeight/contractSkewWeight)自此为稳定的相对力度语义。
    return direction * params.delta_skew_factor * fastPow(excess, params.delta_skew_power) * half_spread_ticks;
}

} // namespace futu
