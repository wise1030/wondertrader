/*!
 * \file test_close_freeze_ledger.cpp
 * \brief 平仓可用量预冻结台账 (F-core v3.1 / C1) unit tests — 2026-09-11
 *
 * 背景: 09-11 全天 889 次 `CTP:平今仓位不足(50/51)` 拒单 → 全量停报 60s 自维持循环
 *       → 双边覆盖率崩塌 (0.06%-3%, 基线 94-99%)。实证 22:25:35 同一 burst 内
 *       scout(1 手) 与义务层(10 手) 各自按**未扣减的可用量**拆出平仓腿 (拆 6 手).
 *
 * 本台账是 F-core v3.1 的不变量载体:
 *   可用量 = 柜台基线 - 本通道在途平仓未终态量
 *   - 下单成功即冻结 (reserve); 回报只负责归还/转换/对账
 *   - 拒单/撤单**必须命中台账**才归还 (exactly-once); 未命中一律不归还只计数
 *     → 重复推送 / 乱序回报 / 他源镜像单造成的"幻影注入"在结构上不可能发生
 *   - 成交只累计 traded; 撤单归还 qty - traded; 全成交销账
 */
#include "../WtUftCore/CloseFreezeLedger.h"
#include "gtest/gtest.h"

#include <thread>
#include <vector>

using namespace wtp;

namespace
{

constexpr const char* CODE_A = "SHFE.ao2611";
constexpr const char* CODE_B = "SHFE.ao2612";

} // namespace

// 下单成功冻结 -> 拒单归还一次, 第二次同 localid 的终态事件不再归还(exactly-once)
TEST(CloseFreezeLedger, ReserveThenReleaseOnce)
{
	CloseFreezeLedger ledger;

	ASSERT_TRUE(ledger.reserve(1001, CODE_A, true, true, 6.0));
	EXPECT_TRUE(ledger.contains(1001));
	EXPECT_EQ(ledger.size(), 1u);

	CloseFreezeLedger::Entry e;
	double qty = 0.0;
	ASSERT_TRUE(ledger.release(1001, e, qty));
	EXPECT_DOUBLE_EQ(qty, 6.0);
	EXPECT_EQ(e.stdCode, CODE_A);
	EXPECT_TRUE(e.isLong);
	EXPECT_TRUE(e.isToday);
	EXPECT_FALSE(ledger.contains(1001));

	// 第二次(重复推送/重复撤单推送) -> 不归还, 只计 unmatched
	ASSERT_FALSE(ledger.release(1001, e, qty));
	EXPECT_DOUBLE_EQ(qty, 0.0);

	auto st = ledger.stats();
	EXPECT_EQ(st.reserved, 1u);
	EXPECT_EQ(st.released, 1u);
	EXPECT_EQ(st.unmatched, 1u);
}

// 部分成交后撤单: 只归还未成交部分 (委托量 - 成交量)
TEST(CloseFreezeLedger, PartialFillThenCancelReleasesRemainder)
{
	CloseFreezeLedger ledger;
	ASSERT_TRUE(ledger.reserve(2001, CODE_A, true, true, 6.0));
	ledger.onTrade(2001, 2.0);

	CloseFreezeLedger::Entry e;
	double qty = 0.0;
	ASSERT_TRUE(ledger.release(2001, e, qty));
	EXPECT_DOUBLE_EQ(qty, 4.0);
}

// 全部成交 -> 台账自动销账; 之后的撤单推送属于未命中, 不得归还(防幻影)
TEST(CloseFreezeLedger, FullFillRetiresEntryAndLaterCancelIsUnmatched)
{
	CloseFreezeLedger ledger;
	ASSERT_TRUE(ledger.reserve(3001, CODE_B, false, false, 5.0));
	ledger.onTrade(3001, 5.0);
	EXPECT_FALSE(ledger.contains(3001));

	CloseFreezeLedger::Entry e;
	double qty = 0.0;
	ASSERT_FALSE(ledger.release(3001, e, qty));
	EXPECT_EQ(ledger.stats().filled_out, 1u);
	EXPECT_EQ(ledger.stats().unmatched, 1u);
}

// 登录回放播种(存活平仓单的冻结已含在登录查询 avail 净值里) -> 后续撤单可正常归还
TEST(CloseFreezeLedger, SeedFromLoginReplayIsReleasable)
{
	CloseFreezeLedger ledger;
	ledger.seed(4001, CODE_A, true, false, 3.0);
	ASSERT_TRUE(ledger.contains(4001));

	CloseFreezeLedger::Entry e;
	double qty = 0.0;
	ASSERT_TRUE(ledger.release(4001, e, qty));
	EXPECT_DOUBLE_EQ(qty, 3.0);
	EXPECT_FALSE(e.isToday); // 平昨
}

// 他源/未知 localid 的终态事件: 不得归还, 只计数
TEST(CloseFreezeLedger, UnknownLocalIdNeverReleases)
{
	CloseFreezeLedger ledger;
	CloseFreezeLedger::Entry e;
	double qty = 0.0;
	ASSERT_FALSE(ledger.release(9999, e, qty));
	EXPECT_EQ(ledger.stats().released, 0u);
	EXPECT_EQ(ledger.stats().unmatched, 1u);
}

// 钳制计数(C7 巡检探针): 由调用方在归还被钳制时登记
TEST(CloseFreezeLedger, ClampCounterIsObservable)
{
	CloseFreezeLedger ledger;
	EXPECT_EQ(ledger.stats().clamped, 0u);
	ledger.noteClamp();
	ledger.noteClamp();
	EXPECT_EQ(ledger.stats().clamped, 2u);

	// clear 只清在途条目, 累计巡检计数保留(便于跨会话观察)
	ASSERT_TRUE(ledger.reserve(7001, CODE_A, true, true, 1.0));
	ledger.clear();
	EXPECT_EQ(ledger.size(), 0u);
	EXPECT_EQ(ledger.stats().clamped, 2u);
	EXPECT_EQ(ledger.stats().reserved, 1u);
}

// 非法/重复登记: 不冻结(reserve 返回 false)
TEST(CloseFreezeLedger, ReserveRejectsInvalidAndDuplicate)
{
	CloseFreezeLedger ledger;
	EXPECT_FALSE(ledger.reserve(UINT_MAX, CODE_A, true, true, 1.0));
	EXPECT_FALSE(ledger.reserve(5001, CODE_A, true, true, 0.0));
	ASSERT_TRUE(ledger.reserve(5001, CODE_A, true, true, 1.0));
	EXPECT_FALSE(ledger.reserve(5001, CODE_A, true, true, 1.0));
	EXPECT_EQ(ledger.stats().reserved, 1u);
}

// 在途冻结量按 合约/方向/今昨 聚合 (C6 快照重放口径)
TEST(CloseFreezeLedger, PendingQtyAggregatesByContractSideAndToday)
{
	CloseFreezeLedger ledger;
	ASSERT_TRUE(ledger.reserve(6001, CODE_A, true, true, 6.0));	// 平今多
	ASSERT_TRUE(ledger.reserve(6002, CODE_A, true, false, 3.0));	// 平昨多
	ASSERT_TRUE(ledger.reserve(6003, CODE_A, false, true, 4.0));	// 平今空
	ASSERT_TRUE(ledger.reserve(6004, CODE_B, true, true, 2.0));	// 别的合约
	ledger.onTrade(6001, 1.0);

	EXPECT_DOUBLE_EQ(ledger.pendingQty(CODE_A, true, 2), 8.0);	// 5(今) + 3(昨)
	EXPECT_DOUBLE_EQ(ledger.pendingQty(CODE_A, true, 0), 5.0);	// 仅平今
	EXPECT_DOUBLE_EQ(ledger.pendingQty(CODE_A, true, 1), 3.0);	// 仅平昨
	EXPECT_DOUBLE_EQ(ledger.pendingQty(CODE_A, false, 2), 4.0);
	EXPECT_DOUBLE_EQ(ledger.pendingQty(CODE_B, true, 2), 2.0);
}

// 并发冒烟: md 线程预扣 vs 回报线程归还 (台账须自持锁)
TEST(CloseFreezeLedger, ConcurrentReserveReleaseSmoke)
{
	CloseFreezeLedger ledger;
	constexpr int kThreads = 4;
	constexpr int kOps = 2000;

	std::vector<std::thread> ths;
	for (int t = 0; t < kThreads; t++)
	{
		ths.emplace_back([&ledger, t]() {
			for (int i = 0; i < kOps; i++)
			{
				uint32_t localid = static_cast<uint32_t>(t * kOps + i + 1);
				if (!ledger.reserve(localid, CODE_A, true, true, 1.0))
					continue;
				ledger.onTrade(localid, 0.4);
				CloseFreezeLedger::Entry e;
				double qty = 0.0;
				ledger.release(localid, e, qty);
			}
		});
	}
	for (auto& th : ths)
		th.join();

	auto st = ledger.stats();
	EXPECT_EQ(st.reserved, static_cast<uint64_t>(kThreads) * kOps);
	EXPECT_EQ(st.released, static_cast<uint64_t>(kThreads) * kOps);
	EXPECT_EQ(st.unmatched, 0u);
	EXPECT_NEAR(st.released_qty, static_cast<double>(kThreads) * kOps * 0.6, 1e-6);
	EXPECT_EQ(ledger.size(), 0u);
}
