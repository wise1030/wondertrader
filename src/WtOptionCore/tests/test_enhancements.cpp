/*!
 * \file test_enhancements.cpp
 * \brief Unit tests for enhancement modules:
 *   RiskFilterChain, PositionOffsetMgr, PositionGuard, FillPriceChecker, RiskLimitsEx
 */
#include "../RiskFilterChain.h"
#include "../PositionOffsetMgr.h"
#include "../PositionGuard.h"
#include "../FillPriceChecker.h"
#include "../RiskLimitsEx.h"
#include "../OptionQuoteManager.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace wt_option;

#define ASSERT_EQ(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    if (!(_va == _vb)) { \
        std::cerr << "FAIL: " << #a << " != " << #b << std::endl; \
        std::exit(1); \
    } \
} while(0)

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        std::cerr << "FAIL: " << #x << " is false" << std::endl; \
        std::exit(1); \
    } \
} while(0)

// ============================================================================
// Test RiskFilterChain
// ============================================================================
void test_risk_filter_chain() {
    std::cout << "=== test_risk_filter_chain ===" << std::endl;

    // MaxOrderSizeFilter - modify mode
    {
        MaxOrderSizeFilter f(10, false);
        FilterContext ctx;
        ctx.qty = 15;
        ASSERT_EQ(f.process(ctx), FilterResult::MODIFIED);
        ASSERT_EQ(ctx.modifiedQty, 10u);
    }

    // MaxOrderSizeFilter - reject mode
    {
        MaxOrderSizeFilter f(10, true);
        FilterContext ctx;
        ctx.qty = 15;
        ASSERT_EQ(f.process(ctx), FilterResult::REJECTED);
    }

    // MinSellPriceFilter
    {
        MinSellPriceFilter f(100.0);
        FilterContext ctx;
        ctx.isBuy = false;
        ctx.price = 90.0;
        ASSERT_EQ(f.process(ctx), FilterResult::REJECTED);

        ctx.isBuy = true;
        ASSERT_EQ(f.process(ctx), FilterResult::APPROVED);

        ctx.isBuy = false;
        ctx.price = 110.0;
        ASSERT_EQ(f.process(ctx), FilterResult::APPROVED);
    }

    // MaxPositionFilter - reject mode
    {
        MaxPositionFilter f(50, MaxPositionFilter::REJECT_ON_OVERFLOW);
        FilterContext ctx;
        ctx.isBuy = true;
        ctx.qty = 10;
        ctx.currentPosition = 45;
        ctx.potentialPosition = 45;
        ASSERT_EQ(f.process(ctx), FilterResult::REJECTED);
    }

    // MaxPositionFilter - modify mode
    {
        MaxPositionFilter f(50, MaxPositionFilter::MODIFY_TO_MAX);
        FilterContext ctx;
        ctx.isBuy = true;
        ctx.qty = 10;
        ctx.currentPosition = 45;
        ctx.potentialPosition = 45;
        ASSERT_EQ(f.process(ctx), FilterResult::MODIFIED);
        ASSERT_EQ(ctx.modifiedQty, 5u);
    }

    // MaxCancelFilter - soft limit allows risk-reducing
    {
        MaxCancelFilter f(5, 10);
        FilterContext ctx;
        ctx.numCancels = 7;
        ctx.isBuy = false;
        ctx.qty = 5;
        ctx.currentPosition = 10;
        ctx.potentialPosition = 10;
        // Sell reduces position -> should be approved at soft limit
        ASSERT_EQ(f.process(ctx), FilterResult::APPROVED);

        // Buy increases position -> rejected at soft limit
        ctx.isBuy = true;
        ASSERT_EQ(f.process(ctx), FilterResult::REJECTED);

        // Hard limit blocks all
        ctx.numCancels = 10;
        ASSERT_EQ(f.process(ctx), FilterResult::REJECTED);
    }

    // Full chain - short circuit on first reject
    {
        RiskFilterChain chain;
        chain.add(std::make_unique<MaxOrderSizeFilter>(5, true));
        chain.add(std::make_unique<MinSellPriceFilter>(100.0));

        FilterContext ctx;
        ctx.code = "test";
        ctx.isBuy = false;
        ctx.price = 90.0;
        ctx.qty = 10;
        // First filter rejects (size > 5, reject mode)
        ASSERT_EQ(chain.execute(ctx), FilterResult::REJECTED);
        ASSERT_TRUE(!ctx.rejectReason.empty());
    }

    std::cout << "  PASS" << std::endl;
}

// ============================================================================
// Test PositionOffsetMgr
// ============================================================================
void test_position_offset_mgr() {
    std::cout << "=== test_position_offset_mgr ===" << std::endl;

    PositionOffsetMgr mgr;

    // Simulate broker position: 10 long today, 5 long prev
    mgr.onPositionUpdate(true, 5.0, 5.0, 10.0, 10.0);
    ASSERT_EQ(mgr.getLongToday(), 10);
    ASSERT_EQ(mgr.getLongPrev(), 5);
    ASSERT_EQ(mgr.getCloseableToday(false), 10);  // closeable long today for sell
    ASSERT_EQ(mgr.getCloseablePrev(false), 5);      // closeable long prev for sell

    // Simulate short position: 8 short today
    mgr.onPositionUpdate(false, 0.0, 0.0, 8.0, 8.0);
    ASSERT_EQ(mgr.getShortToday(), 8);
    ASSERT_EQ(mgr.getCloseableToday(true), 8);   // closeable short today for buy

    // Order breakdown: buy 20, should close 8 short today, then open 12
    auto bd = mgr.getOrderBreakdown(true, 20);
    ASSERT_EQ(bd.closeTodayQty, 8u);
    ASSERT_EQ(bd.closePrevQty, 0u);
    ASSERT_EQ(bd.openQty, 12u);

    // Sell 20: close 10 long today, 5 long prev, open 5
    bd = mgr.getOrderBreakdown(false, 20);
    ASSERT_EQ(bd.closeTodayQty, 10u);
    ASSERT_EQ(bd.closePrevQty, 5u);
    ASSERT_EQ(bd.openQty, 5u);

    // Frozen tracking: send close order
    mgr.onOrderSent(false, 3, true);  // sell 3 to close long today
    ASSERT_EQ(mgr.getCloseableToday(false), 7);  // 10 - 3 frozen

    // Cancel the order
    mgr.onOrderCancelled(false, 3, true);
    ASSERT_EQ(mgr.getCloseableToday(false), 10);  // back to 10

    // Discrepancy: fill increases local but broker hasn't changed
    mgr.onFill(true, 5, false);  // buy 5 open
    auto disc = mgr.checkDiscrepancy();
    ASSERT_TRUE(disc.hasDiscrepancy);
    ASSERT_EQ(mgr.getLocalNet(), 5);

    // Sync to broker
    mgr.syncLocalToBroker();
    ASSERT_EQ(mgr.getLocalNet(), mgr.getBrokerNet());

    std::cout << "  PASS" << std::endl;
}

// ============================================================================
// Test PositionGuard
// ============================================================================
void test_position_guard() {
    std::cout << "=== test_position_guard ===" << std::endl;

    PositionGuard guard;
    ASSERT_TRUE(guard.isOK());

    // Broker says position = 10
    guard.onBrokerPosition(true, 10.0);
    ASSERT_EQ(guard.getBrokerPos(), 10);
    ASSERT_TRUE(guard.isOK());

    // Internal fills: buy 5 -> internal = 15 (matches broker+5)
    // But broker hasn't updated yet -> discrepancy
    guard.onFill(true, 5);
    // internal=15, broker=10, diff=5 > tolerance(0) -> disabled
    ASSERT_TRUE(!guard.isOK());
    ASSERT_EQ(guard.getDiff(), 5);

    // Reconcile
    guard.reconcile();
    ASSERT_TRUE(guard.isOK());
    ASSERT_EQ(guard.getInternalPos(), 10);

    std::cout << "  PASS" << std::endl;
}

// ============================================================================
// Test FillPriceChecker
// ============================================================================
void test_fill_price_checker() {
    std::cout << "=== test_fill_price_checker ===" << std::endl;

    FillPriceChecker checker;
    bool warningCalled = false;
    bool panicCalled = false;

    checker.setWarningCallback([&](const std::string&, double, double, double) {
        warningCalled = true;
    });
    checker.setPanicCallback([&](const std::string&, double, double, double) {
        panicCalled = true;
    });

    // Send order at 100
    checker.onOrderSent("test", 1001, 100.0);

    // Fill at 100.1 -> 0.1% deviation, below warning
    checker.onFill("test", 1001, 100.1);
    ASSERT_TRUE(!warningCalled);
    ASSERT_TRUE(!panicCalled);

    // Fill at 100.3 -> 0.3% deviation, warning
    warningCalled = false;
    checker.onOrderSent("test", 1002, 100.0);
    checker.onFill("test", 1002, 100.3);
    ASSERT_TRUE(warningCalled);
    ASSERT_TRUE(!panicCalled);

    // Fill at 100.6 -> 0.6% deviation, panic
    panicCalled = false;
    checker.onOrderSent("test", 1003, 100.0);
    checker.onFill("test", 1003, 100.6);
    ASSERT_TRUE(panicCalled);

    // Cancel removes tracking
    checker.onOrderSent("test", 1004, 100.0);
    checker.onOrderCancelled(1004);
    // Fill after cancel should not trigger
    warningCalled = false;
    checker.onFill("test", 1004, 101.0);
    ASSERT_TRUE(!warningCalled);

    std::cout << "  PASS" << std::endl;
}

// ============================================================================
// Test RiskLimitsEx
// ============================================================================
void test_risk_limits_ex() {
    std::cout << "=== test_risk_limits_ex ===" << std::endl;

    RiskLimitsEx limits;
    limits.maxOrderSize = 50;
    limits.maxOrderValue = 100000;
    limits.clearlyErroneousPercent = 0.05;
    limits.minSellPrice = 50.0;
    limits.maxPositionPerOption = 100;

    // Order size check
    auto r = limits.checkOrderSize(60);
    ASSERT_EQ(r.result, RiskLimitsEx::CheckResult::REJECT);
    ASSERT_EQ(r.value, 60.0);

    r = limits.checkOrderSize(30);
    ASSERT_EQ(r.result, RiskLimitsEx::CheckResult::PASS);

    // Order value check
    r = limits.checkOrderValue(2000.0, 60);
    ASSERT_EQ(r.result, RiskLimitsEx::CheckResult::REJECT);

    // Clearly erroneous
    r = limits.checkClearlyErroneous(110.0, 100.0);
    ASSERT_EQ(r.result, RiskLimitsEx::CheckResult::REJECT);
    ASSERT_TRUE(std::abs(r.value - 10.0) < 0.01);

    r = limits.checkClearlyErroneous(103.0, 100.0);
    ASSERT_EQ(r.result, RiskLimitsEx::CheckResult::PASS);

    // Min sell price
    r = limits.checkMinSellPrice(false, 40.0);
    ASSERT_EQ(r.result, RiskLimitsEx::CheckResult::REJECT);

    r = limits.checkMinSellPrice(true, 40.0);
    ASSERT_EQ(r.result, RiskLimitsEx::CheckResult::PASS);

    // Pre-trade composite check
    r = limits.checkPreTrade("test", false, 40.0, 10, 0, 100.0);
    // Sell at 40 < minSellPrice(50) -> reject
    ASSERT_EQ(r.result, RiskLimitsEx::CheckResult::REJECT);

    r = limits.checkPreTrade("test", true, 100.0, 10, 0, 100.0);
    ASSERT_EQ(r.result, RiskLimitsEx::CheckResult::PASS);

    // Greeks check
    r = limits.checkGreeks(1500, 50, 5000, 0);
    ASSERT_EQ(r.result, RiskLimitsEx::CheckResult::WARN);
    ASSERT_EQ(r.reason, "max_delta");

    r = limits.checkGreeks(500, 50, 5000, -200000);
    ASSERT_EQ(r.result, RiskLimitsEx::CheckResult::WARN);
    ASSERT_EQ(r.reason, "max_loss_per_day");

    r = limits.checkGreeks(500, 50, 5000, 0);
    ASSERT_EQ(r.result, RiskLimitsEx::CheckResult::PASS);

    std::cout << "  PASS" << std::endl;
}

// ============================================================================
// Test OptionQuoteManager enhanced features
// ============================================================================
void test_oqm_enhancements() {
    std::cout << "=== test_oqm_enhancements ===" << std::endl;

    // Test cancel throttle (hard + buffer + soft)
    {
        OptionQuoteManager::Config cfg;
        cfg.max_cancels_allowed = 10;
        cfg.cancel_buffer = 2;
        cfg.cancel_soft_max = 5;
        // trueMax = 10 - 2 = 8

        OptionQuoteManager oqm("test", cfg, nullptr);

        // Verify true max cancels via public API
        ASSERT_EQ(oqm.getTrueMaxCancels(), 8);

        // Verify guard OK without guard set
        ASSERT_TRUE(oqm.isGuardOK());

        // Verify scale factor doesn't crash
        oqm.setScaleFactor(0.5);
        oqm.setScaleFactor(2.0);   // should clamp
        oqm.setScaleFactor(-0.5);  // should clamp
    }

    // Test PositionGuard integration
    {
        OptionQuoteManager::Config cfg;
        OptionQuoteManager oqm("test", cfg, nullptr);

        // No guard set -> should be OK
        ASSERT_TRUE(oqm.isGuardOK());

        // Set a guard and disable it
        auto guard = std::make_shared<PositionGuard>();
        oqm.setPositionGuard(guard);
        ASSERT_TRUE(oqm.isGuardOK());

        // Simulate discrepancy
        guard->onBrokerPosition(true, 10.0);
        guard->onFill(true, 20); // internal=30, broker=10, diff=20 > tolerance(0)

        // Guard should now be disabled -> OQM should not be OK
        ASSERT_TRUE(!oqm.isGuardOK());

        // Reconcile
        guard->reconcile();
        ASSERT_TRUE(oqm.isGuardOK());
    }

    // Test incremental diffing via public API
    {
        OptionQuoteManager::Config cfg;
        cfg.enable_quote_api = false;
        OptionQuoteManager oqm("test", cfg, nullptr);

        // No orders -> getCurrentMarket should be empty
        ASSERT_TRUE(oqm.getCurrentMarket().empty());
    }

    // Test reject retry state
    {
        OptionQuoteManager::Config cfg;
        OptionQuoteManager oqm("test", cfg, nullptr);

        ASSERT_TRUE(!oqm.isRetryPending());

        // Simulate order sent + full rejection
        // Need to create an order first, then reject it
        // Since OQM requires WT context to send, we test the state indirectly:
        // onOrderStatusChange with unknown localid won't trigger retry
        // (no matching order). This is correct behavior.
        oqm.onOrderStatusChange(9999, true, 10, 10, 100.0, true);
        // Unknown localid -> no retry pending (order not found)
        ASSERT_TRUE(!oqm.isRetryPending());
    }

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "WtOptionCore Enhancement Module Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_risk_filter_chain();
    test_position_offset_mgr();
    test_position_guard();
    test_fill_price_checker();
    test_risk_limits_ex();
    test_oqm_enhancements();

    std::cout << std::endl << "========================================" << std::endl;
    std::cout << "ALL TESTS PASSED" << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
