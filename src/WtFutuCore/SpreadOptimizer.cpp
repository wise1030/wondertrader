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

    // 2a. (V10-R10 已删除) 毒性扩大 spread — 毒性响应统一为"检测即双边暂停"
    //     (StrategyCoordinator TOXICITY 相位), 优化器不再消费毒性信号

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
    if (spread_mult <= 1.0) {
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
    // 4. Delta Skew (R14-2026-09-07: tick 量化档位)
    //    驱动量(保留双驱动加权结构与符号相抵语义):
    //      r_p = total_delta_skew / portfolio_max_delta  (组合带符号利用率, S1 纯化口径)
    //      r_c = contract_position_util                  (合约纯手数口径带符号利用率)
    //      driver = w_p·sign(r_p)·|r_p|^γ + w_c·sign(r_c)·|r_c|^γ
    //    S1(2026-09-15) 纯化: hr 逐 tick 价格重算使旧口径 (position×hr) 随行情抖动,
    //      翻转-成交相关仅 0-9% 实测; skew 驱动改用 纯手数合约项 + hr_slow(5min EMA)
    //      组合项, 消费方语义不变 (direction/sign 混合保留 R14 双驱动结构)
    //    幅度映射(整档量化, 消灭 sub-tick 死偏移 —— 09-07 夜盘实证:
    //      util=1.0 撞限总 skew 仅 0.67t, 未穿越 mid, 7 分钟只减 1 手):
    //      k = clamp(round(k_max × driver), ±k_max), 滞环 h=skew_hysteresis
    //    穿越授权: r_p≥1.0 或 |r_c|≥1.0 → |k| 上限 k_max (k_max=4/half=1 时
    //      减仓侧=mid−3t 可成交, 恰好 = half+cross_max_ticks 的旧扩展上限);
    //      未授权 → |k| ≤ floor(half_spread) (至多贴 mid, 义务合规)
    //==========================================================================
    double half_spread = result.base_spread / 2.0;

    // S1: 驱动输入纯化 (total_delta_skew 无效/未注入时回退旧口径, 保底不空转)
    double portfolio_delta_skew = (pCtx && pCtx->total_delta_skew != 0.0)
                                      ? pCtx->total_delta_skew
                                      : (pCtx ? pCtx->total_delta : 0.0);
    double contract_util_signed =
        (pCtx && pCtx->contract_position_util_valid) ? pCtx->contract_position_util
        : ((pCtx && pCtx->contract_realized_delta_util_valid) ? pCtx->contract_realized_delta_util : 0.0);

    double r_p_signed =
        (params.portfolio_max_delta > 0) ? portfolio_delta_skew / params.portfolio_max_delta : 0.0;
    bool cross_authorized = (std::abs(r_p_signed) >= 1.0) || (std::abs(contract_util_signed) >= 1.0);

    double driver = computeDeltaSkewDriver(portfolio_delta_skew, contract_util_signed);
    // 符号约定: driver>0 (多头库存) → skew<0 (整梯下移: bid 拉远防堆积, ask 压向/穿过 mid 减仓)
    int k = -quantizeSkewTicks(params.skew_tick_max * driver);

    double k_cap = cross_authorized ? params.skew_tick_max : std::floor(half_spread);
    double delta_skew = std::max(-k_cap, std::min(k_cap, (double)k));

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
    // F-core S3(2026-09-15): 增打 skew 驱动分量 r_p/r_c (纯化口径), 供翻转归因
    WTSLogger::debug("[QUOTE] {} mid={:.2f} | alpha={:.4f}(conf={:.2f},adj={:.2f}) | "
                     "skew={:.2f}(d_skew={:.2f}) | "
                     "spread={:.2f}(mult={:.2f}) | bid={:.2f} ask={:.2f} | r_p={:.3f} r_c={:.3f}",
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
                     result.ask_price,
                     r_p_signed,
                     contract_util_signed);

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

double SpreadOptimizer::computeDeltaSkewDriver(double portfolio_delta_signed, double contract_util_signed) const
{
    const GLFTParams params = snapshotParams(); // F20
    auto signedPow = [](double x, double gamma) {
        return (x >= 0.0 ? 1.0 : -1.0) * fastPow(std::abs(x), gamma);
    };
    double r_p = (params.portfolio_max_delta > 0) ? portfolio_delta_signed / params.portfolio_max_delta : 0.0;
    return params.portfolio_skew_weight * signedPow(r_p, params.delta_skew_power) +
           params.contract_skew_weight * signedPow(contract_util_signed, params.delta_skew_power);
}

int SpreadOptimizer::quantizeSkewTicks(double raw_ticks) const
{
    const GLFTParams params = snapshotParams(); // F20
    int k_max_i = (int)std::lround(params.skew_tick_max);
    int target = (int)std::lround(raw_ticks);
    target = std::max(-k_max_i, std::min(k_max_i, target));
    if (target == _last_skew_k)
        return _last_skew_k;

    // 滞环: raw 须越过 (当前档位 ± 0.5) 的切换边界再 ±h 才换档, 防边界振荡引起换单风暴
    double boundary = (double)_last_skew_k + 0.5 * (target > _last_skew_k ? 1.0 : -1.0);
    double h = params.skew_hysteresis;
    if ((target > _last_skew_k && raw_ticks >= boundary + h) ||
        (target < _last_skew_k && raw_ticks <= boundary - h))
        _last_skew_k = target; // 允许多档直跳 (raw 大幅突变时一步到位)
    return _last_skew_k;
}

} // namespace futu
