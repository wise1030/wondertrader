/*!
 * \file test_order_slot_bplus.cpp
 * \brief B+ 订单槽状态机 (2026-08-21) unit tests
 *
 * 背景: 2026-08-19 僵尸单事故 — 撤单"发送即遗忘"+ tracker 5s force-untrack,
 *       撤单被 CTP 前置流控静默吞掉后策略遗忘活单 (8/19 timeout 3633 次,
 *       次日晨残留 858 手僵尸单冻结可平量/幽灵成交污染簿记)。
 *
 * B+ 语义:
 *   - mark 时写 cancel_time (不再懒赋值)
 *   - PendingCancel 仍计入全源 pending (风控保守口径, 防双份敞口)
 *   - 超时重发撤单 (重试间隔默认300ms), 重试 K 次后置 IS_ZOMBIE
 *   - IS_ZOMBIE 保留跟踪+计入 pending+升级列表, **永不 force-untrack 活单**
 *   - clearZombies 通道恢复锚点: untrack zombie + 清升级去重表
 *
 * 注: mocker 撤单必成功, 超时/重试/zombie 路径回测不可复现 — 本文件是唯一
 *     覆盖这些路径的测试 (配合实盘小流量验证)。
 */
#include "../WtFutuCore/UnifiedOrderTracker.h"
#include "../WtFutuCore/FutuRiskMonitor.h"
#include "../WtFutuCore/FutuPortfolio.h"
#include "gtest/gtest.h"

using namespace futu;

namespace
{

constexpr const char* CODE = "SHFE.ao.ao2609";

void initTracker(UnifiedOrderTracker& t)
{
    UnifiedTrackerConfig cfg;
    cfg.pending_cancel_timeout_ms = 300;
    cfg.cancel_max_retries = 3;
    t.setConfig(cfg);
}

} // namespace

// mark 时写 cancel_time; PendingCancel 仍计入全源 pending
TEST(OrderSlotBPlus, MarkWritesCancelTimeAndKeepsPending)
{
    UnifiedOrderTracker t;
    initTracker(t);
    t.trackMMOrder(1, 0, CODE, 2663.0, 10, 2663.0, 1000, true);
    EXPECT_DOUBLE_EQ(t.getPendingBuyQtyAllSources(CODE), 10.0);

    ASSERT_TRUE(t.tryMarkPendingCancel(1, CancelReason::MANUAL, 1000));

    UnifiedOrderInfo oi;
    ASSERT_TRUE(t.getOrderInfoCopy(1, oi));
    EXPECT_EQ(oi.cancel_time, 1000u); // mark 时刻, 非懒赋值
    EXPECT_EQ(oi.cancel_retry_count, 0u);
    EXPECT_TRUE(oi.isPendingCancel());

    // B+ 核心口径: pendingCancel 仍计入全源 pending (撤单未确认前敞口未消)
    EXPECT_DOUBLE_EQ(t.getPendingBuyQtyAllSources(CODE), 10.0);
}

// 撤单 ack (untrack) 时 pending 正确归零
TEST(OrderSlotBPlus, UntrackDeductsPendingForPendingCancelOrder)
{
    UnifiedOrderTracker t;
    initTracker(t);
    t.trackMMOrder(1, 0, CODE, 2663.0, 10, 2663.0, 1000, true);
    ASSERT_TRUE(t.tryMarkPendingCancel(1, CancelReason::MANUAL, 1000));
    EXPECT_DOUBLE_EQ(t.getPendingBuyQtyAllSources(CODE), 10.0);

    t.untrackOrder(1, 1400); // Cncld 到达
    EXPECT_DOUBLE_EQ(t.getPendingBuyQtyAllSources(CODE), 0.0);
    UnifiedOrderInfo oi;
    EXPECT_FALSE(t.getOrderInfoCopy(1, oi));
}

// 超时重发: 间隔内无动作, 到点产生 TIMEOUT 重试动作并刷新计时/计数
TEST(OrderSlotBPlus, TimeoutProducesRetryAction)
{
    UnifiedOrderTracker t;
    initTracker(t);
    t.trackMMOrder(1, 0, CODE, 2663.0, 10, 2663.0, 1000, true);
    ASSERT_TRUE(t.tryMarkPendingCancel(1, CancelReason::MANUAL, 1000));

    // 间隔内 (1000+299) 无动作
    const auto& a1 = t.checkAutoCancel(CODE, 1299, 2663.0, 0.2, false);
    EXPECT_TRUE(a1.empty());

    // 到点 (1300) 产生重试
    const auto& a2 = t.checkAutoCancel(CODE, 1300, 2663.0, 0.2, false);
    ASSERT_EQ(a2.size(), 1u);
    EXPECT_EQ(a2[0].order_id, 1u);
    EXPECT_EQ(a2[0].reason, CancelReason::TIMEOUT);

    UnifiedOrderInfo oi;
    ASSERT_TRUE(t.getOrderInfoCopy(1, oi));
    EXPECT_EQ(oi.cancel_retry_count, 1u);
    EXPECT_EQ(oi.cancel_time, 1300u); // 重试刷新计时
    EXPECT_FALSE(oi.isZombie());
    EXPECT_TRUE(t.getZombieEscalations().empty());
}

// 重试 K 次后置 IS_ZOMBIE: 保留跟踪+计入 pending+升级列表去重, 不再重试
TEST(OrderSlotBPlus, ZombieAfterMaxRetriesKeepsTracked)
{
    UnifiedOrderTracker t;
    initTracker(t);
    t.trackMMOrder(1, 0, CODE, 2663.0, 10, 2663.0, 1000, true);
    ASSERT_TRUE(t.tryMarkPendingCancel(1, CancelReason::MANUAL, 1000));

    // 3 次重试 (1300/1600/1900)
    for (uint64_t now = 1300; now <= 1900; now += 300) {
        const auto& acts = t.checkAutoCancel(CODE, now, 2663.0, 0.2, false);
        ASSERT_EQ(acts.size(), 1u) << "now=" << now;
    }
    // 第 4 次到点 (2200): 置 zombie, 不再产生重试动作
    const auto& acts = t.checkAutoCancel(CODE, 2200, 2663.0, 0.2, false);
    EXPECT_TRUE(acts.empty());

    UnifiedOrderInfo oi;
    ASSERT_TRUE(t.getOrderInfoCopy(1, oi)); // 保留跟踪 (旧实现此处已被 force untrack)
    EXPECT_TRUE(oi.isZombie());
    EXPECT_TRUE(oi.isActive());
    EXPECT_DOUBLE_EQ(t.getPendingBuyQtyAllSources(CODE), 10.0); // 仍计入 pending

    // 升级列表含该合约, 且后续 checkAutoCancel 不重复升级
    ASSERT_EQ(t.getZombieEscalations().size(), 1u);
    EXPECT_EQ(t.getZombieEscalations()[0], CODE);
    t.checkAutoCancel(CODE, 2500, 2663.0, 0.2, false);
    EXPECT_TRUE(t.getZombieEscalations().empty());
}

// clearZombies: untrack zombie 单 + 清升级去重表
TEST(OrderSlotBPlus, ClearZombiesUntracksAndResets)
{
    UnifiedOrderTracker t;
    initTracker(t);
    t.trackMMOrder(1, 0, CODE, 2663.0, 10, 2663.0, 1000, true);
    ASSERT_TRUE(t.tryMarkPendingCancel(1, CancelReason::MANUAL, 1000));
    for (uint64_t now = 1300; now <= 2200; now += 300)
        t.checkAutoCancel(CODE, now, 2663.0, 0.2, false);
    UnifiedOrderInfo oi;
    ASSERT_TRUE(t.getOrderInfoCopy(1, oi));
    ASSERT_TRUE(oi.isZombie());

    t.clearZombies();
    EXPECT_FALSE(t.getOrderInfoCopy(1, oi)); // zombie 单已 untrack
    EXPECT_DOUBLE_EQ(t.getPendingBuyQtyAllSources(CODE), 0.0);

    // 升级去重表已清: 同合约再次 zombie 可重新升级
    t.trackMMOrder(2, 0, CODE, 2663.0, 5, 2663.0, 3000, false);
    ASSERT_TRUE(t.tryMarkPendingCancel(2, CancelReason::MANUAL, 3000));
    // 3 次重试 (3300/3600/3900) + 第 4 次到点 (4200) 置 zombie;
    // 注意不能再用更大时刻调用 checkAutoCancel — sweep 开始会清升级缓冲
    for (uint64_t now = 3300; now <= 4200; now += 300)
        t.checkAutoCancel(CODE, now, 2663.0, 0.2, false);
    ASSERT_EQ(t.getZombieEscalations().size(), 1u);
    EXPECT_EQ(t.getZombieEscalations()[0], CODE);
}

// mark 时不带时刻 (now=0) 的懒赋值兜底路径仍然工作
TEST(OrderSlotBPlus, LazyCancelTimeFallback)
{
    UnifiedOrderTracker t;
    initTracker(t);
    t.trackMMOrder(1, 0, CODE, 2663.0, 10, 2663.0, 1000, true);
    ASSERT_TRUE(t.tryMarkPendingCancel(1, CancelReason::MANUAL, 0)); // 无时刻

    // 首次观察 (t=1000) 懒赋值, 不产生动作
    EXPECT_TRUE(t.checkAutoCancel(CODE, 1000, 2663.0, 0.2, false).empty());
    // 间隔后 (1300) 正常重试
    const auto& acts = t.checkAutoCancel(CODE, 1300, 2663.0, 0.2, false);
    ASSERT_EQ(acts.size(), 1u);
    EXPECT_EQ(acts[0].reason, CancelReason::TIMEOUT);
}

// B+ 修复(P1-2): clearZombies 返回被 untrack 的 zombie id 列表 (调用方据此
// 引擎侧补发全撤 + 清孤儿槽); 无 zombie 时返回空
TEST(OrderSlotBPlus, ClearZombiesReturnsUntrackedIds)
{
    UnifiedOrderTracker t;
    initTracker(t);
    t.trackMMOrder(1, 0, CODE, 2663.0, 10, 2663.0, 1000, true);
    t.trackMMOrder(2, 0, CODE, 2663.0, 5, 2663.0, 1000, false);
    ASSERT_TRUE(t.tryMarkPendingCancel(1, CancelReason::MANUAL, 1000));
    ASSERT_TRUE(t.tryMarkPendingCancel(2, CancelReason::MANUAL, 1000));
    for (uint64_t now = 1300; now <= 2200; now += 300)
        t.checkAutoCancel(CODE, now, 2663.0, 0.2, false);

    auto ids = t.clearZombies();
    ASSERT_EQ(ids.size(), 2u); // 两单均 zombie, 全部返回
    EXPECT_NE(std::find(ids.begin(), ids.end(), 1u), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), 2u), ids.end());
    // 清空后再调返回空
    EXPECT_TRUE(t.clearZombies().empty());
}

// B+ 修复(P2-3): zombie 清零后升级去重重置 -- 同合约新 zombie 可重新升级;
// 存活 zombie 合约集合随 checkAutoCancel 维护
TEST(OrderSlotBPlus, EscalationRearmsAfterZombieGone)
{
    UnifiedOrderTracker t;
    initTracker(t);
    t.trackMMOrder(1, 0, CODE, 2663.0, 10, 2663.0, 1000, true);
    ASSERT_TRUE(t.tryMarkPendingCancel(1, CancelReason::MANUAL, 1000));
    for (uint64_t now = 1300; now <= 2200; now += 300)
        t.checkAutoCancel(CODE, now, 2663.0, 0.2, false);
    ASSERT_EQ(t.getZombieEscalations().size(), 1u);
    ASSERT_EQ(t.getAliveZombieContracts().size(), 1u);
    EXPECT_EQ(t.getAliveZombieContracts()[0], CODE);

    // 兜底 cancelAll 杀掉 zombie, Cncld 回报清账 -> 存活集合清空
    t.untrackOrder(1, 2500);
    t.checkAutoCancel(CODE, 2600, 2663.0, 0.2, false);
    EXPECT_TRUE(t.getAliveZombieContracts().empty());

    // 同合约新单再度 zombie -> 重新升级 (去重已重置, 不再被吞)
    t.trackMMOrder(2, 0, CODE, 2663.0, 5, 2663.0, 3000, false);
    ASSERT_TRUE(t.tryMarkPendingCancel(2, CancelReason::MANUAL, 3000));
    for (uint64_t now = 3300; now <= 4200; now += 300)
        t.checkAutoCancel(CODE, now, 2663.0, 0.2, false);
    ASSERT_EQ(t.getZombieEscalations().size(), 1u);
    EXPECT_EQ(t.getZombieEscalations()[0], CODE);
    ASSERT_EQ(t.getAliveZombieContracts().size(), 1u);
}

// B+ 修复(P2-3): zombie halt 闩锁的释放与重置 (FutuRiskMonitor)
TEST(OrderSlotBPlus, ZombieHaltLatchReleaseAndRearm)
{
    FutuRiskMonitor rm;
    ContractState cs;
    cs.code = CODE;
    cs.position = 0;
    cs.hedge_ratio = 1.0;
    cs.max_position = 50;
    cs.contract_max_delta = 30;

    EXPECT_FALSE(rm.checkPreTradePosition(cs, nullptr, 0).risk.halt_quoting);
    rm.setZombieHalt(CODE);
    EXPECT_TRUE(rm.checkPreTradePosition(cs, nullptr, 0).risk.halt_quoting);

    // 存活集合不含 CODE -> 闩锁释放 (zombie 已被兜底杀掉清账)
    std::vector<std::string> alive_other = {"SHFE.ag.ag2608"};
    rm.retainZombieHalts(alive_other);
    EXPECT_FALSE(rm.checkPreTradePosition(cs, nullptr, 0).risk.halt_quoting);

    // 重新置位后 clearZombieHalts 全清 (通道恢复锚点)
    rm.setZombieHalt(CODE);
    rm.clearZombieHalts();
    EXPECT_FALSE(rm.checkPreTradePosition(cs, nullptr, 0).risk.halt_quoting);
}
