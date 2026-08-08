/*!
 * \file QuotePolicyChain.h
 * \brief 5A-2 (v7.5): 报价决策链模块化
 *
 * processQuoting 内联的 8 个决策阶段中, GLFT 之后的 6 个调整阶段抽为
 * 策略对象链 (执行顺序与旧实现严格一致):
 *
 *   GLFT → RiskWiden → ArbCloseSync → Toxicity → LimitPrice → ColdStart
 *        → FillRetreat → 缓存+发布
 *
 * 等价性关键:
 *   - RiskWidenPolicy._mult 保留旧 _risk_spread_mult 全部写入语义
 *     (soft 每 tick 无条件覆盖 / hard max 闩锁 / 恢复清零);
 *   - ToxicityPolicy._resume_time 保留旧 _toxicity_resume_time
 *     (checkRisk 冷却查询 + processQuoting 抑制 + resetSession 清零);
 *   - 阈值/舍入 (L1×1.2 L2×1.5, 涨跌停 20/10/0.5 ticks, floor/ceil
 *     tick 对齐) 逐行搬迁。
 */
#pragma once

#include <string>
#include <cstdint>
#include <atomic>
#include <cmath>
#include <algorithm>
#include "../WTSTools/WTSLogger.h"
#include "SpreadOptimizer.h"
#include "ToxicFlowDetector.h"
#include "SpreadArbitrageManager.h"
#include "SelfTradeCalibrator.h"

namespace futu
{

/// 链上传递的可变报价决策
struct QuoteState
{
    double skew = 0.0;
    double spread_mult = 1.0;
    double l0_bid = 0.0;
    double l0_ask = 0.0;
    bool allow_bid = true;
    bool allow_ask = true;
};

/// 决策输入 (processQuoting 每 tick 构建)
struct QuotePolicyContext
{
    std::string code;
    double mid = 0.0;
    double tick_size = 0.0;
    double upper_limit = 0.0;
    double lower_limit = 0.0;
    uint64_t timestamp = 0;
    bool cold_start = false;

    // 服务依赖 (可空)
    const SpreadOptimizer* spread_opt = nullptr; ///< ColdStart 取参数
    SpreadArbitrageManager* arb_manager = nullptr;
    ToxicFlowDetector* toxicity = nullptr;
    SelfTradeCalibrator* calibrator = nullptr;

    // 模块配置
    bool use_toxicity_detector = false;
    uint64_t toxicity_cooloff_ms = 0;
};

class IQuotePolicy
{
public:
    virtual ~IQuotePolicy() = default;
    virtual void apply(const QuotePolicyContext& ctx, QuoteState& st) = 0;
};

//==============================================================================
// 1. RiskWidenPolicy — 软风控 WIDEN_SPREAD 倍数
//    写入语义与旧 _risk_spread_mult 完全一致:
//      tickSoft:   qphase != RISK_HALTED 时每 tick 重算并【覆盖】(无状态化,
//                  util 回落即回 1.0; 注意会覆盖同 tick 之前的 hard 闩锁,
//                  但 hard 闩锁在 soft 之后写入, 旧语义得以保留)
//      onHardWiden: violations WIDEN_SPREAD 分支, max 闩锁
//      reset:      恢复路径 (canRecover 恢复 / onExternalResumeFromRisk)
//==============================================================================
class RiskWidenPolicy : public IQuotePolicy
{
public:
    // v7.6: _mult 原子化 — tickSoft/onHardWiden 在 MdSpi (checkRisk),
    //       reset 在 TdSpi (onExternalResumeFromRisk), apply 读在 MdSpi
    void tickSoft(double cur_util, double l1, double l2, bool risk_halted)
    {
        if (risk_halted)
            return;
        double target = (cur_util >= l2) ? 1.5 : (cur_util >= l1) ? 1.2 : 1.0;
        double old = _mult.load(std::memory_order_acquire);
        if (target != old) {
            WTSLogger::debug(
                "[RISK] soft WIDEN_SPREAD stateless: spread_mult {:.1f}->{:.1f} (util={:.2f})", old, target, cur_util);
            _mult.store(target, std::memory_order_release);
        }
    }

    void onHardWiden(double cur_util, double l2)
    {
        double target_mult = (cur_util >= l2) ? 1.5 : 1.2; // R2.5: L2→×1.5, L1→×1.2
        // max 闩锁的 CAS 形式 (并发 reset 下不丢失"至少 target"语义)
        double old = _mult.load(std::memory_order_acquire);
        while (target_mult > old &&
               !_mult.compare_exchange_weak(old, target_mult, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
        WTSLogger::warn("[RISK] WIDEN_SPREAD: spread_mult={:.1f} (util={:.2f}, L{})",
                        _mult.load(std::memory_order_acquire),
                        cur_util,
                        cur_util >= l2 ? 2 : 1);
    }

    void reset() { _mult.store(1.0, std::memory_order_release); }
    double multiplier() const { return _mult.load(std::memory_order_acquire); }

    void apply(const QuotePolicyContext& /*ctx*/, QuoteState& st) override
    {
        st.spread_mult *= _mult.load(std::memory_order_acquire);
    }

private:
    std::atomic<double> _mult{1.0};
};

//==============================================================================
// 2. ArbCloseSyncPolicy — B2: ARB 平仓协同抑制
//    arb 卖 leg → 抑制 MM bid; arb 买 leg → 抑制 MM ask。
//    含 B6 MarketMakingEnhancer 观测模式 (|agg_z|>0.1 时 debug 日志, 不注入)。
//==============================================================================
class ArbCloseSyncPolicy : public IQuotePolicy
{
public:
    /// v7.7 性能#3: B6 观测模式开关 (默认 false; 观测期置 true)
    static inline std::atomic<bool> s_observe_enhancer{false};

    void apply(const QuotePolicyContext& ctx, QuoteState& st) override
    {
        if (!ctx.arb_manager)
            return;

        int arb_close_dir = ctx.arb_manager->getArbCloseDirection(ctx.code);
        if (arb_close_dir > 0) {
            st.allow_ask = false;
            WTSLogger::debug("[ARB-SYNC] {} arb buying leg, suppress MM ask", ctx.code);
        } else if (arb_close_dir < 0) {
            st.allow_bid = false;
            WTSLogger::debug("[ARB-SYNC] {} arb selling leg, suppress MM bid", ctx.code);
        }

        // B6: MarketMakingEnhancer 观测模式 — 计算 adjustment 但暂不注入 skew.
        // v7.7 性能#3: 运行期开关 (默认关) — 开启前每 tick 无条件算
        //   agg_z+adjustment 仅打 debug, 纯浪费 CPU。
        if (!s_observe_enhancer.load(std::memory_order_relaxed))
            return;
        double agg_z = ctx.arb_manager->getAggregateZscore(ctx.code);
        if (std::abs(agg_z) > 0.1) {
            auto adj = ctx.arb_manager->getQuotingAdjustmentForLeg(ctx.code, ctx.timestamp);
            if (adj.confidence > 0.0) {
                WTSLogger::debug(
                    "[ARB-ENH] {} agg_z={:.2f} adj[bid={:.3f},ask={:.3f},mult={:.2f},supB={},supA={}] (observe-only)",
                    ctx.code,
                    agg_z,
                    adj.bid_skew_adjustment,
                    adj.ask_skew_adjustment,
                    adj.spread_multiplier,
                    adj.suppress_bid,
                    adj.suppress_ask);
            }
        }
    }
};

//==============================================================================
// 3. ToxicityPolicy — 毒性抑制 + cooloff 冷却
//    _resume_time 与旧 _toxicity_resume_time 语义一致:
//    is_toxic → 设置冷却 + 按 toxic_side 抑制; 冷却期内双边抑制。
//==============================================================================
class ToxicityPolicy : public IQuotePolicy
{
public:
    // v7.6: _resume_time 原子化 — apply/inCooloff 在 MdSpi,
    //       reset 在 onSessionBegin (RtTicker 线程)
    bool inCooloff(uint64_t timestamp) const { return timestamp < _resume_time.load(std::memory_order_acquire); }

    void reset() { _resume_time.store(0, std::memory_order_release); }

    void apply(const QuotePolicyContext& ctx, QuoteState& st) override
    {
        if (!ctx.use_toxicity_detector || !ctx.toxicity)
            return;

        ToxicityMetrics tox = ctx.toxicity->analyze();

        if (tox.is_toxic) {
            // 设置冷却期：即使score短暂回落，也保持保护期
            _resume_time.store(ctx.timestamp + ctx.toxicity_cooloff_ms, std::memory_order_release);

            if (tox.toxic_side == 1) {
                st.allow_bid = false;
                WTSLogger::warn(
                    "[TOXIC] {} Buy-side toxic (score={:.2f}), pausing bid quotes", ctx.code, tox.toxic_score);
            } else if (tox.toxic_side == -1) {
                st.allow_ask = false;
                WTSLogger::warn(
                    "[TOXIC] {} Sell-side toxic (score={:.2f}), pausing ask quotes", ctx.code, tox.toxic_score);
            } else {
                st.allow_bid = false;
                st.allow_ask = false;
                WTSLogger::warn(
                    "[TOXIC] {} Both-side toxic (score={:.2f}), pausing all quotes", ctx.code, tox.toxic_score);
            }
        } else if (ctx.timestamp < _resume_time.load(std::memory_order_acquire)) {
            // 冷却期内：is_toxic已恢复，但仍在保护期
            st.allow_bid = false;
            st.allow_ask = false;
            WTSLogger::debug("[TOXIC] {} in cooloff (resume in {}ms)",
                             ctx.code,
                             _resume_time.load(std::memory_order_acquire) - ctx.timestamp);
        }
    }

private:
    std::atomic<uint64_t> _resume_time{0};
};

//==============================================================================
// 4. LimitPricePolicy — 涨跌停保护 (P0-2)
//    L1: 距涨跌停 <= 20 ticks → 加宽 spread 2x
//    L2: 距涨跌停 <= 10 ticks → block 加仓侧
//    L3: 锁板(mid ≈ 涨跌停) → 双边暂停
//==============================================================================
class LimitPricePolicy : public IQuotePolicy
{
public:
    void apply(const QuotePolicyContext& ctx, QuoteState& st) override
    {
        if (!(ctx.upper_limit > 0 && ctx.lower_limit > 0 && ctx.tick_size > 0))
            return;

        double dist_upper = (ctx.upper_limit - ctx.mid) / ctx.tick_size;
        double dist_lower = (ctx.mid - ctx.lower_limit) / ctx.tick_size;

        // L1: 距涨跌停 <= 20 ticks, 加宽 spread
        if (dist_upper <= 20.0 || dist_lower <= 20.0) {
            st.spread_mult *= 2.0;
        }

        // L2: 距涨停 <= 10 ticks → block 买单(避免吃到涨停)
        if (dist_upper <= 10.0) {
            st.allow_bid = false;
            WTSLogger::warn("[LIMIT] {} near UPPER ({} ticks), block bid", ctx.code, (int)dist_upper);
        }
        // L2: 距跌停 <= 10 ticks → block 卖单
        if (dist_lower <= 10.0) {
            st.allow_ask = false;
            WTSLogger::warn("[LIMIT] {} near LOWER ({} ticks), block ask", ctx.code, (int)dist_lower);
        }

        // L3: 锁板 → 双边暂停
        if (dist_upper <= 0.5 || dist_lower <= 0.5) {
            st.allow_bid = false;
            st.allow_ask = false;
            WTSLogger::error("[LIMIT] {} LOCKED at limit, PAUSE all quotes", ctx.code);
        }
    }
};

//==============================================================================
// 5. ColdStartPolicy — 冷启动保护: 信号源未热身时 maxSpreadMult 保守报价
//==============================================================================
class ColdStartPolicy : public IQuotePolicy
{
public:
    void apply(const QuotePolicyContext& ctx, QuoteState& st) override
    {
        if (!ctx.cold_start || !ctx.spread_opt)
            return;

        double max_mult = ctx.spread_opt->getParams().max_spread_mult;
        if (st.spread_mult < max_mult) {
            st.spread_mult = max_mult;
            double half_spread = ctx.tick_size * ctx.spread_opt->getParams().base_spread * max_mult / 2.0;
            st.l0_bid = ctx.mid - half_spread - st.skew * ctx.tick_size;
            st.l0_ask = ctx.mid + half_spread - st.skew * ctx.tick_size;
            st.l0_bid = std::floor(st.l0_bid / ctx.tick_size) * ctx.tick_size;
            st.l0_ask = std::ceil(st.l0_ask / ctx.tick_size) * ctx.tick_size;
        }
    }
};

//==============================================================================
// 6. FillRetreatPolicy — 成交后退机制 (Fill Retreat)
//    买单成交 → bid 不得高于 (成交价 - retreat_ticks)
//    卖单成交 → ask 不得低于 (成交价 + retreat_ticks)
//==============================================================================
class FillRetreatPolicy : public IQuotePolicy
{
public:
    void apply(const QuotePolicyContext& ctx, QuoteState& st) override
    {
        if (!ctx.calibrator)
            return;

        FillRetreat retreat = ctx.calibrator->getFillRetreat(ctx.code, ctx.timestamp);
        if (retreat.bid_retreat_active && st.l0_bid > retreat.bid_retreat_price) {
            st.l0_bid = std::floor(retreat.bid_retreat_price / ctx.tick_size) * ctx.tick_size;
        }
        if (retreat.ask_retreat_active && st.l0_ask < retreat.ask_retreat_price) {
            st.l0_ask = std::ceil(retreat.ask_retreat_price / ctx.tick_size) * ctx.tick_size;
        }
    }
};

//==============================================================================
// QuotePolicyChain — 固定顺序执行 (与旧 processQuoting 内联顺序一致)
//==============================================================================
class QuotePolicyChain
{
public:
    RiskWidenPolicy& riskWiden() { return _risk_widen; }
    ToxicityPolicy& toxicity() { return _toxicity; }

    /// GLFT 之后依次执行: RiskWiden → ArbCloseSync → Toxicity → LimitPrice
    /// → ColdStart → FillRetreat
    void run(const QuotePolicyContext& ctx, QuoteState& st)
    {
        _risk_widen.apply(ctx, st);
        _arb_sync.apply(ctx, st);
        _toxicity.apply(ctx, st);
        _limit_price.apply(ctx, st);
        _cold_start.apply(ctx, st);
        _fill_retreat.apply(ctx, st);
    }

private:
    RiskWidenPolicy _risk_widen;
    ArbCloseSyncPolicy _arb_sync;
    ToxicityPolicy _toxicity;
    LimitPricePolicy _limit_price;
    ColdStartPolicy _cold_start;
    FillRetreatPolicy _fill_retreat;
};

} // namespace futu
