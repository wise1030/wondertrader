/*!
 * \file test_inventory_delta_separation.cpp
 * \brief delta/position 语义边界 (2026-08-19 原则) unit tests
 *
 * 原则: 策略库存调控 (skew/qty衰减/义务/穿越/block_add) 只用 delta 口径,
 *       归一化分母 = contract_max_delta; maxPosition 仅用于风控硬闸门.
 *
 * 覆盖:
 *   - computeInventoryStrategyInputs 的 delta 口径 (hedge_ratio≠1 时 position≠delta)
 *   - delta_util≥1.0 触发 force 义务 + block_add; <1.0 不触发
 *   - contract_max_delta=0 时策略输入全零, 风控闸门不受影响
 *   - maxPosition 硬停 (halt_quoting) 与策略软限相互独立
 *   - SpreadOptimizer 单路径 skew: contractDelta 参数不再驱动 skew,
 *     仅 pCtx->contract_realized_delta_util 生效 (C2: 已实现口径, 不含 pending 投影);
 *     util≥1.0 授权穿越 half_spread
 */
#include "../WtFutuCore/FutuRiskMonitor.h"
#include "../WtFutuCore/FutuPortfolio.h"
#include "../WtFutuCore/SpreadOptimizer.h"
#include "gtest/gtest/gtest.h"

using namespace futu;

namespace
{

ContractState makeCs(double position, double hedge_ratio, double max_position, double max_delta)
{
    ContractState cs;
    cs.code = "SHFE.ag2610";
    cs.position = position;
    cs.hedge_ratio = hedge_ratio;
    cs.max_position = max_position;
    cs.contract_max_delta = max_delta;
    return cs;
}

} // namespace

// delta 口径: hedge_ratio=2 时 util 按 delta(=position×hr) 而非 position 归一化
TEST(InventoryDeltaSeparation, StrategyInputsUseDeltaNotPosition)
{
    FutuRiskMonitor rm;
    // position=10, hr=2 → delta=20; maxDelta=30 → util=20/30≈0.667
    // (若误用 position 口径且分母 maxPosition=50, util 会是 10/50=0.2)
    auto cs = makeCs(10, 2.0, 50, 30);
    auto d = rm.checkPreTradePosition(cs, nullptr, 0);
    EXPECT_NEAR(d.strategy.long_delta_util, 20.0 / 30.0, 1e-9);
    EXPECT_NEAR(d.strategy.short_delta_util, 0.0, 1e-9);
    EXPECT_FALSE(d.strategy.force_ask_obligation);
    EXPECT_FALSE(d.risk.halt_quoting);
}

// delta_util≥1.0 → force 减仓侧义务 + block_add(默认 ratio=1.0)
TEST(InventoryDeltaSeparation, DeltaUtilFullTriggersObligationAndBlockAdd)
{
    FutuRiskMonitor rm;
    // position=15, hr=2 → delta=30; maxDelta=30 → util=1.0
    auto cs = makeCs(15, 2.0, 50, 30);
    auto d = rm.checkPreTradePosition(cs, nullptr, 0);
    EXPECT_NEAR(d.strategy.long_delta_util, 1.0, 1e-9);
    EXPECT_TRUE(d.strategy.force_ask_obligation); // 多 delta 打满 → ask 侧义务减仓
    EXPECT_FALSE(d.strategy.force_bid_obligation);
    EXPECT_TRUE(d.strategy.block_add_long);
    EXPECT_FALSE(d.strategy.block_add_short);
    EXPECT_FALSE(d.risk.halt_quoting); // 15 < maxPosition=50, 风控不触发
}

// contract_max_delta=0 → 策略输入全零; 风控硬闸门 (maxPosition) 不受影响
TEST(InventoryDeltaSeparation, ZeroMaxDeltaDisablesStrategyButNotRisk)
{
    FutuRiskMonitor rm;
    auto cs = makeCs(11, 1.0, 10, 0); // |pos|=11 > maxPosition=10
    auto d = rm.checkPreTradePosition(cs, nullptr, 0);
    EXPECT_NEAR(d.strategy.long_delta_util, 0.0, 1e-9);
    EXPECT_FALSE(d.strategy.force_ask_obligation);
    EXPECT_FALSE(d.strategy.block_add_long);
    EXPECT_TRUE(d.risk.halt_quoting); // 风控硬停独立生效
}

// maxPosition=0 (未配硬顶) → 无风控闸门, 但策略输入仍按 maxDelta 正常计算
TEST(InventoryDeltaSeparation, ZeroMaxPositionKeepsStrategyInputs)
{
    FutuRiskMonitor rm;
    auto cs = makeCs(20, 1.0, 0, 30); // delta=20/30≈0.667
    auto d = rm.checkPreTradePosition(cs, nullptr, 0);
    EXPECT_NEAR(d.strategy.long_delta_util, 20.0 / 30.0, 1e-9);
    EXPECT_FALSE(d.risk.halt_quoting);
}

// 空头对称: delta<0 → short_delta_util 生效, force_bid_obligation
TEST(InventoryDeltaSeparation, ShortSideSymmetric)
{
    FutuRiskMonitor rm;
    auto cs = makeCs(-16, 1.0, 50, 30); // delta=-16 → util≈0.533
    auto d = rm.checkPreTradePosition(cs, nullptr, 0);
    EXPECT_NEAR(d.strategy.long_delta_util, 0.0, 1e-9);
    EXPECT_NEAR(d.strategy.short_delta_util, 16.0 / 30.0, 1e-9);

    auto cs2 = makeCs(-30, 1.0, 50, 30); // util=1.0
    auto d2 = rm.checkPreTradePosition(cs2, nullptr, 0);
    EXPECT_TRUE(d2.strategy.force_bid_obligation);
    EXPECT_TRUE(d2.strategy.block_add_short);
}

namespace
{

GLFTParams makeSkewParams()
{
    GLFTParams p;
    p.base_spread = 2.0; // half_spread = 1.0 tick
    p.tick_size = 0.2;
    p.delta_skew_power = 1.5;
    p.inventory_skew_gain = 1.0;
    p.skew_cross_max_ticks = 3.0;
    p.portfolio_skew_weight = 0.0; // 纯 contract 维度
    p.contract_skew_weight = 1.0;
    p.portfolio_max_delta = 0; // 关闭组合 skew
    p.no_depth_spread_mult = 1.0; // 测试 ctx 无深度数据;  neutralize 深度放大, 隔离 skew 变量
    return p;
}

PortfolioContext makePCtx(double signed_util)
{
    PortfolioContext ctx;
    ctx.contract_max_delta = 30;
    // C2: skew/穿越消费 realized 口径; projected 同值注入 (本测试无 pending 差异场景)
    ctx.contract_realized_delta_util = signed_util;
    ctx.contract_realized_delta_util_valid = true;
    ctx.contract_delta_util = signed_util;
    ctx.contract_delta_util_valid = true;
    return ctx;
}

} // namespace

// 单路径 skew: util 驱动 inventory_skew, 方向 = 减仓侧贴 mid
TEST(InventoryDeltaSeparation, SkewDrivenByDeltaUtil)
{
    SpreadOptimizer opt("SHFE.ag2610");
    opt.setParams(makeSkewParams());
    SignalContext sig; // 全默认: alpha 无效, 无毒性 → fair_value=mid, spread_mult=1

    auto ctx = makePCtx(0.5); // 多头 50% → skew 应为负 (压 ask 向 mid)
    auto res = opt.computeOptimalQuote(100.0, 0.0, sig, 0.0, &ctx);
    double expect = -std::pow(0.5, 1.5); // -0.3536 ticks
    EXPECT_NEAR(res.inventory_skew, expect, 1e-6);
    EXPECT_LT(res.inventory_skew, 0.0); // 多头 → 负 skew (减仓侧=ask 压向 mid)
    // 注: 不断言 ask_price — tick 取整会掩盖亚 tick 级 skew, inventory_skew 是取整前真值
}

// legacy 双路径已删除: contractDelta 参数不再驱动 skew (无注入时 contract_skew=0)
TEST(InventoryDeltaSeparation, ContractDeltaParamNoLongerDrivesSkew)
{
    SpreadOptimizer opt("SHFE.ag2610");
    opt.setParams(makeSkewParams());
    SignalContext sig;

    PortfolioContext ctx; // 未注入 delta_util (valid=false)
    ctx.contract_max_delta = 30;
    auto res = opt.computeOptimalQuote(100.0, 25.0 /*大 delta*/, sig, 0.0, &ctx);
    EXPECT_NEAR(res.inventory_skew, 0.0, 1e-9);
}

// util≥1.0 授权穿越: inventory_skew 可超 half_spread(=1.0 tick);
// util<1.0 时截断在 half_spread 内
TEST(InventoryDeltaSeparation, CrossAuthorizationBeyondHalfSpread)
{
    SpreadOptimizer opt("SHFE.ag2610");
    opt.setParams(makeSkewParams());
    SignalContext sig;

    auto below = makePCtx(0.9);
    auto res_below = opt.computeOptimalQuote(100.0, 0.0, sig, 0.0, &below);
    double expect_below = -std::pow(0.9, 1.5); // ≈-0.8538, 未穿越
    EXPECT_NEAR(res_below.inventory_skew, expect_below, 1e-6);
    EXPECT_GT(res_below.inventory_skew, -1.0);

    auto above = makePCtx(1.2);
    auto res_above = opt.computeOptimalQuote(100.0, 0.0, sig, 0.0, &above);
    double expect_above = -std::pow(1.2, 1.5); // ≈-1.3145, 穿越 half_spread
    EXPECT_NEAR(res_above.inventory_skew, expect_above, 1e-6);
    EXPECT_LT(res_above.inventory_skew, -1.0);
}
