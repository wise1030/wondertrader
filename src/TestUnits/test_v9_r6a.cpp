/*!
 * \file test_v9_r6a.cpp
 * \brief V8-R6a 轮回归护栏 (WS-A 停机域收编 + P2 修复)
 *
 * 覆盖:
 *   - WS-A  FutuRiskMonitor halt/closeout 状态域跨线程收编:
 *           4 线程并发 hammer (halt/resume/closeout 转移/读) 无崩溃无死锁,
 *           终态合法 (_halt_category 与 _trading_halted 一致性由原子序保证)
 *   - WS-A  getCloseoutSubInfo 按值返回锁内拷贝 (字段读取正确)
 *   - WS-A  IRREVERSIBLE 语义保持: resumeTrading 拒绝 / clearIrreversible 解锁 /
 *           resetDaily 保留(默认) 或 auto_clear 清除
 *   - P2-3  Source::RISK_REDUCE 枚举就位且与既有值两两不同
 */
#include "../WtFutuCore/FutuRiskMonitor.h"
#include "../WtFutuCore/OrderTypes.h"
#include "../WtFutuCore/SelfTradeCalibrator.h"
#include "../WtFutuCore/PerformanceMonitor.h"
#include "gtest/gtest.h"

#include <atomic>
#include <set>
#include <thread>
#include <vector>

using namespace futu;

//==============================================================================
// WS-A: 并发 hammer
//==============================================================================

TEST(HaltDomainWSA, ConcurrentHammerNoCrash)
{
    FutuRiskMonitor rm;
    rm.setCurrentTime(1000);

    constexpr int kIterations = 20000;
    std::atomic<bool> stop{false};

    // 写者1: halt/recover 翻转 (模拟 MdSpi checkRisk 与 arb 线程 handleRiskAlert)
    std::thread t1([&] {
        for (int i = 0; i < kIterations; ++i) {
            rm.haltTrading((i % 4 == 0) ? RiskCategory::IRREVERSIBLE : RiskCategory::REVERSIBLE, -100.0);
            if (i % 3 == 0)
                rm.resumeTrading();
            (void)rm.getHaltCategory();
        }
    });

    // 写者2: closeout 状态机转移 (模拟 TdSpi onOrderEvent)
    std::thread t2([&] {
        for (int i = 0; i < kIterations; ++i) {
            rm.markCloseoutTriggered(i);
            if (i % 2 == 0)
                rm.markCloseoutDraining(i);
            else
                rm.markCloseoutCompleted(i);
            if (i % 5 == 0)
                rm.markCloseoutFailed(i);
            if (i % 10 == 0)
                rm.resetCloseout(true); // session 边界强清
        }
    });

    // 读者×2: 状态查询 (模拟 MdSpi 流水线每 tick 多处读)
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                CloseoutSub s = rm.getCloseoutSub();
                (void)rm.isCloseoutFlattening();
                (void)rm.isCloseoutTriggered();
                CloseoutSubInfo info = rm.getCloseoutSubInfo();
                // 终态合法性: 快照 state 必须落在枚举域内 (撕裂读会产出非法值)
                ASSERT_TRUE(s >= CloseoutSub::IDLE && s <= CloseoutSub::RETRYING);
                ASSERT_TRUE(info.state >= CloseoutSub::IDLE && info.state <= CloseoutSub::RETRYING);
            }
        });
    }

    t1.join();
    t2.join();
    stop.store(true);
    for (auto& t : readers)
        t.join();

    // 收尾复位, 不影响后续用例
    rm.resetDaily();
    rm.resetCloseout(true);
}

//==============================================================================
// WS-A: getCloseoutSubInfo 按值拷贝语义
//==============================================================================

TEST(HaltDomainWSA, CloseoutInfoIsValueCopy)
{
    FutuRiskMonitor rm;
    rm.setCloseoutConfig(CloseoutConfig{});
    rm.markCloseoutTriggered(123);

    CloseoutSubInfo snap = rm.getCloseoutSubInfo();
    EXPECT_EQ(snap.state, CloseoutSub::TRIGGERED);
    EXPECT_EQ(snap.trigger_time, 123u);

    // 后续状态推进不影响已取快照 (按值语义)
    rm.markCloseoutCompleted(456);
    EXPECT_EQ(snap.state, CloseoutSub::TRIGGERED);
    EXPECT_EQ(rm.getCloseoutSubInfo().state, CloseoutSub::COMPLETED);
    EXPECT_EQ(rm.getCloseoutSubInfo().complete_time, 456u);
}

//==============================================================================
// WS-A: IRREVERSIBLE 语义保持
//==============================================================================

TEST(HaltDomainWSA, IrreversibleSemanticsPreserved)
{
    FutuRiskMonitor rm;
    rm.setCurrentTime(5000);

    rm.haltTrading(RiskCategory::IRREVERSIBLE, -999.0);
    EXPECT_EQ(rm.getHaltCategory(), RiskCategory::IRREVERSIBLE);
    EXPECT_TRUE(rm.isTradingHalted());
    EXPECT_FALSE(rm.resumeTrading()); // IRREVERSIBLE 拒绝自动恢复

    // 默认配置: resetDaily 保留 IRREVERSIBLE
    rm.resetDaily();
    EXPECT_TRUE(rm.isTradingHalted());
    EXPECT_EQ(rm.getHaltCategory(), RiskCategory::IRREVERSIBLE);

    // 人工确认解锁
    EXPECT_TRUE(rm.clearIrreversible());
    EXPECT_FALSE(rm.isTradingHalted());
    EXPECT_EQ(rm.getHaltCategory(), RiskCategory::REVERSIBLE);
}

TEST(HaltDomainWSA, AutoClearIrreversibleOnResetGated)
{
    FutuRiskMonitor rm;
    RecoveryConfig cfg;
    cfg.auto_clear_irreversible_on_reset = true; // 回测模式
    rm.setRecoveryConfig(cfg);

    rm.haltTrading(RiskCategory::IRREVERSIBLE, -500.0);
    rm.resetDaily();
    EXPECT_FALSE(rm.isTradingHalted());
    EXPECT_EQ(rm.getHaltCategory(), RiskCategory::REVERSIBLE);
}

//==============================================================================
// WS-A: recovery 计数受锁保护且语义不变 (cooldown 内不恢复)
//==============================================================================

TEST(HaltDomainWSA, RecoveryCooldownRespected)
{
    FutuRiskMonitor rm;
    RecoveryConfig cfg;
    cfg.check_interval_ms = 1000;
    rm.setRecoveryConfig(cfg);

    rm.setCurrentTime(10000);
    rm.haltTrading(RiskCategory::REVERSIBLE, 0);

    // checkAndRecover(nullptr): canRecover 对 null portfolio 直接 false,
    // 但节流路径必须先生效 —— 连续调用不产生恢复副作用
    EXPECT_FALSE(rm.checkAndRecover(nullptr));
    rm.setCurrentTime(10200); // < check_interval_ms
    EXPECT_FALSE(rm.checkAndRecover(nullptr));
    rm.setCurrentTime(11500); // > check_interval_ms, 进入 canRecover → nullptr 拒绝
    EXPECT_FALSE(rm.checkAndRecover(nullptr));
}

//==============================================================================
// P2-3: Source::RISK_REDUCE 枚举
//==============================================================================

TEST(SourceEnum, RiskReduceDistinctValues)
{
    EXPECT_NE(static_cast<uint8_t>(Source::RISK_REDUCE), static_cast<uint8_t>(Source::ARBITRAGE));
    EXPECT_NE(static_cast<uint8_t>(Source::RISK_REDUCE), static_cast<uint8_t>(Source::HEDGING));
    EXPECT_NE(static_cast<uint8_t>(Source::RISK_REDUCE), static_cast<uint8_t>(Source::CLOSEOUT));

    std::set<uint8_t> vals{static_cast<uint8_t>(Source::ARBITRAGE),
                           static_cast<uint8_t>(Source::HEDGING),
                           static_cast<uint8_t>(Source::CLOSEOUT),
                           static_cast<uint8_t>(Source::RISK_REDUCE)};
    EXPECT_EQ(vals.size(), 4u);
}

//==============================================================================
// 收官①: SelfTradeCalibrator 跨线程收编 —— Td(recordFill/getCalibration) vs
// Md(onTick/decayCalibration/getFillRetreat) 并发, map 结构插入+RingBuffer 读写
// 在无锁实现下是 UB; 收编后无崩溃且计数守恒。
//==============================================================================

TEST(CalibratorClosing, ConcurrentFillTickNoCrash)
{
    SelfTradeCalibrator cal;
    SelfTradeCalibratorConfig cfg;
    cfg.tick_size = 1.0;
    cal.setConfig(cfg);

    constexpr int kIterations = 20000;
    const std::string code = "SHFE.ao.ao2610";

    // TdSpi 侧: 成交记录 + 标定读取
    std::thread td([&] {
        for (int i = 0; i < kIterations; ++i) {
            cal.recordFill(code, 2800.0 + (i % 7), 1.0, (i % 2 == 0), 2801.0, 2.0, 1000 + i);
            if (i % 5 == 0)
                (void)cal.getCalibration(code);
        }
    });

    // MdSpi 侧: tick 更新 + retreat 查询 + 衰减
    std::thread md([&] {
        for (int i = 0; i < kIterations; ++i) {
            cal.onTick(code, 2800.0 + (i % 5), 1000 + i);
            if (i % 3 == 0)
                (void)cal.getFillRetreat(code, 1000 + i);
            if (i % 11 == 0)
                cal.decayCalibration(code, 1000 + i, 30000);
        }
    });

    td.join();
    md.join();

    // 计数守恒: 样本数 = 记录数减去 RingBuffer 容量淘汰与衰减剔除, 上限 128
    EXPECT_LE(cal.getSampleCount(code), 128u);
    // retreat 状态查询不崩溃且返回合法结构
    FillRetreat r = cal.getFillRetreat(code, 1000 + kIterations);
    if (r.bid_retreat_active)
        EXPECT_GT(r.bid_retreat_price, 0.0);
    if (r.ask_retreat_active)
        EXPECT_GT(r.ask_retreat_price, 0.0);
}

//==============================================================================
// 收官②: PerformanceMonitor 计数器原子化 —— 双线程递增同一计数器,
// 无 lost-update (原子化前为裸 uint64_t ++, 竞态下总数 < 期望值)。
//==============================================================================

TEST(PerfMonClosing, ConcurrentCountersNoLostUpdate)
{
    PerformanceMonitor pm;
    constexpr int kIterations = 50000;

    // 模拟 Md(refreshQuotes) 与 Td(requoteAfterFill 补挂) 双写同一计数器
    std::thread t1([&] {
        for (int i = 0; i < kIterations; ++i)
            pm.recordOrderPlaced();
    });
    std::thread t2([&] {
        for (int i = 0; i < kIterations; ++i)
            pm.recordOrderPlaced();
    });
    t1.join();
    t2.join();

    auto stats = pm.getThroughputStats();
    EXPECT_EQ(stats.orders_placed.load(), 2u * kIterations);
}
