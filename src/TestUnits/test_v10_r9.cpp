
/**
 * @file test_v10_r9.cpp
 * @brief V10-R9 回归锁定 —— 2026-09-01 10:15-10:30 休市回调风暴 + 幽灵单闩锁焊死
 *
 * 根因链 (详见 src/WtFutuCore/AGENTS.md R9 立案):
 *   1. 休市期 SHFE 拒绝撤单(err 26) → 撤拒被当终态 → 事件驱动补挂 → 新单再被拒
 *      → 回调速度指数正反馈 (12,411 单 / 峰值 430/s)
 *   2. 会话门时钟取 ctx->stra_get_time() (tick 钟, 休市冻结在 1015) → 休市全程放行
 *   3. 节流谓词 `now_ms > last &&` 在等值时钟下恒假 → 200ms 节流被静默绕过
 *   4. checkRateLimits 零调用者 (死代码) → ORDER_RATE 永不触发
 *
 * 修复语义锁定:
 *   - isOrderSubmissionAllowed: bStrict 排除每节末分钟 (实盘墙钟注入由
 *     StrategyCoordinator::setLiveWallclock 承载, 本文件锁模板语义)
 *   - requoteThrottleAllow: 单向窗口, 等值/冻结/回拨时钟均拦截
 *   - checkRateLimits: 超限 false, 窗口修剪后恢复
 */
#include <gtest/gtest.h>
#include "../WtFutuCore/StrategyCoordinator.h"
#include "../WtFutuCore/FutuRiskMonitor.h"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Share/TimeUtils.hpp"

namespace
{

/// 复刻 isOrderSubmissionAllowed 的 R9 语义 (bStrict + 竞价排除)
bool orderableStrict(wtp::WTSSessionInfo* s, uint32_t hhmm)
{
    return s->isInTradingTime(hhmm, true) && !s->isInAuctionTime(hhmm);
}

wtp::WTSSessionInfo* makeFN0100()
{
    auto* s = wtp::WTSSessionInfo::create("FN0100", "", 300);
    s->addTradingSection(2100, 100);
    s->addTradingSection(900, 1015);
    s->addTradingSection(1030, 1130);
    s->addTradingSection(1330, 1500);
    s->setAuctionTime(2059, 2100);
    return s;
}

} // namespace

//============================================================================
// 时段门: bStrict 秒级封死休市边界 (R9-D3)
//============================================================================

// 每节末分钟整分钟排除 (10:15:00-59 的 HHMM==1015, 分钟级判定曾误判为交易中)
TEST(test_v10_r9, StrictBoundary_ExcludesSectionEndMinute)
{
    wtp::WTSSessionInfo* s = makeFN0100();
    EXPECT_FALSE(orderableStrict(s, 1015));  // 上午一节收盘分钟 (风暴窗口)
    EXPECT_FALSE(orderableStrict(s, 1130));  // 上午二节收盘分钟
    EXPECT_FALSE(orderableStrict(s, 1500));  // 白盘收盘分钟
}

// 收盘前最后一个真实交易分钟不受影响 (零交易时间损失)
TEST(test_v10_r9, StrictBoundary_KeepsLastRealTradingMinute)
{
    wtp::WTSSessionInfo* s = makeFN0100();
    EXPECT_TRUE(orderableStrict(s, 1014));
    EXPECT_TRUE(orderableStrict(s, 1129));
    EXPECT_TRUE(orderableStrict(s, 1459));
}

// 小节休市中段与开节分钟维持既有语义
TEST(test_v10_r9, StrictBoundary_BreakAndSectionStart)
{
    wtp::WTSSessionInfo* s = makeFN0100();
    EXPECT_FALSE(orderableStrict(s, 1020)); // 10:15-10:30 休市中段
    EXPECT_TRUE(orderableStrict(s, 1125));  // 11:25 仍交易 (午休为 1130-1330)
    EXPECT_TRUE(orderableStrict(s, 900));   // 开节分钟可交易
    EXPECT_TRUE(orderableStrict(s, 1030));
    EXPECT_TRUE(orderableStrict(s, 2100));  // 夜盘开盘
}

//============================================================================
// 补挂限频: 单向窗口谓词 (R9-D2)
//============================================================================

// 等值/冻结时钟必须拦截 —— 2026-09-01 风暴放大器 (旧判定恒放行)
TEST(test_v10_r9, Throttle_EqualFrozenClockBlocked)
{
    uint64_t last = 1000;
    EXPECT_FALSE(futu::StrategyCoordinator::requoteThrottleAllow(last, 1000, 200));
    EXPECT_EQ(last, 1000u); // 拦截时不得刷新 last
}

TEST(test_v10_r9, Throttle_WindowSemantics)
{
    uint64_t last = 0;
    // 首调用放行并记录
    EXPECT_TRUE(futu::StrategyCoordinator::requoteThrottleAllow(last, 1000, 200));
    EXPECT_EQ(last, 1000u);
    // 窗口内拦截
    EXPECT_FALSE(futu::StrategyCoordinator::requoteThrottleAllow(last, 1100, 200));
    // 窗口外放行
    EXPECT_TRUE(futu::StrategyCoordinator::requoteThrottleAllow(last, 1200, 200));
    EXPECT_EQ(last, 1200u);
}

// 时钟回拨 (NTP 校正) 保守拦截, 防止负窗口放行
TEST(test_v10_r9, Throttle_ClockBackwardBlocked)
{
    uint64_t last = 5000;
    EXPECT_FALSE(futu::StrategyCoordinator::requoteThrottleAllow(last, 4000, 200));
    EXPECT_EQ(last, 5000u);
}

// interval=0 → 禁用 (恒放行, 兼容既有配置语义)
TEST(test_v10_r9, Throttle_ZeroIntervalDisabled)
{
    uint64_t last = 1000;
    EXPECT_TRUE(futu::StrategyCoordinator::requoteThrottleAllow(last, 1000, 0));
    EXPECT_TRUE(futu::StrategyCoordinator::requoteThrottleAllow(last, 1005, 0));
}

//============================================================================
// 频控熔断接线 (R9-D5): checkRateLimits 超限 false + 窗口修剪后恢复
//============================================================================

TEST(test_v10_r9, RateLimits_OverLimitBlocksAndRecovers)
{
    futu::FutuRiskMonitor rm;
    futu::RiskRateLimits limits;
    limits.max_orders_per_sec = 50;
    limits.max_cancels_per_sec = 30;
    limits.max_trades_per_sec = 20;
    rm.setRateLimits(limits);

    uint64_t t0 = TimeUtils::getLocalTimeNow();
    rm.setCurrentTime(t0);

    EXPECT_TRUE(rm.checkRateLimits()); // 空窗口放行

    rm.recordOrders(60); // 超过 max_orders_per_sec=50
    EXPECT_FALSE(rm.checkRateLimits());

    // 窗口修剪后恢复 (模拟时间前进, 如复牌后 tick 恢复注入)
    rm.setCurrentTime(t0 + 2000);
    EXPECT_TRUE(rm.checkRateLimits());
}

TEST(test_v10_r9, RateLimits_CancelAndTradeWindowsEnforced)
{
    futu::FutuRiskMonitor rm;
    futu::RiskRateLimits limits;
    rm.setRateLimits(limits); // 默认 50/30/20

    uint64_t t0 = TimeUtils::getLocalTimeNow();
    rm.setCurrentTime(t0);

    for (uint32_t i = 0; i < 35; ++i)
        rm.recordCancel();
    EXPECT_FALSE(rm.checkRateLimits()); // cancels 35 > 30

    rm.setCurrentTime(t0 + 2000);
    EXPECT_TRUE(rm.checkRateLimits());
}
