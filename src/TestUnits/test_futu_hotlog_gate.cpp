//==============================================================================
// F-core P0-B(2026-09-18): HotLogGate 采样语义
//   背景: 09-17 单日 Runner 日志 342,544 行/47MB, debug 占 86.6%, 热路径 ~166k 行/日。
//   闸门只决定"是否落日志", 必须保证: 关=全不落; 开=严格 1/n 采样; 计数可观测。
//==============================================================================

#include <gtest/gtest.h>

#include "../WtFutuCore/HotLogGate.h"

using namespace futu;

TEST(FutuHotLogGate, DisabledDropsEverythingAndCountsSkips)
{
	HotLogGate::configure(false, 10);
	const uint64_t base_skip = HotLogGate::skipped();
	for (int i = 0; i < 50; ++i)
		EXPECT_FALSE(HotLogGate::hit());
	EXPECT_EQ(HotLogGate::skipped() - base_skip, 50u);
}

TEST(FutuHotLogGate, SampleEveryNHitsExactlyOnce)
{
	HotLogGate::configure(true, 10);
	const uint64_t base_emit = HotLogGate::emitted();
	int hits = 0;
	for (int i = 0; i < 100; ++i)
		if (HotLogGate::hit())
			++hits;
	EXPECT_EQ(hits, 10);
	EXPECT_EQ(HotLogGate::emitted() - base_emit, 10u);
}

TEST(FutuHotLogGate, SampleEveryOnePassesAll)
{
	HotLogGate::configure(true, 1);
	const uint64_t base_emit = HotLogGate::emitted();
	for (int i = 0; i < 20; ++i)
		EXPECT_TRUE(HotLogGate::hit());
	EXPECT_EQ(HotLogGate::emitted() - base_emit, 20u);
}

TEST(FutuHotLogGate, ZeroSampleTreatedAsOne)
{
	HotLogGate::configure(true, 0);
	EXPECT_EQ(HotLogGate::sampleEvery(), 1u);
	EXPECT_TRUE(HotLogGate::hit());
}
