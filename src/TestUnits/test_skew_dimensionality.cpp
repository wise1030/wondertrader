/*!
 * \file test_skew_dimensionality.cpp
 * \brief 复核修复包 C 批 (2026-08-24②) unit tests
 *
 * C1: 组合 delta skew 量纲统一到 ticks (×half_spread) —— 此前返回无纲量被隐式当 ticks,
 *     与 contract 分量(ticks)加权相加时相对力度随价差宽度漂移。
 * C2: skew/穿越授权切换已实现口径 (contract_realized_delta_util, 不含 pending 投影);
 *     force_obligation 保持 projected 口径 (computeInventoryStrategyInputs, 既有测试覆盖)。
 */
#include "../WtFutuCore/SpreadOptimizer.h"
#include "gtest/gtest/gtest.h"

#include <cmath>

using namespace futu;

namespace
{

GLFTParams makeParams()
{
    GLFTParams p;
    p.base_spread = 2.0; // half_spread = 1.0 tick (mult=1 时)
    p.tick_size = 0.2;
    p.delta_skew_power = 1.5;
    p.inventory_skew_gain = 1.0;
    p.skew_cross_max_ticks = 3.0;
    p.delta_skew_threshold = 0.3;
    p.delta_skew_factor = 1.5;
    p.portfolio_max_delta = 30.0;
    p.no_depth_spread_mult = 1.0; // 隔离深度变量
    return p;
}

} // namespace

//==========================================================================
// C1: 组合 skew 量纲统一
//==========================================================================

// 组合 skew 随 half_spread 线性伸缩 (同 util 同输入, 权重置 1 隔离加权)
TEST(SkewDimensionality, PortfolioSkewScalesWithHalfSpread)
{
    SpreadOptimizer opt("T.C1");
    auto p = makeParams();
    p.portfolio_skew_weight = 1.0; // 纯组合维度, 权重隔离
    p.delta_skew_factor = 0.5;     // 缩小强度避开 ±half_spread 截断, 纯验证线性关系
    opt.setParams(p);

    SignalContext sig;
    PortfolioContext ctx; // realized/projected 均无效 → contract 分量=0
    ctx.total_delta = 45.0; // util = 45/30 = 1.5 → excess=1.2

    auto r1 = opt.computeOptimalQuote(100.0, 0.0, sig, 0.0, &ctx);
    // half=1.0 tick (base=2, mult=1): C1 后组合分量 = factor × excess^p × half_spread
    EXPECT_NEAR(r1.inventory_skew, -0.5 * std::pow(1.2, 1.5) * 1.0, 1e-9);
}

// 权重语义稳定: 两维同量纲后, 加权结果 = 0.5×port + 1.0×contract (ticks), 且不触 ±half 截断
TEST(SkewDimensionality, WeightedSumStableSemantics)
{
    SpreadOptimizer opt("T.C1b");
    auto p = makeParams();
    p.portfolio_skew_weight = 0.5;
    p.contract_skew_weight = 1.0;
    opt.setParams(p);

    SignalContext sig;
    PortfolioContext ctx;
    ctx.total_delta = 30.0;                                        // 组合 util=1.0, excess=0.7
    ctx.contract_realized_delta_util = 0.5;                        // 合约 util=0.5
    ctx.contract_realized_delta_util_valid = true;

    auto r = opt.computeOptimalQuote(100.0, 0.0, sig, 0.0, &ctx);
    const double port = -1.5 * std::pow(0.7, 1.5) * 1.0;           // -0.8786 ticks (half=1)
    const double contract = -std::pow(0.5, 1.5) * 1.0;             // -0.3536 ticks
    // 加权和 -0.7929 在 ±half_spread(±1.0) 内 → 不触截断, 权重语义可精确断言
    EXPECT_NEAR(r.inventory_skew, 0.5 * port + 1.0 * contract, 1e-9);
}

// 组合阈值死区不变: util ≤ threshold → 组合分量为 0
TEST(SkewDimensionality, PortfolioDeadZoneUnchanged)
{
    SpreadOptimizer opt("T.C1c");
    auto p = makeParams();
    opt.setParams(p);

    SignalContext sig;
    PortfolioContext ctx;
    ctx.total_delta = 9.0; // util = 0.3 == threshold → 0
    auto r = opt.computeOptimalQuote(100.0, 0.0, sig, 0.0, &ctx);
    EXPECT_NEAR(r.inventory_skew, 0.0, 1e-12);
}

//==========================================================================
// C2: skew/穿越授权用已实现口径
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
    // skew 按 realized=0.8: -0.8^1.5 ≈ -0.7155, 且未穿越 (|skew| < half=1.0)
    EXPECT_NEAR(r.inventory_skew, -std::pow(0.8, 1.5), 1e-9);
    EXPECT_GT(r.inventory_skew, -1.0);
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
    double expect = -std::pow(1.3, 1.5);              // ≈-1.4822 > half(1.0) → 穿越
    EXPECT_NEAR(r.inventory_skew, expect, 1e-9);
    EXPECT_LT(r.inventory_skew, -1.0);
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
