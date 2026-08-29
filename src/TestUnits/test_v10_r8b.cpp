/*!
 * \file test_v10_r8b.cpp
 * \brief V10-R8b 集合竞价撮合分钟挂单门回归测试
 *
 * 根因: 框架 WTSSessionInfo::isInTradingTime 对集合竞价分钟按设计返回 true
 * (timeToMinutes 对 isInAuctionTime 直接返回 0, 用于行情/状态机对齐), 但交易所
 * 撮合分钟不接受连续竞价报单 → 08-28 盘前 20:59 一分钟 44,304 单 (738/s) 风暴。
 *
 * 修复语义 (preCheck + requoteAfterFill 统一): 可挂单 = isInTradingTime(hhmm)
 * && !isInAuctionTime(hhmm)。
 *
 * 本测试锁定真实 session 模板语义 (远程 sessions.json 实测配置复刻):
 *   - FN0100 (ao 腿): offset=300, auction 2059-2100, sections 2100-0100/白盘三段
 *   - FD0900 (ec 锚): offset=0,   auction 0859-0900, 白盘三段
 */
#include "../Includes/WTSSessionInfo.hpp"
#include "gtest/gtest.h"

using namespace wtp;

namespace
{

/// 复刻 preCheck/isOrderSubmissionAllowed 的挂单面语义 (单一表达式, 语义锁定)
bool orderable(wtp::WTSSessionInfo* s, uint32_t hhmm)
{
    return s->isInTradingTime(hhmm) && !s->isInAuctionTime(hhmm);
}

wtp::WTSSessionInfo* makeFN0100()
{
    auto* s = WTSSessionInfo::create("FN0100", "", 300);
    s->addTradingSection(2100, 100);
    s->addTradingSection(900, 1015);
    s->addTradingSection(1030, 1130);
    s->addTradingSection(1330, 1500);
    s->setAuctionTime(2059, 2100);
    return s;
}

wtp::WTSSessionInfo* makeFD0900()
{
    auto* s = WTSSessionInfo::create("FD0900", "", 0);
    s->addTradingSection(900, 1015);
    s->addTradingSection(1030, 1130);
    s->addTradingSection(1330, 1500);
    s->setAuctionTime(859, 900);
    return s;
}

} // namespace

TEST(test_v10_r8b, FN0100_AuctionMatchMinuteNotOrderable)
{
    WTSSessionInfo* s = makeFN0100();
    // 20:59 = 夜盘集合竞价撮合分钟: 框架 isInTradingTime=true (设计如此),
    // 但挂单面必须为 false —— 08-28 44,304 单风暴根因
    EXPECT_TRUE(s->isInTradingTime(2059));
    EXPECT_TRUE(s->isInAuctionTime(2059));
    EXPECT_FALSE(orderable(s, 2059));
    // 20:58: 既非交易段也非竞价段 (本模板竞价=撮合分钟 2059)
    EXPECT_FALSE(orderable(s, 2058));
    // 21:00: 连续竞价开始, 可挂单
    EXPECT_TRUE(orderable(s, 2100));
    // 夜盘 2100-0100 段内午夜时刻
    EXPECT_TRUE(orderable(s, 0));
    // 夜盘收盘 01:00 (raw second): 交易时间边缘 (timeToMinutes 对 section.second 含边界)
    EXPECT_TRUE(s->isInTradingTime(100));
}

TEST(test_v10_r8b, FD0900_DayAuctionMinuteNotOrderable)
{
    WTSSessionInfo* s = makeFD0900();
    // 08:59 = 白盘集合竞价撮合分钟 (同构暴露面, 08-28 未触发仅因无 tick 流入)
    EXPECT_TRUE(s->isInAuctionTime(859));
    EXPECT_FALSE(orderable(s, 859));
    EXPECT_TRUE(orderable(s, 900));
    // 小节休市窗口 (11:30-13:30): 不可挂单 —— requoteAfterFill 同门顺带封堵的存量面
    EXPECT_FALSE(orderable(s, 1135));
    EXPECT_FALSE(orderable(s, 1300));
}

TEST(test_v10_r8b, TradingTimeBoundarySemanticsUnchanged)
{
    // 防退化: 修复只收窄竞价撮合分钟, 正常交易时段判定不受影响
    WTSSessionInfo* s = makeFN0100();
    EXPECT_TRUE(orderable(s, 1000));  // 日盘上午段
    EXPECT_TRUE(orderable(s, 2200));  // 夜盘段
    EXPECT_FALSE(orderable(s, 1020)); // 10:15-10:30 小节休市
}
