/*!
 * \file test_order_sink.cpp
 * \brief B6: IOrderSink narrow interface + MockOrderSink unit tests
 *
 * Verifies that IOrderSink (4-method order execution interface) can be
 * mocked without the full IUftStraCtx/OrderRouter stack. This unlocks
 * unit testing for RiskCoordinator, RiskLiquidator, and future consumers.
 */
#include "../WtFutuCore/IOrderSink.h"
#include "gtest/gtest/gtest.h"
#include <string>
#include <vector>

using namespace futu;

//==============================================================================
// MockOrderSink: records all order operations for test verification
//==============================================================================
class MockOrderSink : public IOrderSink
{
public:
    struct RecordedOrder
    {
        bool is_buy;
        std::string code;
        double price;
        double qty;
        Source src;
        int flag;
    };

    std::vector<RecordedOrder> orders;
    std::vector<Source> cancel_all_sources;
    std::vector<std::string> cancel_pair_ids;

    OrderSubmitResult submitBuy(wtp::IUftStraCtx* /*ctx*/, const char* code,
                                double price, double qty, Source src,
                                int flag = 0) override
    {
        orders.push_back({true, code ? code : "", price, qty, src, flag});
        OrderSubmitResult r;
        r.localids.push_back(static_cast<uint32_t>(1000 + orders.size()));
        return r;
    }

    OrderSubmitResult submitSell(wtp::IUftStraCtx* /*ctx*/, const char* code,
                                 double price, double qty, Source src,
                                 int flag = 0) override
    {
        orders.push_back({false, code ? code : "", price, qty, src, flag});
        OrderSubmitResult r;
        r.localids.push_back(static_cast<uint32_t>(2000 + orders.size()));
        return r;
    }

    void cancelAllBySource(wtp::IUftStraCtx* /*ctx*/, Source src) override
    {
        cancel_all_sources.push_back(src);
    }

    size_t cancelByPair(wtp::IUftStraCtx* /*ctx*/, const std::string& pair_id) override
    {
        cancel_pair_ids.push_back(pair_id);
        return 1;
    }
};

//==============================================================================
// Tests
//==============================================================================

TEST(test_order_sink, mock_records_submit_buy)
{
    MockOrderSink sink;
    auto r = sink.submitBuy(nullptr, "CFFEX.IF.2503", 3800.0, 5, Source::CLOSEOUT, 1);
    ASSERT_EQ(sink.orders.size(), 1u);
    EXPECT_TRUE(sink.orders[0].is_buy);
    EXPECT_EQ(sink.orders[0].code, "CFFEX.IF.2503");
    EXPECT_DOUBLE_EQ(sink.orders[0].price, 3800.0);
    EXPECT_DOUBLE_EQ(sink.orders[0].qty, 5.0);
    EXPECT_EQ(sink.orders[0].src, Source::CLOSEOUT);
    EXPECT_EQ(sink.orders[0].flag, 1);
    EXPECT_FALSE(r.localids.empty());
    EXPECT_EQ(r.localids[0], 1001u);
}

TEST(test_order_sink, mock_records_submit_sell)
{
    MockOrderSink sink;
    auto r = sink.submitSell(nullptr, "CFFEX.IF.2503", 3810.0, 3, Source::HEDGING);
    ASSERT_EQ(sink.orders.size(), 1u);
    EXPECT_FALSE(sink.orders[0].is_buy);
    EXPECT_DOUBLE_EQ(sink.orders[0].qty, 3.0);
    EXPECT_EQ(sink.orders[0].src, Source::HEDGING);
    EXPECT_EQ(r.localids[0], 2001u);
}

TEST(test_order_sink, mock_records_cancel_all_by_source)
{
    MockOrderSink sink;
    sink.cancelAllBySource(nullptr, Source::ARBITRAGE);
    sink.cancelAllBySource(nullptr, Source::CLOSEOUT);
    sink.cancelAllBySource(nullptr, Source::HEDGING);
    ASSERT_EQ(sink.cancel_all_sources.size(), 3u);
    EXPECT_EQ(sink.cancel_all_sources[0], Source::ARBITRAGE);
    EXPECT_EQ(sink.cancel_all_sources[1], Source::CLOSEOUT);
    EXPECT_EQ(sink.cancel_all_sources[2], Source::HEDGING);
}

TEST(test_order_sink, mock_records_cancel_by_pair)
{
    MockOrderSink sink;
    size_t n = sink.cancelByPair(nullptr, "pair_001");
    EXPECT_EQ(n, 1u);
    ASSERT_EQ(sink.cancel_pair_ids.size(), 1u);
    EXPECT_EQ(sink.cancel_pair_ids[0], "pair_001");
}

TEST(test_order_sink, mock_mixed_operations)
{
    MockOrderSink sink;
    sink.submitBuy(nullptr, "INE.ec.ec2607", 2500.0, 2, Source::CLOSEOUT, 1);
    sink.submitSell(nullptr, "INE.ec.ec2608", 2510.0, 1, Source::HEDGING);
    sink.cancelAllBySource(nullptr, Source::ARBITRAGE);
    sink.cancelByPair(nullptr, "pair_042");
    EXPECT_EQ(sink.orders.size(), 2u);
    EXPECT_EQ(sink.cancel_all_sources.size(), 1u);
    EXPECT_EQ(sink.cancel_pair_ids.size(), 1u);
    // Verify order sequencing
    EXPECT_TRUE(sink.orders[0].is_buy);
    EXPECT_FALSE(sink.orders[1].is_buy);
    EXPECT_EQ(sink.orders[0].code, "INE.ec.ec2607");
    EXPECT_EQ(sink.orders[1].code, "INE.ec.ec2608");
}

TEST(test_order_sink, order_submit_result_fields)
{
    MockOrderSink sink;
    auto r = sink.submitBuy(nullptr, "TEST", 100.0, 1, Source::ARBITRAGE);
    EXPECT_FALSE(r.rate_limited);
    EXPECT_FALSE(r.self_trade_blocked);
    EXPECT_FALSE(r.rejected);
    EXPECT_EQ(r.localids.size(), 1u);
}

//==============================================================================
// B9: SignalCombinerRegistry smoke test (verify extension point is not broken)
//==============================================================================
#include "../WtFutuCore/signals/ISignalCombiner.h"

TEST(test_signal_combiner_registry, linear_registered_by_default)
{
    // The registry auto-registers "linear" in its constructor
    EXPECT_TRUE(futu::SignalCombinerRegistry::instance().has("linear"));
    EXPECT_FALSE(futu::SignalCombinerRegistry::instance().has("nonexistent_model"));
}

TEST(test_signal_combiner_registry, create_linear_returns_valid_combiner)
{
    auto combiner = futu::SignalCombinerRegistry::instance().create("linear");
    ASSERT_NE(combiner, nullptr);
    EXPECT_STREQ(combiner->typeName(), "linear");
}

// Dummy second implementation: proves the registry accepts custom combiners
class DummyCombiner : public futu::ISignalCombiner
{
public:
    const char* typeName() const override { return "dummy_test"; }
};

TEST(test_signal_combiner_registry, register_and_lookup_custom_combiner)
{
    futu::SignalCombinerRegistry::instance().registerCombiner(
        "dummy_test", [] { return std::make_unique<DummyCombiner>(); });
    EXPECT_TRUE(futu::SignalCombinerRegistry::instance().has("dummy_test"));
    auto c = futu::SignalCombinerRegistry::instance().create("dummy_test");
    ASSERT_NE(c, nullptr);
    EXPECT_STREQ(c->typeName(), "dummy_test");
}
