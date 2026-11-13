//==============================================================================
// F-core P1-6 阶段 3 前置回归(2026-09-19): 收盘清算状态机不变式
//   背景: 阶段 3 要把清算状态收敛为"单一权威"; 收敛前先用用例钉住现状不变式:
//     ① 合法/非法转移矩阵 ② 失败→限时重试链 ③ **卡死守卫**(非 force reset 被状态机拒绝,
//     session begin 必须用 force=true 硬清) —— 对应 2026-08-31 R8-c 立案的
//     "closeout FAILED 后重试死锁/上一日残留 FLATTENING 导致永久死锁" 同类缺陷;
//     ④ 触发即 pauseQuoting ⇒ P1-6 原因位图应出现 QUOTING_PAUSED (跨阶段一致性)。
//==============================================================================

#include <gtest/gtest.h>

#include "../WtFutuCore/FutuRiskMonitor.h"
#include "../WtFutuCore/HaltReason.h"
#include "../WtFutuCore/HaltGate.h"

using namespace futu;

namespace
{
void driveTo(FutuRiskMonitor& rm, CloseoutSub target, uint64_t ts)
{
	// 按合法路径推进: IDLE→TRIGGERED→DRAINING→ASSESSING→EXECUTING→COMPLETED/FAILED
	switch (target)
	{
	case CloseoutSub::TRIGGERED:
		rm.markCloseoutTriggered(ts);
		break;
	case CloseoutSub::DRAINING:
		rm.markCloseoutTriggered(ts);
		rm.markCloseoutDraining(ts + 1);
		break;
	case CloseoutSub::FAILED:
		rm.markCloseoutTriggered(ts);
		rm.markCloseoutDraining(ts + 1);
		rm.markCloseoutFailed(ts + 2);
		break;
	case CloseoutSub::EXECUTING:
		rm.markCloseoutTriggered(ts);
		rm.markCloseoutDraining(ts + 1);
		rm.transitionCloseoutSub(CloseoutSub::ASSESSING, ts + 2);
		rm.transitionCloseoutSub(CloseoutSub::EXECUTING, ts + 3);
		break;
	default:
		break;
	}
}
} // namespace

// 初态 + 触发链 + 时间戳落地
TEST(FutuCloseoutState, TriggerChainAndTimestamps)
{
	FutuRiskMonitor rm;
	HaltReasons hr;                 // §20.9 批 1: 实例位图
	rm.setHaltReasons(&hr);
	EXPECT_EQ(rm.getCloseoutSub(), CloseoutSub::IDLE);
	EXPECT_FALSE(rm.isCloseoutTriggered());

	hr.clearAll();
	rm.markCloseoutTriggered(1000);
	EXPECT_EQ(rm.getCloseoutSub(), CloseoutSub::TRIGGERED);
	EXPECT_TRUE(rm.isCloseoutTriggered());           // 非 IDLE 即视为"清算进行中"
	EXPECT_EQ(rm.getCloseoutSubInfo().trigger_time, 1000u);
	// 触发时 pauseQuoting() ⇒ P1-6 原因位图必须同步出现 QUOTING_PAUSED (跨阶段一致性)
	EXPECT_TRUE(hr.has(HR_QUOTING_PAUSED));

	rm.markCloseoutDraining(2000);
	EXPECT_EQ(rm.getCloseoutSub(), CloseoutSub::DRAINING);
	EXPECT_EQ(rm.getCloseoutSubInfo().flatten_start, 2000u);

	rm.markCloseoutCompleted(3000);
	EXPECT_EQ(rm.getCloseoutSub(), CloseoutSub::COMPLETED);
	EXPECT_TRUE(rm.isCloseoutCompleted());
	EXPECT_EQ(rm.getCloseoutSubInfo().complete_time, 3000u);

	hr.clearAll();
}

// 非法转移必须被状态机拒绝且不改状态 (IDLE 只能 → TRIGGERED)
TEST(FutuCloseoutState, IllegalTransitionsRejected)
{
	FutuRiskMonitor rm;
	HaltReasons hr;
	rm.setHaltReasons(&hr);
	EXPECT_FALSE(rm.transitionCloseoutSub(CloseoutSub::EXECUTING, 1));
	EXPECT_EQ(rm.getCloseoutSub(), CloseoutSub::IDLE);
	EXPECT_FALSE(rm.transitionCloseoutSub(CloseoutSub::COMPLETED, 1));
	EXPECT_EQ(rm.getCloseoutSub(), CloseoutSub::IDLE);

	// TRIGGERED 不能直接跳 ASSESSING (必须经 DRAINING)
	rm.markCloseoutTriggered(10);
	EXPECT_FALSE(rm.transitionCloseoutSub(CloseoutSub::ASSESSING, 11));
	EXPECT_EQ(rm.getCloseoutSub(), CloseoutSub::TRIGGERED);

	hr.clearAll();
}

// 失败 → 限时重试: 早于 retry_interval 拒绝, 到点放行; 超过 max_retries 后不再重试
TEST(FutuCloseoutState, FailedRetriesAreBoundedAndTimed)
{
	FutuRiskMonitor rm;
	HaltReasons hr;
	rm.setHaltReasons(&hr);
	driveTo(rm, CloseoutSub::FAILED, 100);
	ASSERT_EQ(rm.getCloseoutSub(), CloseoutSub::FAILED);
	const auto info = rm.getCloseoutSubInfo();
	ASSERT_GT(info.retry_interval_ms, 0u);

	// 未到重试间隔 ⇒ 拒绝
	EXPECT_FALSE(rm.checkCloseoutRetry(info.fail_time + info.retry_interval_ms - 1));
	EXPECT_EQ(rm.getCloseoutSub(), CloseoutSub::FAILED);

	// 到点 ⇒ 进入 RETRYING
	EXPECT_TRUE(rm.checkCloseoutRetry(info.fail_time + info.retry_interval_ms));
	EXPECT_EQ(rm.getCloseoutSub(), CloseoutSub::RETRYING);

	// 重试预算耗尽 (retry_count >= max_retries) ⇒ 不再重试 (需人工干预)
	FutuRiskMonitor rm2;
	HaltReasons hr2;
	rm2.setHaltReasons(&hr2);
	for (uint32_t i = 0; i < 5; ++i)
	{
		rm2.transitionCloseoutSub(CloseoutSub::TRIGGERED, 1 + i * 100000);
		rm2.transitionCloseoutSub(CloseoutSub::DRAINING, 2 + i * 100000);
		rm2.transitionCloseoutSub(CloseoutSub::FAILED, 3 + i * 100000);
		rm2.transitionCloseoutSub(CloseoutSub::RETRYING, 4 + i * 100000);
	}
	EXPECT_GE(rm2.getCloseoutSubInfo().retry_count, rm2.getCloseoutSubInfo().max_retries);
	EXPECT_FALSE(rm2.checkCloseoutRetry(1'000'000'000ull));

	hr.clearAll();
}

// 卡死守卫: 非 force 的 reset 被状态机拒绝 (EXECUTING → IDLE 非法), force=true 必须硬清
//   —— 对应 FutuRuntimeOps::onSessionBegin 的 resetCloseout(true) 语义 (R8-c 同类缺陷)
TEST(FutuCloseoutState, SessionBeginForceResetBreaksStuckState)
{
	FutuRiskMonitor rm;
	HaltReasons hr;
	rm.setHaltReasons(&hr);
	driveTo(rm, CloseoutSub::EXECUTING, 100);
	ASSERT_EQ(rm.getCloseoutSub(), CloseoutSub::EXECUTING);

	rm.resetCloseout(false); // 非 force: 状态机不允许 EXECUTING → IDLE
	EXPECT_EQ(rm.getCloseoutSub(), CloseoutSub::EXECUTING) << "非 force reset 不应能清掉活动态";

	rm.resetCloseout(true); // session begin: 强制清态 (硬边界)
	EXPECT_EQ(rm.getCloseoutSub(), CloseoutSub::IDLE);
	EXPECT_FALSE(rm.isCloseoutTriggered());
	const auto info = rm.getCloseoutSubInfo();
	EXPECT_EQ(info.trigger_time, 0u);
	EXPECT_EQ(info.flatten_start, 0u);
	EXPECT_EQ(info.retry_count, 0u);

	// session begin 的实际序列 (FutuRuntimeOps::onSessionBegin): resetDaily + resetCloseout(true)
	rm.markCloseoutTriggered(5000);
	rm.resetDaily();
	rm.resetCloseout(true);
	EXPECT_EQ(rm.getCloseoutSub(), CloseoutSub::IDLE);

	hr.clearAll();
}

// COMPLETED → IDLE 是唯一允许的"回 IDLE"转移 (夜盘收尾后复位)
TEST(FutuCloseoutState, CompletedAllowsResetToIdle)
{
	FutuRiskMonitor rm;
	HaltReasons hr;
	rm.setHaltReasons(&hr);
	rm.markCloseoutTriggered(1);
	rm.markCloseoutDraining(2);
	rm.markCloseoutCompleted(3);
	ASSERT_EQ(rm.getCloseoutSub(), CloseoutSub::COMPLETED);

	rm.resetCloseout(false); // COMPLETED → IDLE 在矩阵内, 无需 force
	EXPECT_EQ(rm.getCloseoutSub(), CloseoutSub::IDLE);

	hr.clearAll();
}
