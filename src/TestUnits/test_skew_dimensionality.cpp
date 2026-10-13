/*!
 * \file test_skew_dimensionality.cpp
 * \brief 复核修复包 C 批 (2026-08-24②) + V10-R14 (2026-09-07) 改写 unit tests
 *
 * C2(保留语义): skew/穿越授权切换已实现口径 (contract_realized_delta_util, 不含 pending 投影)。
 * R14(2026-09-07): skew 幅度映射改 tick 量化档位 (driver 无纲 → k=round(k_max×driver)+滞环),
 *   C1 的"量纲随 half_spread 漂移"问题由构造消除 (skew 恒为整 tick 数);
 *   旧 delta_skew_threshold/factor/inventory_skew_gain 参数已删除。
 */
#include "../WtFutuCore/SpreadOptimizer.h"
#include "gtest/gtest.h"

#include <cmath>

using namespace futu;

namespace
{

GLFTParams makeParams()
{
    GLFTParams p;
    p.base_spread = 2.0; // half_spread = 1.0 tick (mult=1 时)
    p.tick_size = 0.2;
    p.delta_skew_power = 1.0;    // γ=1 线性
    p.skew_tick_max = 4.0;
    p.skew_hysteresis = 0.05;
    p.skew_cross_max_ticks = 3.0;
    p.portfolio_max_delta = 30.0;
    p.no_depth_spread_mult = 1.0; // 隔离深度变量
    return p;
}

} // namespace

//==========================================================================
// R14: 组合/合约双驱动量化 (替代 C1 量纲测试 —— skew 恒为整 tick, 量纲问题由构造消除)
//==========================================================================

// 组合分量独立驱动: 纯组合维度 (w_c=0), r_p=1.0 → driver=0.5 → raw=2.0t → k=2,
// 授权穿越 (r_p≥1.0) → skew=-2t; 幅度不再随 half_spread 漂移
TEST(SkewDimensionality, PortfolioComponentQuantized)
{
    SpreadOptimizer opt("T.R14a");
    auto p = makeParams();
    p.portfolio_skew_weight = 1.0; // 纯组合维度, 权重隔离
    p.contract_skew_weight = 0.0;
    opt.setParams(p);

    SignalContext sig;
    PortfolioContext ctx; // realized 未注入 → contract 分量=0
    ctx.total_delta = 30.0; // r_p = 1.0

    auto r = opt.computeOptimalQuote(100.0, 0.0, sig, 0.0, &ctx);
    // driver = 1.0×1.0 = 1.0 → raw=4t → k=4 (clamp k_max);
    // 19.23 授权档 cap=min(k_max, max(1, spread-1))=min(4,1)=1 → 贴中 -1t
    EXPECT_NEAR(r.inventory_skew, -1.0, 1e-9);
}

// 权重语义稳定: driver = w_p·r_p + w_c·r_c (γ=1), 纯函数直接断言
TEST(SkewDimensionality, WeightedSumStableSemantics)
{
    SpreadOptimizer opt("T.R14b");
    auto p = makeParams();
    p.portfolio_skew_weight = 0.5;
    p.contract_skew_weight = 1.0;
    opt.setParams(p);

    // 对齐: 0.5×0.5 + 1.0×0.5 = 0.75
    EXPECT_NEAR(opt.computeDeltaSkewDriver(15.0, 0.5), 0.75, 1e-9);
    // 权重变化 → driver 变化 (语义稳定可断言)
    auto p2 = makeParams();
    p2.portfolio_skew_weight = 1.0;
    p2.contract_skew_weight = 0.0;
    opt.setParams(p2);
    EXPECT_NEAR(opt.computeDeltaSkewDriver(15.0, 0.5), 0.5, 1e-9);
}

// 死区由量化取整承担: |k_max×driver| < 0.5 → k=0 (无 sub-tick 死偏移)
TEST(SkewDimensionality, DeadZoneFromQuantization)
{
    SpreadOptimizer opt("T.R14c");
    auto p = makeParams();
    opt.setParams(p);

    SignalContext sig;
    PortfolioContext ctx;
    ctx.total_delta = 1.5; // r_p=0.05 → driver=0.5×0.05=0.025 → raw=0.1t → k=0
    auto r = opt.computeOptimalQuote(100.0, 0.0, sig, 0.0, &ctx);
    EXPECT_NEAR(r.inventory_skew, 0.0, 1e-12);
}

//==========================================================================
// C2(保留): skew/穿越授权用已实现口径
//==========================================================================

// pending 高位时 (projected≥1.0 而 realized<1.0): skew 按 realized, 穿越不授权
TEST(SkewDimensionality, RealizedDrivesSkewNotProjection)
{
    SpreadOptimizer opt("T.C2");
    auto p = makeParams();
    p.portfolio_skew_weight = 0.0; // 纯 contract 维度
    p.contract_skew_weight = 1.0;
    opt.setParams(p);

    SignalContext sig;
    PortfolioContext ctx;
    ctx.contract_max_delta = 30.0;
    ctx.contract_delta_util = 1.5;                    // projected (含 pending): 高
    ctx.contract_delta_util_valid = true;
    ctx.contract_realized_delta_util = 0.8;           // realized: 未打满
    ctx.contract_realized_delta_util_valid = true;

    auto r = opt.computeOptimalQuote(100.0, 0.0, sig, 0.0, &ctx);
    // R14: driver=0.8 → raw=3.2t → k=3, realized 未打满 → 未授权 → cap=floor(half)=1 → -1.0t
    EXPECT_NEAR(r.inventory_skew, -1.0, 1e-9);
    EXPECT_GT(r.inventory_skew, -1.0 - 1e-9);
}

// realized ≥ 1.0 授权穿越 (projected 无论高低)
TEST(SkewDimensionality, CrossAuthorizedByRealizedOnly)
{
    SpreadOptimizer opt("T.C2b");
    auto p = makeParams();
    p.portfolio_skew_weight = 0.0;
    p.contract_skew_weight = 1.0;
    opt.setParams(p);

    SignalContext sig;
    PortfolioContext ctx;
    ctx.contract_max_delta = 30.0;
    ctx.contract_delta_util = 0.2;                    // projected 低
    ctx.contract_delta_util_valid = true;
    ctx.contract_realized_delta_util = 1.3;           // realized 打满
    ctx.contract_realized_delta_util_valid = true;

    auto r = opt.computeOptimalQuote(100.0, 0.0, sig, 0.0, &ctx);
    // R14: driver=1.3 → raw=5.2t → k=4 (clamp); 19.23 授权档 cap=1 → 贴中 -1t
    EXPECT_NEAR(r.inventory_skew, -1.0, 1e-9);
    EXPECT_LE(r.inventory_skew, -1.0 + 1e-9);
}

// realized 未注入 (valid=false) → contract 分量 0 (与既有 ContractDeltaParamNoLongerDrivesSkew 呼应)
TEST(SkewDimensionality, MissingRealizedInputDisablesContractSkew)
{
    SpreadOptimizer opt("T.C2c");
    auto p = makeParams();
    p.portfolio_skew_weight = 0.0;
    p.contract_skew_weight = 1.0;
    opt.setParams(p);

    SignalContext sig;
    PortfolioContext ctx;
    ctx.contract_max_delta = 30.0;
    ctx.contract_delta_util = 1.0;                    // projected 有效但非消费口径
    ctx.contract_delta_util_valid = true;
    // realized 未注入

    auto r = opt.computeOptimalQuote(100.0, 0.0, sig, 0.0, &ctx);
    EXPECT_NEAR(r.inventory_skew, 0.0, 1e-12);
}
