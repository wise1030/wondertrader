//==============================================================================
// F-core P1-6 阶段 1(2026-09-19): halt 原因位图 (影子模式)
//   背景: "能不能报价"当前由 4 组状态分别回答; 收敛为单一原因位图 (裁决 A1/B:
//   门禁层用 fetch_or/fetch_and 原子位图, 无锁; 阶段 1 只做镜像与一致性对比)。
// §20.9 批 1(2026-11-12): 位图 per-strategy 实例化 —— 修复进程级 static 被双策略
//   共享的污染 (11-06 根因实锤: ec halt 置位全局 RISK_HALT → ao 影子对比全天分歧)。
//   新增 DualInstanceIsolation 回归用例直接锁定该污染形态。
//==============================================================================

#include <gtest/gtest.h>

#include "../WtFutuCore/HaltReason.h"
#include "../WtFutuCore/HaltGate.h"
#include "../WtFutuCore/TradingState.h"

using namespace futu;

// 位图基本语义: 置位/清位/查询/描述 (实例)
TEST(FutuHaltReasons, BitSetClearAndDescribe)
{
	HaltReasons hr;
	EXPECT_FALSE(hr.blocked());
	EXPECT_EQ(HaltReasons::describe(0), "NONE");

	hr.raise(HR_RISK_HALT);
	hr.raise(HR_QUOTING_PAUSED);
	EXPECT_TRUE(hr.blocked());
	EXPECT_TRUE(hr.has(HR_RISK_HALT));
	EXPECT_EQ(hr.raw(), HaltReasons::bitOf(HR_RISK_HALT) | HaltReasons::bitOf(HR_QUOTING_PAUSED));
	EXPECT_EQ(HaltReasons::describe(hr.raw()), "RISK_HALT|QUOTING_PAUSED");

	hr.clear(HR_RISK_HALT);
	EXPECT_FALSE(hr.has(HR_RISK_HALT));
	EXPECT_TRUE(hr.has(HR_QUOTING_PAUSED));

	// 幂等: 重复置位/清位不改变结果
	hr.raise(HR_QUOTING_PAUSED);
	EXPECT_EQ(hr.raw(), HaltReasons::bitOf(HR_QUOTING_PAUSED));
	hr.clear(HR_RISK_HALT);
	EXPECT_EQ(hr.raw(), HaltReasons::bitOf(HR_QUOTING_PAUSED));

	hr.clearAll();
	EXPECT_EQ(hr.raw(), 0ull);
	EXPECT_EQ(HaltReasons::maskAll(), (1ull << HaltReasons::count()) - 1ull);
}

// §20.9 批 1 核心回归: 双策略实例隔离 —— 11-06 污染形态 (static 位图下 ec halt
//   置位全局 RISK_HALT, ao 影子对比全天 RISK_HALT-only 分歧)。实例化后 A 的置位
//   对 B 不可见。
TEST(FutuHaltReasons, DualInstanceIsolation)
{
	HaltReasons hr_ec;   // 模拟 ec 策略实例
	HaltReasons hr_ao;   // 模拟 ao 策略实例

	hr_ec.raise(HR_RISK_HALT);
	hr_ec.raise(HR_QUOTING_PAUSED);

	EXPECT_TRUE(hr_ec.has(HR_RISK_HALT));
	// 污染回归断言: ao 实例位图不得出现 ec 的 halt 位
	EXPECT_FALSE(hr_ao.has(HR_RISK_HALT));
	EXPECT_FALSE(hr_ao.has(HR_QUOTING_PAUSED));
	EXPECT_EQ(hr_ao.raw(), 0ull);

	// ec 恢复后 ao 仍独立干净; 反向置位同样隔离
	hr_ec.clearAll();
	hr_ao.raise(HR_ZOMBIE_LATCH);
	EXPECT_TRUE(hr_ao.has(HR_ZOMBIE_LATCH));
	EXPECT_FALSE(hr_ec.has(HR_ZOMBIE_LATCH));
	EXPECT_EQ(hr_ec.raw(), 0ull);
}

// TradingState qphase → 原因位镜像 (含 RISK_HALTED 不可被抢占的既有语义)
TEST(FutuHaltReasons, TradingStateMirrorsQPhase)
{
	TradingState ts;

	EXPECT_TRUE(ts.setQuotingPhase(QuotingPhase::TOXICITY));
	EXPECT_TRUE(ts.haltReasons().has(HR_QPHASE_TOXICITY));
	EXPECT_EQ(ts.haltReasons().raw(), HaltReasons::bitOf(HR_QPHASE_TOXICITY));

	EXPECT_TRUE(ts.setQuotingPhase(QuotingPhase::ERROR));
	EXPECT_TRUE(ts.haltReasons().has(HR_QPHASE_ERROR));
	EXPECT_FALSE(ts.haltReasons().has(HR_QPHASE_TOXICITY));

	// RISK_HALTED 不可被直接抢占 (既有语义); 位图应随之置 RISK 位
	EXPECT_TRUE(ts.setQuotingPhase(QuotingPhase::RISK_HALTED));
	EXPECT_FALSE(ts.setQuotingPhase(QuotingPhase::TOXICITY));
	EXPECT_TRUE(ts.haltReasons().has(HR_QPHASE_RISK));
	EXPECT_FALSE(ts.haltReasons().has(HR_QPHASE_TOXICITY));

	// 恢复路径: resumeFromRisk / tryResumeFrom 均需镜像清零
	ts.resumeFromRisk();
	EXPECT_EQ(ts.haltReasons().raw(), 0ull);

	EXPECT_TRUE(ts.setQuotingPhase(QuotingPhase::MARKET));
	EXPECT_TRUE(ts.haltReasons().has(HR_QPHASE_MARKET));
	EXPECT_TRUE(ts.tryResumeFrom(QuotingPhase::MARKET));
	EXPECT_FALSE(ts.haltReasons().has(HR_QPHASE_MARKET));
}

// 双 TradingState 实例的 qphase 镜像同样隔离 (生产形态: 每策略一个壳实例)
TEST(FutuHaltReasons, TradingStateInstancesAreIsolated)
{
	TradingState ts_ec;
	TradingState ts_ao;

	EXPECT_TRUE(ts_ec.setQuotingPhase(QuotingPhase::RISK_HALTED));
	EXPECT_TRUE(ts_ec.haltReasons().has(HR_QPHASE_RISK));
	EXPECT_EQ(ts_ao.haltReasons().raw(), 0ull);
	EXPECT_FALSE(ts_ao.haltReasons().has(HR_QPHASE_RISK));
}

//==============================================================================
// F-core P1-6 阶段 2(2026-09-19): HaltGate 三种模式的门禁求值
//   legacy(默认)=仅既有标志, 与切换前逐比特一致; union=标志∪位图(只可能多停);
//   bits=仅位图(阶段 4 目标态)。§20.9 批 1: blocked/statusLine 传入实例位图。
//==============================================================================

TEST(FutuHaltGate, LegacyModeIgnoresBits)
{
	HaltReasons hr;
	HaltGate::setMode(HaltGateMode::LEGACY);

	EXPECT_FALSE(HaltGate::blocked(false, hr.raw()));
	EXPECT_TRUE(HaltGate::blocked(true, hr.raw()));

	hr.raise(HR_RISK_HALT); // 位图置位不得影响 legacy 模式
	EXPECT_FALSE(HaltGate::blocked(false, hr.raw()));
	EXPECT_TRUE(HaltGate::blocked(true, hr.raw()));

	HaltGate::setMode(HaltGateMode::LEGACY);
}

TEST(FutuHaltGate, UnionModeAddsBitsOnly)
{
	HaltReasons hr;
	HaltGate::setMode(HaltGateMode::UNION);

	EXPECT_FALSE(HaltGate::blocked(false, hr.raw())); // 位图空 ⇒ 与 legacy 同
	EXPECT_TRUE(HaltGate::blocked(true, hr.raw()));

	hr.raise(HR_QUOTING_PAUSED); // 仅位图有 ⇒ union 下拦截 (legacy 下本门禁不拦)
	EXPECT_TRUE(HaltGate::blocked(false, hr.raw()));
	EXPECT_TRUE(HaltGate::blocked(true, hr.raw()));

	HaltGate::setMode(HaltGateMode::LEGACY);
}

TEST(FutuHaltGate, BitsModeIgnoresLegacy)
{
	HaltReasons hr;
	HaltGate::setMode(HaltGateMode::BITS);

	// 位图空时, 即便 legacy 判定为禁止也放行 (bits 为唯一权威 —— 阶段 4 目标态)
	EXPECT_FALSE(HaltGate::blocked(true, hr.raw()));

	hr.raise(HR_RATE_BREACH);
	EXPECT_TRUE(HaltGate::blocked(false, hr.raw()));
	EXPECT_TRUE(HaltGate::blocked(true, hr.raw()));

	HaltGate::setMode(HaltGateMode::LEGACY);
}

// union 模式的跨实例隔离: 本策略位图置位拦截本策略门禁, 不拦截另一策略的门禁
TEST(FutuHaltGate, UnionModeUsesOwnInstanceBits)
{
	HaltReasons hr_ec;
	HaltReasons hr_ao;
	HaltGate::setMode(HaltGateMode::UNION);

	hr_ec.raise(HR_RISK_HALT);
	EXPECT_TRUE(HaltGate::blocked(false, hr_ec.raw()));   // ec 门禁被拦
	EXPECT_FALSE(HaltGate::blocked(false, hr_ao.raw()));  // ao 门禁不受影响

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

	HaltReasons hr;
	hr.raise(HR_ZOMBIE_LATCH);
	HaltGate::setMode(HaltGateMode::UNION);
	const std::string line = HaltGate::statusLine(hr.raw());
	EXPECT_NE(line.find("mode=union"), std::string::npos) << line;
	EXPECT_NE(line.find("ZOMBIE_LATCH"), std::string::npos) << line;

	HaltGate::setMode(HaltGateMode::LEGACY);
}

// 方向软禁与 reset 的镜像 (实例位图)
TEST(FutuHaltReasons, TradingStateMirrorsDirectionBlocks)
{
	TradingState ts;

	ts.blockLong();
	EXPECT_TRUE(ts.haltReasons().has(HR_LONG_BLOCKED));
	ts.blockShort();
	EXPECT_TRUE(ts.haltReasons().has(HR_SHORT_BLOCKED));
	ts.unblockLong();
	EXPECT_FALSE(ts.haltReasons().has(HR_LONG_BLOCKED));
	EXPECT_TRUE(ts.haltReasons().has(HR_SHORT_BLOCKED));

	ts.reset();
	EXPECT_EQ(ts.haltReasons().raw(), 0ull);
}
