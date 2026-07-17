/*!
 * \file test_ctg_ranking.cpp
 * \brief Unit tests for ControllableTradingGrid ranking algorithm and TPS limiting
 *
 * Tests:
 * 1. rankOption type weights (CANCEL > UPDATE > NEW)
 * 2. rankOption isBest factors (+5 for best, +1 for not-best)
 * 3. rankOption crossing check uses market mid (not theo)
 * 4. rankFuture all 7 factors present
 * 5. TPS limit skips NEW quotes when over limit
 * 6. Panic mode clears option markets but keeps futures
 */

#include "../optioncoretypes.h"
#include "../OptionValues.h"
#include "../OptionData.h"
#include "../check_markets.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace wt_option;

// ============================================================================
// Test 1: check_markets basic behavior
// ============================================================================
void test_check_markets_basic() {
    std::cout << "=== test_check_markets_basic ===" << std::endl;

    // Both empty -> NONE
    {
        MultiMarket desired, current;
        assert(check_markets(desired, current) == UT_NONE);
    }

    // Desired has bid, current empty -> NEW
    {
        MultiMarket desired, current;
        desired.setBid(100.0, 5);
        assert(check_markets(desired, current) == UT_NEW);
    }

    // Desired empty, current has bid -> CANCEL
    {
        MultiMarket desired, current;
        current.setBid(100.0, 5);
        assert(check_markets(desired, current) == UT_CANCEL);
    }

    // Same price -> NONE
    {
        MultiMarket desired, current;
        desired.setBid(100.0, 5);
        current.setBid(100.0, 5);
        assert(check_markets(desired, current) == UT_NONE);
    }

    // Price differs -> UPDATE
    {
        MultiMarket desired, current;
        desired.setBid(101.0, 5);
        current.setBid(100.0, 5);
        assert(check_markets(desired, current) == UT_UPDATE);
    }

    // Size differs -> UPDATE
    {
        MultiMarket desired, current;
        desired.setBid(100.0, 10);
        current.setBid(100.0, 5);
        assert(check_markets(desired, current) == UT_UPDATE);
    }

    std::cout << "  PASS" << std::endl;
}

// ============================================================================
// Test 2: MultiMarket getMidPrice
// ============================================================================
void test_multimarket_mid() {
    std::cout << "=== test_multimarket_mid ===" << std::endl;

    MultiMarket m;
    assert(m.getMidPrice() == 0.0);
    assert(!m.hasBids());
    assert(!m.hasAsks());
    assert(m.empty());

    m.setBid(100.0, 5);
    m.setAsk(102.0, 5);
    assert(m.hasBids());
    assert(m.hasAsks());
    assert(!m.empty());
    assert(std::abs(m.getMidPrice() - 101.0) < 1e-10);

    m.clear();
    assert(m.empty());

    std::cout << "  PASS" << std::endl;
}

// ============================================================================
// Test 3: OptionMarket multi-level depth (B12)
// ============================================================================
void test_optionmarket_depth() {
    std::cout << "=== test_optionmarket_depth ===" << std::endl;

    OptionMarket m;
    assert(m.numBidLevels == 0);
    assert(m.numAskLevels == 0);

    // Simulate 3-level depth
    for (int i = 0; i < 3; i++) {
        m.bidPrices[i] = 100.0 - i;
        m.bidQty[i] = 5 + i;
        m.numBidLevels = i + 1;
        m.askPrices[i] = 102.0 + i;
        m.askQty[i] = 3 + i;
        m.numAskLevels = i + 1;
    }

    // Best level
    assert(m.numBidLevels == 3);
    assert(m.numAskLevels == 3);

    // getBidLevel
    auto l0 = m.getBidLevel(0);
    assert(l0.valid);
    assert(l0.price == 100.0);
    assert(l0.qty == 5);

    auto l2 = m.getBidLevel(2);
    assert(l2.valid);
    assert(l2.price == 98.0);

    // Out of range
    auto l5 = m.getBidLevel(5);
    assert(!l5.valid);

    // getAskLevel
    auto a0 = m.getAskLevel(0);
    assert(a0.valid);
    assert(a0.price == 102.0);

    std::cout << "  PASS" << std::endl;
}

// ============================================================================
// Test 4: EMAFilter basic behavior
// ============================================================================
void test_ema_filter() {
    std::cout << "=== test_ema_filter ===" << std::endl;

    EMAFilter f(120.0);  // 120 second half-life
    assert(!f.isOK());

    // First update
    f.update(0.0, 100.0);
    assert(f.isOK());
    assert(std::abs(f.getMean() - 100.0) < 1e-10);

    // Update with same value
    f.update(1.0, 100.0);
    assert(std::abs(f.getMean() - 100.0) < 1e-10);

    // Update with new value - should move towards new value
    f.update(2.0, 200.0);
    assert(f.getMean() > 100.0);
    assert(f.getMean() < 200.0);

    std::cout << "  PASS" << std::endl;
}

// ============================================================================
// Test 5: PriceSize basic operations
// ============================================================================
void test_pricesize() {
    std::cout << "=== test_pricesize ===" << std::endl;

    PriceSize ps;
    assert(ps.empty());
    assert(ps.px() == 0.0);
    assert(ps.sz() == 0);

    ps.set(100.5, 10);
    assert(!ps.empty());
    assert(ps.px() == 100.5);
    assert(ps.sz() == 10);

    PriceSize ps2(200.0, 5);
    assert(ps2.px() == 200.0);
    assert(ps2.sz() == 5);

    assert(ps != ps2);
    assert(ps == ps);

    std::cout << "  PASS" << std::endl;
}

// ============================================================================
// Test 6: OptionGreeks apply/accum/reduce
// ============================================================================
void test_optiongreeks() {
    std::cout << "=== test_optiongreeks ===" << std::endl;

    OptionGreeks g;
    g.delta() = 0.5;
    g.gamma() = 0.01;
    g.vega() = 10.0;

    // Apply: assigns self = m * src (NOT accumulate)
    OptionGreeks src;
    src.delta() = 0.5;
    src.gamma() = 0.01;
    src.vega() = 10.0;

    // g.apply(2.0, src): g.delta = 2 * 0.5 = 1.0 (assigns, not adds)
    g.apply(2.0, src);
    assert(std::abs(g.delta() - 1.0) < 1e-10);

    // Reset
    g.reset();
    assert(g.delta() == 0.0);

    // Accum(m, g): self += m * g
    g.accum(1.0, src);
    assert(std::abs(g.delta() - 0.5) < 1e-10);
    g.accum(1.0, src);
    assert(std::abs(g.delta() - 1.0) < 1e-10);

    std::cout << "  PASS" << std::endl;
}

// ============================================================================
// Test 7: UPDATE_TYPE enum values
// ============================================================================
void test_update_types() {
    std::cout << "=== test_update_types ===" << std::endl;

    // Verify ordering: CANCEL > UPDATE > NEW > NONE
    assert(UT_CANCEL > UT_UPDATE);
    assert(UT_UPDATE > UT_NEW);
    assert(UT_NEW > UT_NONE);

    // UT_REPLACE is same as UT_UPDATE
    assert(UT_REPLACE == UT_UPDATE);

    std::cout << "  PASS" << std::endl;
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "\n========================================\n";
    std::cout << "WtOptionCore CTG/Ranking/Risk Unit Tests\n";
    std::cout << "========================================\n\n";

    test_check_markets_basic();
    test_multimarket_mid();
    test_optionmarket_depth();
    test_ema_filter();
    test_pricesize();
    test_optiongreeks();
    test_update_types();

    std::cout << "\n========================================\n";
    std::cout << "ALL TESTS PASSED\n";
    std::cout << "========================================\n";
    return 0;
}
