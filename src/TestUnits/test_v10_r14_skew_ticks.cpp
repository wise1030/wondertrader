/**
 * @file test_v10_r14_skew_ticks.cpp
 * @brief V10-R14 skew tick 量化重构 回归锁定
 *
 * 背景 (09-07 夜盘实证, 详见 AGENTS.md R14 立案+修正①):
 *   下跌市 delta 0→30 一小时撞限, util=1.0 时总 skew 仅 -0.67t 未穿越 mid,
 *   7 分钟只减 1 手 —— sub-tick skew 是死区间 (盘口对偏移按 tick 离散感知)。
 *
 * 修复语义锁定:
 *   driver = w_p·sign(r_p)·|r_p|^γ + w_c·sign(r_c)·|r_c|^γ  (双驱动加权求和, 对冲腿符号相抵)
 *   k = clamp(round(k_max×driver), ±k_max) + 滞环(h)
 *   driver>0(多头) → skew<0(整梯下移)
 *   穿越授权: |r_p|≥1.0 或 |r_c|≥1.0 → 上限 k_max; 否则 floor(half_spread) (贴 mid 为止)
 */
#include "../WtFutuCore/SpreadOptimizer.h"
#include "gtest/gtest.h"

using namespace futu;

namespace
{

GLFTParams makeR14Params()
{
    GLFTParams p;
    p.base_spread = 2.0; // half_spread = 1.0 tick
    p.tick_size = 0.2;
    p.delta_skew_power = 1.0; // γ=1 线性
    p.skew_tick_max = 4.0;
    p.skew_hysteresis = 0.05;
    p.skew_cross_max_ticks = 3.0;
    p.portfolio_skew_weight = 0.5;
    p.contract_skew_weight = 1.0;
    p.portfolio_max_delta = 30.0;
    p.no_depth_spread_mult = 1.0;
    return p;
}

PortfolioContext makePCtx(double total_delta, double contract_util)
{
    PortfolioContext ctx;
    ctx.total_delta = total_delta;
    ctx.contract_max_delta = 30;
    ctx.contract_realized_delta_util = contract_util;
    ctx.contract_realized_delta_util_valid = true;
    return ctx;
}

} // namespace

// T1: 滞环量化档位映射 (raw ticks → k, 边界 ±0.5, 滞环 h=0.05)
TEST(V10R14SkewTicks, QuantizationMap)
{
    SpreadOptimizer opt("SHFE.ao2611");
    opt.setParams(makeR14Params());

    EXPECT_EQ(opt.quantizeSkewTicks(0.0), 0);
    EXPECT_EQ(opt.quantizeSkewTicks(0.49), 0);  // 目标 0, 不切换
    EXPECT_EQ(opt.quantizeSkewTicks(0.55), 1);  // 越过 0.5+0.05 → 换档
    EXPECT_EQ(opt.quantizeSkewTicks(1.4), 1);   // 目标 1, 停留
    EXPECT_EQ(opt.quantizeSkewTicks(1.6), 2);   // 越过 1.5+0.05 → 换档
    EXPECT_EQ(opt.quantizeSkewTicks(2.6), 3);   // 越过 2.5+0.05 → 换档
    EXPECT_EQ(opt.quantizeSkewTicks(3.6), 4);   // 越过 3.5+0.05 → 换档
    EXPECT_EQ(opt.quantizeSkewTicks(9.9), 4);   // clamp 到 k_max
    // 大幅负跳变: 多档直跳
    EXPECT_EQ(opt.quantizeSkewTicks(-2.6), -3); // 目标-3, 边界 4-0.5=3.5, -2.6<=3.45 → 直跳
    EXPECT_EQ(opt.quantizeSkewTicks(-9.9), -4); // clamp
}

// T2: 双驱动符号混合 —— 对冲腿相抵语义 (修正①核心)
TEST(V10R14SkewTicks, SignedBlendHedgeLegCancellation)
{
    SpreadOptimizer opt("SHFE.ao2611");
    opt.setParams(makeR14Params());

    // 顺势腿: 组合多 15 (r_p=0.5) + 合约多 0.5 → 叠加
    EXPECT_NEAR(opt.computeDeltaSkewDriver(15.0, 0.5), 0.5 * 0.5 + 1.0 * 0.5, 1e-9);
    // 对冲腿: 组合多 15 (r_p=0.5) + 合约空 -0.5 → 相抵 (B 腿仅获弱反向 skew, 不拆对冲)
    EXPECT_NEAR(opt.computeDeltaSkewDriver(15.0, -0.5), 0.5 * 0.5 - 1.0 * 0.5, 1e-9);
    // 组合关闭 (max_delta=0): 仅合约分量
    GLFTParams p = makeR14Params();
    p.portfolio_max_delta = 0;
    opt.setParams(p);
    EXPECT_NEAR(opt.computeDeltaSkewDriver(15.0, 0.5), 0.5, 1e-9);
}

// T3: 滞环防抖 —— 边界内抖动不换档
TEST(V10R14SkewTicks, HysteresisNoFlapping)
{
    SpreadOptimizer opt("SHFE.ao2611");
    opt.setParams(makeR14Params());

    opt.quantizeSkewTicks(0.55);           // → k=1 (当前档 1)
    EXPECT_EQ(opt.quantizeSkewTicks(1.49), 1); // 目标仍 1
    EXPECT_EQ(opt.quantizeSkewTicks(1.51), 1); // 目标 2, 但 1.51 < 1.5+0.05 → 滞环保持
    EXPECT_EQ(opt.quantizeSkewTicks(1.56), 2); // 越过 1.55 → 换档
    EXPECT_EQ(opt.quantizeSkewTicks(1.49), 2); // 目标 1, 1.49 > 1.5-0.05 → 滞环保持
    EXPECT_EQ(opt.quantizeSkewTicks(1.44), 1); // 越过 1.45 → 换档
}

// T4: 端到端 —— 撞限穿越 vs 非授权贴 mid
TEST(V10R14SkewTicks, EndToEndCrossVsCap)
{
    SpreadOptimizer opt("SHFE.ao2611");
    opt.setParams(makeR14Params());
    SignalContext sig;

    // 撞限: 组合 30/30 (r_p=1.0) + 合约 1.0 → driver=1.5 → raw=6t → k=4 (clamp)
    //       19.23 授权档 → cap=min(k_max, max(1, spread-1))=min(4, 2-1)=1 → skew=-1t
    //       减仓侧几何 = 贴 mid: ask=mid+1-1=mid (join-mid 减仓, 不再 -3t 穿越)
    auto ctx_limit = makePCtx(30.0, 1.0);
    auto res_limit = opt.computeOptimalQuote(100.0, 0.0, sig, 0.0, &ctx_limit);
    EXPECT_NEAR(res_limit.inventory_skew, -1.0, 1e-6);
    EXPECT_GE(res_limit.ask_price, 100.0 - 1e-9); // 贴 mid 不穿越

    // 对冲腿: 组合多 15 + 本合约空 -0.5 → driver=-0.25 → raw=-1.0t → k=+1 (抬报价, 鼓励建空头对冲)
    //         注意 quantize 滞环状态延续上一用例? 否 — 每 TEST 新实例
    SpreadOptimizer opt2("SHFE.ao2612");
    opt2.setParams(makeR14Params());
    auto ctx_hedge = makePCtx(15.0, -0.5);
    auto res_hedge = opt2.computeOptimalQuote(100.0, 0.0, sig, 0.0, &ctx_hedge);
    EXPECT_NEAR(res_hedge.inventory_skew, 1.0, 1e-6); // 正 skew (上移: bid 贴 touch 买平空头=降组合敞口? 见下注)
    // 注: 合约空 (r_c<0) 时合约分量驱动减仓侧=bid 向 touch; 组合多头分量驱动 ask 向 touch;
    //     相抵后净 driver=-0.25 → skew=+1 (上移): ask 拉远 (不加空) + bid 贴 touch (买平空),
    //     同时组合分量在该腿体现为抑制其空头增长 —— 相抵语义正确

    // 非授权中档: 组合 15/30 + 合约 0.5 → driver=0.75 → raw=3.0t → k=3, 未授权 → cap=floor(half)=1
    SpreadOptimizer opt3("SHFE.ao2611");
    opt3.setParams(makeR14Params());
    auto ctx_mid = makePCtx(15.0, 0.5);
    auto res_mid = opt3.computeOptimalQuote(100.0, 0.0, sig, 0.0, &ctx_mid);
    EXPECT_NEAR(res_mid.inventory_skew, -1.0, 1e-6); // 贴 mid (ask=mid), bid=mid-2t 防堆积
    EXPECT_GT(res_mid.ask_price, 100.0 - 1e-9);      // ask 未穿越 mid (>= mid)
}

// T5: 无库存零偏移 (消灭 sub-tick 死区间)
TEST(V10R14SkewTicks, ZeroInventoryZeroSkew)
{
    SpreadOptimizer opt("SHFE.ao2611");
    opt.setParams(makeR14Params());
    SignalContext sig;

    auto ctx = makePCtx(3.0, 0.1); // r_p=0.1, r_c=0.1 → driver=0.05+0.1=0.15 → raw=0.6t → k=1
    auto res = opt.computeOptimalQuote(100.0, 0.0, sig, 0.0, &ctx);
    EXPECT_NEAR(res.inventory_skew, -1.0, 1e-6); // driver 虽小, 量化后已是一整档 (无 sub-tick)

    auto ctx0 = makePCtx(0.0, 0.0);
    SpreadOptimizer opt0("SHFE.ao2611");
    opt0.setParams(makeR14Params());
    auto res0 = opt0.computeOptimalQuote(100.0, 0.0, sig, 0.0, &ctx0);
    EXPECT_NEAR(res0.inventory_skew, 0.0, 1e-9);
}
