/*!
 * \file test_trade_id_dedup.cpp
 * \brief 2026-10-18: 实时成交去重 key 语义（交易日|交易所|TradeID）。
 */
#include "../WtUftCore/TradeIdDedup.h"
#include "gtest/gtest.h"

using namespace wtp;

TEST(TradeIdDedup, EmptyIdIsNotDeduplicated)
{
    TradeIdDedup d;
    EXPECT_FALSE(d.seen(""));
    EXPECT_EQ(d.size(), 0u);
}

TEST(TradeIdDedup, SameKeyIsDuplicate)
{
    TradeIdDedup d;
    const std::string k = TradeIdDedup::makeKey(20261014, "SHFE", "12345");
    EXPECT_FALSE(d.seen(k));
    EXPECT_TRUE(d.seen(k));
    EXPECT_EQ(d.size(), 1u);
}

TEST(TradeIdDedup, ExchangeIsPartOfKey)
{
    // CTP TradeID is exchange-scoped; SHFE/INE must not collide.
    const std::string sh = TradeIdDedup::makeKey(20261014, "SHFE", "12345");
    const std::string in = TradeIdDedup::makeKey(20261014, "INE", "12345");
    EXPECT_NE(sh, in);
    TradeIdDedup d;
    EXPECT_FALSE(d.seen(sh));
    EXPECT_FALSE(d.seen(in));
    EXPECT_EQ(d.size(), 2u);
}

TEST(TradeIdDedup, DateIsPartOfKeyAndClear)
{
    const std::string d1 = TradeIdDedup::makeKey(20261014, "SHFE", "12345");
    const std::string d2 = TradeIdDedup::makeKey(20261015, "SHFE", "12345");
    EXPECT_NE(d1, d2);
    TradeIdDedup d;
    EXPECT_FALSE(d.seen(d1));
    EXPECT_TRUE(d.seen(d1));
    d.clear();
    EXPECT_EQ(d.size(), 0u);
    EXPECT_FALSE(d.seen(d1));
}

TEST(TradeIdDedup, EmptyTradeIdProducesNoKey)
{
    EXPECT_TRUE(TradeIdDedup::makeKey(20261014, "SHFE", nullptr).empty());
    EXPECT_TRUE(TradeIdDedup::makeKey(20261014, "SHFE", "").empty());
}
