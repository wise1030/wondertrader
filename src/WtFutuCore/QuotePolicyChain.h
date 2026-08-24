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
 * A4(2026-08-24②) 边界声明:
 *   - 本链混合了【风控响应】(RiskWiden/Toxicity/LimitPrice) 与【业务调整】
 *     (ArbCloseSync/ColdStart/FillRetreat), 边界由 chain.run 的固定顺序维护,
 *     非类型系统强制 —— 新增策略对象必须评估对既有顺序依赖的影响。
 *   - 已知同 tick 覆盖顺序依赖: RiskWiden.tickSoft 无条件重算会覆盖本 tick
 *     更早写入的 hard 闩锁; 因 hard 闩锁(onHardWiden, 由 RiskCoordinator 在
 *     tickSoft 之后调用)总是后写, 旧语义得以保留 —— run() 不得重排前两阶段。
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
#include <unordered_set>
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
    double last_price = 0.0; ///< 最新成交价 (L0 触板判定, 0=无成交)
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

    void apply(const QuotePolicyContext& ctx, QuoteState& st) override
    {
        double mult = _mult.load(std::memory_order_acquire);
        st.spread_mult *= mult;

        // v7.8 链路修复: FutuQuoter::computeQuotePrices 不消费 spread_mult
        // (见 FutuQuoter.cpp "(void)spread_mult" — spread 由上游 l0_bid/l0_ask 承载),
        // 仅写 st.spread_mult 是死写, WIDEN_SPREAD 从未真正生效.
        // 在此绕 l0 中心对称拉宽: 中心已含 fair_value(alpha)+skew 偏移,
        // 拉宽只扩大宽度, 不改变 skew 方向语义.
        if (mult > 1.0 && ctx.tick_size > 0 && st.l0_ask > st.l0_bid) {
            double center = (st.l0_bid + st.l0_ask) * 0.5;
            double half_width = (st.l0_ask - st.l0_bid) * 0.5 * mult;
            st.l0_bid = std::floor((center - half_width) / ctx.tick_size) * ctx.tick_size;
            st.l0_ask = std::ceil((center + half_width) / ctx.tick_size) * ctx.tick_size;
        }
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
        if (arb_close_dir == SpreadArbitrageManager::kArbCloseConflict) {
            // V8-A13: 1:N 场景多个活跃 intent 方向冲突 — 单侧抑制依据不可靠, 双侧抑制
            st.allow_bid = false;
            st.allow_ask = false;
            WTSLogger::debug("[ARB-SYNC] {} conflicting arb close intents (1:N), suppress MM both sides", ctx.code);
        } else if (arb_close_dir > 0) {
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

            // V8-T5 方向语义统一 (与 T1 同车): toxic_side==1 为激进买流
            // (ofi>0 且 trade imbalance>0, 见 PredictiveToxicity.cpp) --
            // 知情买方吃的是我方 ask (逆向选择在 ask 侧), 故抑制 ask;
            // ==-1 激进卖流 -> 抑制 bid。原映射 (1->停bid) 方向反了。
            if (tox.toxic_side == 1) {
                st.allow_ask = false;
                WTSLogger::warn(
                    "[TOXIC] {} Aggressive buy flow (score={:.2f}), pausing ask quotes", ctx.code, tox.toxic_score);
            } else if (tox.toxic_side == -1) {
                st.allow_bid = false;
                WTSLogger::warn(
                    "[TOXIC] {} Aggressive sell flow (score={:.2f}), pausing bid quotes", ctx.code, tox.toxic_score);
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
//    L0: 交易价触板 → 全停 (义务报价豁免, 含 force_obligation)
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

        // L0: 交易价触板 → 全停 (交易所义务豁免口径: 最新价触板即豁免, 不等锁板)
        //     守卫 last_price>0: 盘前/无成交时段 last=0, 否则恒触发下板误判
        //     精确比较: 成交价不可能越过板价, 触板即 last==板价 (浮点精确表示)
        //     日志按进入/离开触板状态跳变打, 避免贴板期间每 tick 日志洪水
        if (ctx.last_price > 0 && std::isfinite(ctx.last_price)) {
            if (ctx.last_price >= ctx.upper_limit) {
                st.allow_bid = false;
                st.allow_ask = false;
                if (_touch_active.insert(ctx.code).second)
                    WTSLogger::error("[LIMIT-TOUCH] {} last={} touches UPPER {}, STOP all quotes",
                                     ctx.code, ctx.last_price, ctx.upper_limit);
                return;
            }
            if (ctx.last_price <= ctx.lower_limit) {
                st.allow_bid = false;
                st.allow_ask = false;
                if (_touch_active.insert(ctx.code).second)
                    WTSLogger::error("[LIMIT-TOUCH] {} last={} touches LOWER {}, STOP all quotes",
                                     ctx.code, ctx.last_price, ctx.lower_limit);
                return;
            }
        }
        if (_touch_active.erase(ctx.code) > 0)
            WTSLogger::info("[LIMIT-TOUCH] {} last={} off limit, resume quoting", ctx.code, ctx.last_price);

        double dist_upper = (ctx.upper_limit - ctx.mid) / ctx.tick_size;
        double dist_lower = (ctx.mid - ctx.lower_limit) / ctx.tick_size;

        // L1: 距涨跌停 <= 20 ticks, 加宽 spread
        if (dist_upper <= 20.0 || dist_lower <= 20.0) {
            st.spread_mult *= 2.0;

            // v7.8 链路修复 (与 RiskWidenPolicy 同类): spread_mult 死写 -> l0 实际拉宽
            // 涨跌停附近流动性风险高, 绕中心拉宽 2x 降低挂单吸引力
            if (ctx.tick_size > 0 && st.l0_ask > st.l0_bid) {
                double center = (st.l0_bid + st.l0_ask) * 0.5;
                double half_width = (st.l0_ask - st.l0_bid); // * 0.5 * 2.0
                st.l0_bid = std::floor((center - half_width) / ctx.tick_size) * ctx.tick_size;
                st.l0_ask = std::ceil((center + half_width) / ctx.tick_size) * ctx.tick_size;
            }
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

private:
    std::unordered_set<std::string> _touch_active; ///< L0 触板状态 (日志跳变节流)
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
            // v7.8 skew 符号修复: SpreadOptimizer 约定 bid/ask = fair ∓ half_spread + skew_price
            // (实盘日志验证: mid=2710.50 skew=1.32 half=0.5 -> bid=2711.00, 与 +skew 一致)
            // 原 "- st.skew" 使冷启动窗口内 skew 方向反转: 多头持仓 bid 反而上移追买
            st.l0_bid = ctx.mid - half_spread + st.skew * ctx.tick_size;
            st.l0_ask = ctx.mid + half_spread + st.skew * ctx.tick_size;
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
