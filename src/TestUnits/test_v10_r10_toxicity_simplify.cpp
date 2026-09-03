/*!
 * \file test_v10_r10_toxicity_simplify.cpp
 * \brief V10-R10 毒性响应简化 — 单层"检测即双边暂停"契约测试
 *
 * 目标语义 (AGENTS.md 2026-09-02③ 方案, 用户已确认):
 *   - is_toxic → TOXICITY 相位: canQuote()==false (双边报价暂停) + 边沿 cancelAll
 *   - 无毒且超过 resume-hold (cooloffMs) → tryResumeFrom(TOXICITY) 恢复
 *   - 单边抑制 (toxic_side)/GLFT 毒性加宽/链内冷却全部删除
 *   - isActive() 在 TOXICITY 时保持 true (只停报价, 其余子系统不受累)
 *
 * 本文件在 TradingState 层固化该契约的状态机半边
 * ( ToxicityPolicy 单边抑制等删除路径以编译期不存在的方式防回归 —
 *   引用已删符号即无法通过编译 ):
 *   - TOXICITY 态阻断报价且 isActive()==true
 *   - TOXICITY 恢复走 tryResumeFrom(TOXICITY) 精确匹配 (CAS 防跨态闪烁)
 *   - 同 tick 双源同触发时 TOXICITY 后入胜出, 市场恢复分支不互翻 TOXICITY
 *   - RISK_HALTED 守卫: HALT 态拒绝 TOXICITY 进入
 */
#include "../WtFutuCore/TradingState.h"
#include "gtest/gtest/gtest.h"

using namespace futu;

// 毒性暂停 = 只停报价: canQuote()==false 但 isActive()==true (成交/风控/closeout 不受累)
TEST(ToxicitySimplify, ToxicityPhaseBlocksQuotingOnly)
{
    TradingState ts;
    ts.reset();
    EXPECT_TRUE(ts.canQuote());
    EXPECT_TRUE(ts.isActive());

    ASSERT_TRUE(ts.setQuotingPhase(QuotingPhase::TOXICITY));
    EXPECT_FALSE(ts.canQuote()); // 双边报价暂停
    EXPECT_TRUE(ts.isActive());  // 其余子系统保持活跃 (语义 vs MARKET 的关键差异)
    EXPECT_EQ(ts.qphase.load(), QuotingPhase::TOXICITY);

    // 恢复: 仅 expected==TOXICITY 才翻 NORMAL (无毒 + resume-hold 期满后的唯一出口)
    EXPECT_TRUE(ts.tryResumeFrom(QuotingPhase::TOXICITY));
    EXPECT_TRUE(ts.canQuote());
}

// 同 tick 双源同触发: MARKET 先入、TOXICITY 后入胜出 (转移自由);
// 市场恢复分支 (tryResumeFrom(MARKET)) 在 TOXICITY 期间为 no-op, 不互翻
TEST(ToxicitySimplify, ToxicityDominatesMarketAndNoCrossResume)
{
    TradingState ts;
    ts.reset();

    // 市场门先进入
    ASSERT_TRUE(ts.setQuotingPhase(QuotingPhase::MARKET));
    // 毒性门同 tick 后入: MARKET→TOXICITY 自由转移, 胜出
    ASSERT_TRUE(ts.setQuotingPhase(QuotingPhase::TOXICITY));
    EXPECT_EQ(ts.qphase.load(), QuotingPhase::TOXICITY);

    // 下一 tick 市场恢复分支: expected=MARKET 不匹配当前 TOXICITY → no-op
    EXPECT_FALSE(ts.tryResumeFrom(QuotingPhase::MARKET));
    EXPECT_EQ(ts.qphase.load(), QuotingPhase::TOXICITY);

    // 毒性恢复分支: 精确匹配 → NORMAL
    EXPECT_TRUE(ts.tryResumeFrom(QuotingPhase::TOXICITY));
    EXPECT_TRUE(ts.canQuote());
}

// 毒性恢复分支在 MARKET 期间同样不得误翻 (对称守卫)
TEST(ToxicitySimplify, ToxicityResumeIsNoOpDuringMarket)
{
    TradingState ts;
    ts.reset();
    ASSERT_TRUE(ts.setQuotingPhase(QuotingPhase::MARKET));

    EXPECT_FALSE(ts.tryResumeFrom(QuotingPhase::TOXICITY));
    EXPECT_EQ(ts.qphase.load(), QuotingPhase::MARKET);
    EXPECT_FALSE(ts.canQuote());
}

// RISK_HALTED 守卫: HALT 态毒性暂停不得进入, 唯一出口 resumeFromRisk
TEST(ToxicitySimplify, RiskHaltedGuardRejectsToxicityEntry)
{
    TradingState ts;
    ts.reset();
    ASSERT_TRUE(ts.setQuotingPhase(QuotingPhase::RISK_HALTED));

    EXPECT_FALSE(ts.setQuotingPhase(QuotingPhase::TOXICITY));
    EXPECT_EQ(ts.qphase.load(), QuotingPhase::RISK_HALTED);
    EXPECT_FALSE(ts.isActive());
}
