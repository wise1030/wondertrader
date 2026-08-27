/*!
 * \file test_v10_r7d.cpp
 * \brief V10 诊断报告 R7-d 信号卫生包回归测试
 *
 * 覆盖:
 *   - P2-4: TickTransactionInferer 跨间隙/跨日守卫 (陈旧档位差分 → 伪"大单")
 *   - P2-4: OFISignalSource 跨间隙守卫 (间隙帧丢帧 + prev-state 重置后自愈)
 *   - P2-5: TradeFlow fallback 通道统计显著性门 (n>3 才 valid)
 *
 * 说明: Momentum/Volatility 守卫与 OFI 同模式 (共享 kSignalGapGuardMs);
 * coordinator.resetSession 信号栈接线无独立接缝, 由编译+v5 冒烟覆盖。
 */
#include "../WtFutuCore/TickTransactionInferer.h"
#include "../WtFutuCore/signals/MarketDataContext.h"
#include "../WtFutuCore/signals/OFISignalSource.h"
#include "../WtFutuCore/signals/TradeFlowSignalSource.h"
#include "../Includes/WTSDataDef.hpp"
#include "gtest/gtest.h"

using namespace futu;

namespace
{

wtp::WTSTickData* makeTick(double bid, double ask, double bid_qty, double ask_qty, uint32_t action_time)
{
    auto* t = wtp::WTSTickData::create("SHFE.ec.ec2509");
    auto& s = t->getTickStruct();
    s.bid_prices[0] = bid;
    s.bid_qty[0] = bid_qty;
    s.ask_prices[0] = ask;
    s.ask_qty[0] = ask_qty;
    s.price = (bid + ask) / 2.0;
    s.action_time = action_time;
    s.action_date = 20260827;
    s.total_volume = 0;
    return t;
}

} // namespace

// === V10-P2-4: TickInferer 跨间隙/跨日守卫 ===

TEST(test_v10_r7d, TickInfererGapFrameRejectedThenRecovers)
{
    TickTransactionInferer inf;

    // 首帧: 初始化 prev-state
    auto r0 = inf.inferFromTick(100.0, 100.2, 5, 5, 100.1, 0, 100000);
    EXPECT_EQ(r0.method, InferenceMethod::UNKNOWN);

    // 正常相邻帧 (500ms): 撤买量 5→0 → 推断出卖向成交
    auto r1 = inf.inferFromTick(100.0, 100.2, 0, 5, 100.1, 0, 100500);
    EXPECT_GT(r1.volume, 0.0);
    EXPECT_GT(r1.confidence, 0.0);

    // 跨间隙帧 (+10s): 必须拒收 (UNKNOWN/confidence 0), 不得用 10s 前的档位差分
    auto r2 = inf.inferFromTick(100.0, 100.2, 8, 5, 100.1, 0, 110500);
    EXPECT_EQ(r2.method, InferenceMethod::UNKNOWN);
    EXPECT_DOUBLE_EQ(r2.confidence, 0.0);

    // 间隙后首帧: prev 已重置为本帧 → 下一帧恢复差分能力
    auto r3 = inf.inferFromTick(100.0, 100.2, 8, 5, 100.1, 0, 111000);
    (void)r3; // 仅要求不崩溃且状态已重置
    auto r4 = inf.inferFromTick(100.0, 100.2, 2, 5, 100.1, 0, 111500);
    EXPECT_GT(r4.volume, 0.0) << "间隙重置后差分应恢复 (8→2)";
}

TEST(test_v10_r7d, TickInfererDayRolloverRejected)
{
    TickTransactionInferer inf;
    (void)inf.inferFromTick(100.0, 100.2, 5, 5, 100.1, 0, 235959000);
    // 日内 HHMMSSmmm 域回绕 (次日 00:00 后 ts < prev): 必须按间隙拒收
    auto r = inf.inferFromTick(100.0, 100.2, 0, 5, 100.1, 0, 1000);
    EXPECT_EQ(r.method, InferenceMethod::UNKNOWN);
    EXPECT_DOUBLE_EQ(r.confidence, 0.0);
}

// === V10-P2-4: OFI 跨间隙守卫 ===

TEST(test_v10_r7d, OFIGapFrameDropsAndPrevStateResets)
{
    MarketDataContext book;
    book.setContract("SHFE.ec.ec2509", 0.5);

    OFISignalSource ofi;

    // 帧1 (T0): 初始化 prev, 不产 OFI
    {
        auto* t = makeTick(100.0, 100.4, 5, 5, 90000000);
        book.onTick(t);
        t->release();
    }
    ofi.update(book);
    EXPECT_FALSE(ofi.result().valid);

    // 帧2 (T0+10s, 间隙): 丢帧, valid=false
    {
        auto* t = makeTick(100.0, 100.4, 50, 5, 90010000);
        book.onTick(t);
        t->release();
    }
    ofi.update(book);
    EXPECT_FALSE(ofi.result().valid) << "跨间隙帧的档位差分必须丢弃 (伪流)";

    // 帧3 (T0+10.5s): prev 已在间隙帧重置为该帧 → 本帧为重新武装帧, 仍不产 OFI
    {
        auto* t = makeTick(100.0, 100.4, 50, 5, 90010500);
        book.onTick(t);
        t->release();
    }
    ofi.update(book);
    EXPECT_FALSE(ofi.result().valid) << "间隙后首帧为重新武装帧 (prev 已对齐本帧)";

    // 帧4 (T0+11s): 恢复正常差分 —— ask 量撤离 → 正 OFI
    {
        auto* t = makeTick(100.0, 100.4, 50, 2, 90011000);
        book.onTick(t);
        t->release();
    }
    ofi.update(book);
    EXPECT_TRUE(ofi.result().valid);
    EXPECT_GT(ofi.getOFI(), 0.0) << "ask 量撤离 → 正 OFI (差分自愈)";
}

// === V10-P2-5: TradeFlow fallback 显著性门 ===

TEST(test_v10_r7d, TradeFlowFallbackInvalidWithoutSamples)
{
    MarketDataContext book; // 无任何 tick/成交
    book.setContract("SHFE.ec.ec2509", 0.5);

    TradeFlowSignalSource tf;
    tf.update(book);
    // 零样本: TradeFlowAnalysis 默认 buy=sell_pressure=0.5, 旧实现满置信 valid=true
    EXPECT_FALSE(tf.result().valid) << "零成交不得以默认 0.5 压力满置信参与加权";
}
