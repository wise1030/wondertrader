/*!
 * \file test_v8_r1_p0.cpp
 * \brief V8-P0-2 (MM 频控计数) 与 P0-4 (MarketDataContext 装配) unit tests
 *
 * P0-2: 此前 recordOrder 仅 taker/arb 调用, MM 报单 (最高频来源) 零计数;
 *       频控时间环 LockFreeRingBuffer 为 SPSC, 多生产者 (MdSpi/arb/TdSpi)
 *       必须以自旋锁串行化。recordOrders 为 MM 批量计数入口。
 *
 * P0-4: createMarketDataContext 此前忽略参数 (tick_size 恒默认 0.2,
 *       EC 实际 0.5 -> depth_imbalance 系统性偏差 2.5 倍)。
 */
#include "../WtFutuCore/FutuRiskMonitor.h"
#include "../WtFutuCore/FutuComponentFactory.h"
#include "../WtFutuCore/StrategyCoordinator.h"
#include "../WtFutuCore/signals/MarketDataContext.h"
#include "gtest/gtest.h"

#include "../WTSUtils/WTSCfgLoader.h"

#include <fstream>
#include <thread>
#include <vector>

using namespace futu;

namespace
{

std::string writeTmpYaml(const char* content)
{
    static int seq = 0;
    std::string path = "/tmp/opencode/p0_test_" + std::to_string(++seq) + ".yaml";
    std::ofstream f(path);
    f << content;
    return path;
}

} // namespace

//------------------------------------------------------------------------------
// P0-2: recordOrders 批量计数 + 时间窗剪裁
//------------------------------------------------------------------------------

TEST(RateOrderCount, RecordOrdersBatchCounts)
{
    FutuRiskMonitor m;
    m.setCurrentTime(100000);
    m.recordOrders(3);
    EXPECT_EQ(m.getOrdersPerSec(), 3u);

    // 0 笔不计数
    m.recordOrders(0);
    EXPECT_EQ(m.getOrdersPerSec(), 3u);
}

TEST(RateOrderCount, WindowPrunesExpiredEntries)
{
    FutuRiskMonitor m;
    m.setCurrentTime(100000);
    m.recordOrders(3);
    m.setCurrentTime(102000); // 2s 后: 3 笔全部过期
    m.recordOrders(1);
    EXPECT_EQ(m.getOrdersPerSec(), 1u);
}

TEST(RateOrderCount, ConcurrentRecordIsExact)
{
    // 多生产者并发计数: 加锁后应精确 (无锁 SPSC 会丢/重, 本用例即回归护栏)
    FutuRiskMonitor m;
    m.setCurrentTime(100000);
    m.recordOrders(1);

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++)
        threads.emplace_back([&m]() {
            for (int i = 0; i < 50; i++)
                m.recordOrder();
        });
    for (auto& th : threads)
        th.join();

    EXPECT_EQ(m.getOrdersPerSec(), 201u); // 1 + 4*50, 环容量 256 内
}

//------------------------------------------------------------------------------
// P0-4: MarketDataContext 合约装配
//------------------------------------------------------------------------------

TEST(MarketDataWiring, FactoryWiresContractTickSize)
{
    std::string yaml = writeTmpYaml(
        "modules:\n"
        "  signalAggregator:\n"
        "    signals:\n"
        "      trade_flow:\n"
        "        largeTradeThreshold: 30.0\n");

    CoordinatorConfig cfg;
    cfg._raw_variant = WTSCfgLoader::load_from_file(yaml.c_str());
    ASSERT_NE(cfg._raw_variant, nullptr);

    auto ctx = FutuComponentFactory::createMarketDataContext(cfg, "SHFE.ec.ec2509", 0.5);
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(ctx->getCode(), "SHFE.ec.ec2509");
    EXPECT_DOUBLE_EQ(ctx->getTickSize(), 0.5); // 此前恒为默认 0.2

    cfg._raw_variant->release();
}

TEST(MarketDataWiring, FactorySkipsInvalidTickSize)
{
    CoordinatorConfig cfg; // _raw_variant = nullptr -> 阈值走默认

    auto ctx = FutuComponentFactory::createMarketDataContext(cfg, "SHFE.ec.ec2509", 0.0);
    ASSERT_NE(ctx, nullptr);
    EXPECT_TRUE(ctx->getCode().empty()); // 未装配 -> 首帧 onTick 亦有兜底告警
}

TEST(MarketDataWiring, SetContractTickSizeDrivesSpreadTicks)
{
    MarketDataContext ctx;
    EXPECT_DOUBLE_EQ(ctx.getTickSize(), 0.2); // 未装配默认值
    ctx.setContract("SHFE.ec.ec2509", 0.5);
    EXPECT_DOUBLE_EQ(ctx.getTickSize(), 0.5);
}
