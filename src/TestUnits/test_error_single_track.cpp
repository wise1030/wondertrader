/*!
 * \file test_error_single_track.cpp
 * \brief 复核修复包 D 批 (2026-08-24②) — 暂停源单一化契约测试
 *
 * D1: 下单错误风暴只走 qphase=ERROR 单轨 (指数退避自探恢复),
 *     不再叠加 haltTrading(REVERISIBLE)。_trading_halted 标志属 FutuRiskMonitor,
 *     仅由真正的风控 halt 路径设置 (delta-rate/exposure/DAILY_LOSS/arb EMERGENCY/
 *     channel-lost)。
 *
 * 本文件在 TradingState 层固化该契约的状态机半边:
 *   - ERROR 态阻断报价 (canQuote()==false)
 *   - ERROR 态可被 tryResumeFrom(ERROR) 精确恢复 (CAS 防跨态闪烁)
 *   - RISK_HALTED 进入受限守卫不受影响
 */
#include "../WtFutuCore/TradingState.h"
#include "gtest/gtest/gtest.h"

using namespace futu;

// ERROR 单轨: 阻断报价但语义上不是 risk-halt (无 halted 标志伴随)
TEST(ErrorSingleTrack, ErrorBlocksQuotingAndIsRecoverable)
{
    TradingState ts;
    ts.reset();
    EXPECT_TRUE(ts.canQuote());

    ASSERT_TRUE(ts.setQuotingPhase(QuotingPhase::ERROR));
    EXPECT_FALSE(ts.canQuote());                          // 报价被阻断
    EXPECT_EQ(ts.qphase.load(), QuotingPhase::ERROR);

    // 指数退避自探恢复路径: 仅当当前==ERROR 才翻 NORMAL
    EXPECT_TRUE(ts.tryResumeFrom(QuotingPhase::ERROR));
    EXPECT_TRUE(ts.canQuote());
}

// RISK_HALTED 守卫不因 D1 改变: HALT 态拒绝进入其它子态, 唯一出口 NORMAL
TEST(ErrorSingleTrack, RiskHaltedGuardUnchanged)
{
    TradingState ts;
    ts.reset();
    ASSERT_TRUE(ts.setQuotingPhase(QuotingPhase::RISK_HALTED));
    // HALT 期间 MARKET/TOXICITY/ERROR 的 shouldPause 分支不得误翻 NORMAL
    EXPECT_FALSE(ts.setQuotingPhase(QuotingPhase::MARKET));
    EXPECT_FALSE(ts.setQuotingPhase(QuotingPhase::TOXICITY));
    EXPECT_FALSE(ts.setQuotingPhase(QuotingPhase::ERROR));
    EXPECT_FALSE(ts.tryResumeFrom(QuotingPhase::ERROR)); // 非 NORMAL 出口被拒
    EXPECT_EQ(ts.qphase.load(), QuotingPhase::RISK_HALTED);
    // 唯一合法出口
    ts.resumeFromRisk();
    EXPECT_TRUE(ts.canQuote());
}

// CLOSEOUT 相位与 ERROR 正交: setQuotingPhase 不校验顶层相位,
// 报价阻断由 canQuote() 的 MmPhase 判定承担 (closeout 优先)
TEST(ErrorSingleTrack, CloseoutOrthogonalToError)
{
    TradingState ts;
    ts.reset();
    ts.enterCloseout();
    EXPECT_FALSE(ts.canQuote()); // CLOSEOUT 相位本身不可报价
    // qphase 转移在 closeout 下仍可发生 (正交维度), 但报价保持阻断
    ts.setQuotingPhase(QuotingPhase::ERROR);
    EXPECT_FALSE(ts.canQuote());
}
