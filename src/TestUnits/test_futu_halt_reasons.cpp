//==============================================================================
// F-core P1-6 阶段 1(2026-09-19): halt 原因位图 (影子模式)
//   背景: "能不能报价"当前由 4 组状态分别回答; 收敛为单一原因位图 (裁决 A1/B:
//   门禁层用 fetch_or/fetch_and 原子位图, 无锁; 阶段 1 只做镜像与一致性对比)。
//==============================================================================

#include <gtest/gtest.h>

#include "../WtFutuCore/HaltReason.h"
#include "../WtFutuCore/HaltGate.h"
#include "../WtFutuCore/TradingState.h"

using namespace futu;

// 位图基本语义: 置位/清位/查询/描述
TEST(FutuHaltReasons, BitSetClearAndDescribe)
{
	HaltReasons::clearAll();
	EXPECT_FALSE(HaltReasons::blocked());
	EXPECT_EQ(HaltReasons::describe(0), "NONE");

	HaltReasons::raise(HR_RISK_HALT);
	HaltReasons::raise(HR_QUOTING_PAUSED);
	EXPECT_TRUE(HaltReasons::blocked());
	EXPECT_TRUE(HaltReasons::has(HR_RISK_HALT));
	EXPECT_EQ(HaltReasons::raw(), HaltReasons::bitOf(HR_RISK_HALT) | HaltReasons::bitOf(HR_QUOTING_PAUSED));
	EXPECT_EQ(HaltReasons::describe(HaltReasons::raw()), "RISK_HALT|QUOTING_PAUSED");

	HaltReasons::clear(HR_RISK_HALT);
	EXPECT_FALSE(HaltReasons::has(HR_RISK_HALT));
	EXPECT_TRUE(HaltReasons::has(HR_QUOTING_PAUSED));

	// 幂等: 重复置位/清位不改变结果
	HaltReasons::raise(HR_QUOTING_PAUSED);
	EXPECT_EQ(HaltReasons::raw(), HaltReasons::bitOf(HR_QUOTING_PAUSED));
	HaltReasons::clear(HR_RISK_HALT);
	EXPECT_EQ(HaltReasons::raw(), HaltReasons::bitOf(HR_QUOTING_PAUSED));

	HaltReasons::clearAll();
	EXPECT_EQ(HaltReasons::raw(), 0ull);
	EXPECT_EQ(HaltReasons::maskAll(), (1ull << HaltReasons::count()) - 1ull);
}

// TradingState qphase → 原因位镜像 (含 RISK_HALTED 不可被抢占的既有语义)
TEST(FutuHaltReasons, TradingStateMirrorsQPhase)
{
	HaltReasons::clearAll();
	TradingState ts;

	EXPECT_TRUE(ts.setQuotingPhase(QuotingPhase::TOXICITY));
	EXPECT_TRUE(HaltReasons::has(HR_QPHASE_TOXICITY));
	EXPECT_EQ(HaltReasons::raw(), HaltReasons::bitOf(HR_QPHASE_TOXICITY));

	EXPECT_TRUE(ts.setQuotingPhase(QuotingPhase::ERROR));
	EXPECT_TRUE(HaltReasons::has(HR_QPHASE_ERROR));
	EXPECT_FALSE(HaltReasons::has(HR_QPHASE_TOXICITY));

	// RISK_HALTED 不可被直接抢占 (既有语义); 位图应随之置 RISK 位
	EXPECT_TRUE(ts.setQuotingPhase(QuotingPhase::RISK_HALTED));
	EXPECT_FALSE(ts.setQuotingPhase(QuotingPhase::TOXICITY));
	EXPECT_TRUE(HaltReasons::has(HR_QPHASE_RISK));
	EXPECT_FALSE(HaltReasons::has(HR_QPHASE_TOXICITY));

	// 恢复路径: resumeFromRisk / tryResumeFrom 均需镜像清零
	ts.resumeFromRisk();
	EXPECT_EQ(HaltReasons::raw(), 0ull);

	EXPECT_TRUE(ts.setQuotingPhase(QuotingPhase::MARKET));
	EXPECT_TRUE(HaltReasons::has(HR_QPHASE_MARKET));
	EXPECT_TRUE(ts.tryResumeFrom(QuotingPhase::MARKET));
	EXPECT_FALSE(HaltReasons::has(HR_QPHASE_MARKET));

	HaltReasons::clearAll();
}

//==============================================================================
// F-core P1-6 阶段 2(2026-09-19): HaltGate 三种模式的门禁求值
//   legacy(默认)=仅既有标志, 与切换前逐比特一致; union=标志∪位图(只可能多停);
//   bits=仅位图(阶段 4 目标态)。
//==============================================================================

TEST(FutuHaltGate, LegacyModeIgnoresBits)
{
	HaltReasons::clearAll();
	HaltGate::setMode(HaltGateMode::LEGACY);

	EXPECT_FALSE(HaltGate::blocked(false));
	EXPECT_TRUE(HaltGate::blocked(true));

	HaltReasons::raise(HR_RISK_HALT); // 位图置位不得影响 legacy 模式
	EXPECT_FALSE(HaltGate::blocked(false));
	EXPECT_TRUE(HaltGate::blocked(true));

	HaltReasons::clearAll();
}

TEST(FutuHaltGate, UnionModeAddsBitsOnly)
{
	HaltReasons::clearAll();
	HaltGate::setMode(HaltGateMode::UNION);

	EXPECT_FALSE(HaltGate::blocked(false)); // 位图空 ⇒ 与 legacy 同
	EXPECT_TRUE(HaltGate::blocked(true));

	HaltReasons::raise(HR_QUOTING_PAUSED); // 仅位图有 ⇒ union 下拦截 (legacy 下本门禁不拦)
	EXPECT_TRUE(HaltGate::blocked(false));
	EXPECT_TRUE(HaltGate::blocked(true));

	HaltReasons::clearAll();
	HaltGate::setMode(HaltGateMode::LEGACY);
}

TEST(FutuHaltGate, BitsModeIgnoresLegacy)
{
	HaltReasons::clearAll();
	HaltGate::setMode(HaltGateMode::BITS);

	// 位图空时, 即便 legacy 判定为禁止也放行 (bits 为唯一权威 —— 阶段 4 目标态)
	EXPECT_FALSE(HaltGate::blocked(true));

	HaltReasons::raise(HR_RATE_BREACH);
	EXPECT_TRUE(HaltGate::blocked(false));
	EXPECT_TRUE(HaltGate::blocked(true));

	HaltReasons::clearAll();
	HaltGate::setMode(HaltGateMode::LEGACY);
}

TEST(FutuHaltGate, ParseModeAndStatusLine)
{
	HaltGateMode m = HaltGateMode::UNION;
	EXPECT_TRUE(HaltGate::parseMode("legacy", m));
	EXPECT_EQ(m, HaltGateMode::LEGACY);
	EXPECT_TRUE(HaltGate::parseMode("union", m));
	EXPECT_EQ(m, HaltGateMode::UNION);
	EXPECT_TRUE(HaltGate::parseMode("bits", m));
	EXPECT_EQ(m, HaltGateMode::BITS);
	EXPECT_FALSE(HaltGate::parseMode("bogus", m)); // 非法值不改变 out
	EXPECT_EQ(m, HaltGateMode::BITS);
	EXPECT_FALSE(HaltGate::parseMode(nullptr, m));

	HaltReasons::clearAll();
	HaltReasons::raise(HR_ZOMBIE_LATCH);
	HaltGate::setMode(HaltGateMode::UNION);
	const std::string line = HaltGate::statusLine();
	EXPECT_NE(line.find("mode=union"), std::string::npos) << line;
	EXPECT_NE(line.find("ZOMBIE_LATCH"), std::string::npos) << line;

	HaltReasons::clearAll();
	HaltGate::setMode(HaltGateMode::LEGACY);
}

// 方向软禁与 reset 的镜像
TEST(FutuHaltReasons, TradingStateMirrorsDirectionBlocks)
{
	HaltReasons::clearAll();
	TradingState ts;

	ts.blockLong();
	EXPECT_TRUE(HaltReasons::has(HR_LONG_BLOCKED));
	ts.blockShort();
	EXPECT_TRUE(HaltReasons::has(HR_SHORT_BLOCKED));
	ts.unblockLong();
	EXPECT_FALSE(HaltReasons::has(HR_LONG_BLOCKED));
	EXPECT_TRUE(HaltReasons::has(HR_SHORT_BLOCKED));

	ts.reset();
	EXPECT_EQ(HaltReasons::raw(), 0ull);

	HaltReasons::clearAll();
}
