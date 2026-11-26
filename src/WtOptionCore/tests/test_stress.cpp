// Phase 2: scenario stress analyzer acceptance.
#include "../AsyncStress.h"
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <thread>

using namespace wt_option;

#define ASSERT_TRUE(x) do { if (!(x)) { std::cerr << "FAIL: " #x << std::endl; std::exit(1); } } while (0)
#define ASSERT_NEAR(a,b,e) do { if (std::fabs((a)-(b)) > (e)) { \
    std::cerr << "FAIL: " << #a << "=" << (a) << " expected " << (b) << std::endl; std::exit(1); } } while (0)

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "WtOptionCore Stress Analyzer Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    StressAnalyzer sa;
    sa.addDefaultScenarios();
    ASSERT_TRUE(sa.scenarios().size() >= 6);

    // Spot-only: delta 100, spot 4000, +10% -> 40,000.
    StressScenario up{"up", 0.10, 0.0, 0.0};
    ASSERT_NEAR(StressAnalyzer::evaluate(up, 4000.0, 100.0, 0.0, 0.0, 0.0), 40000.0, 1e-6);

    // Gamma adds 0.5*gamma*dS^2: dS=400, gamma=2 -> 160,000; +40,000 = 200,000.
    ASSERT_NEAR(StressAnalyzer::evaluate(up, 4000.0, 100.0, 2.0, 0.0, 0.0), 200000.0, 1e-6);

    // Vega +5 vol with vega=1000 -> +50; theta -10/day * 1 -> -10; total 40.
    StressScenario vol{"vol", 0.0, 0.05, 1.0};
    ASSERT_NEAR(StressAnalyzer::evaluate(vol, 4000.0, 0.0, 0.0, 1000.0, -10.0), 40.0, 1e-6);

    // Worst: long delta 100, spot -20% -> -80,000 (crash adds vega/theta but
    // spot dominates for this book).
    auto w = sa.worst(4000.0, 100.0, 0.0, 0.0, 0.0);
    ASSERT_TRUE(w.scenario.name == "spot_-20%");
    ASSERT_NEAR(w.pnl, -80000.0, 1e-6);

    // Invalid inputs are neutral, not NaN.
    ASSERT_NEAR(StressAnalyzer::evaluate(up, 0.0, 100.0, 0.0, 0.0, 0.0), 0.0, 1e-12);
    ASSERT_NEAR(StressAnalyzer::evaluate(up, 4000.0, std::nan(""), 0.0, 0.0, 0.0), 0.0, 1e-12);

    // Invalid scenario addition is rejected.
    sa.clearScenarios();
    sa.addScenario({"bad", std::nan(""), 0.0, 0.0});
    ASSERT_TRUE(sa.scenarios().empty());

    // B13: async result is computed from an immutable value snapshot and is
    // delivered exactly once. Mutating the caller's object after submit must
    // not affect the background job.
    AsyncStress async;
    StressSnapshot snapshot;
    snapshot.seq = 42;
    snapshot.sessionDate = 20260605;
    snapshot.positionVersion = 7;
    snapshot.submittedAt = 100.0;
    snapshot.spot = 4000.0;
    snapshot.portfolio = {100.0, 0.0, 0.0, 0.0};
    snapshot.scenarios = {{"up", 0.10, 0.0, 0.0},
                          {"down", -0.10, 0.0, 0.0}};
    snapshot.expiries = {
        {202606, 50.0, 60.0, 0.0, 0.0, 0.0},
        {202607, -20.0, -10.0, 0.0, 0.0, 0.0}};
    ASSERT_TRUE(async.submit(snapshot));
    snapshot.spot = 1.0;
    snapshot.portfolio.delta = 999999.0;
    snapshot.expiries.clear();
    snapshot.scenarios[0].spot_shock_pct = 5.0;

    StressReport report;
    bool published = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (async.tryTake(report)) { published = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(published);
    ASSERT_TRUE(!async.tryTake(report));
    ASSERT_TRUE(report.seq == 42 && report.sessionDate == 20260605);
    ASSERT_TRUE(report.positionVersion == 7 && report.submittedAt == 100.0);
    ASSERT_NEAR(report.spot, 4000.0, 1e-12);
    ASSERT_TRUE(report.all.size() == 2);
    ASSERT_TRUE(report.all[0].scenario.name == "up");
    ASSERT_NEAR(report.all[0].pnl, 40000.0, 1e-6);
    ASSERT_TRUE(report.all[1].scenario.name == "down");
    ASSERT_NEAR(report.all[1].pnl, -40000.0, 1e-6);
    ASSERT_TRUE(report.worst.scenario.name == "down");
    ASSERT_NEAR(report.worst.pnl, -40000.0, 1e-6);
    ASSERT_TRUE(report.expiries.size() == 2);
    ASSERT_NEAR(report.expiries[0].optionDelta, 60.0, 1e-12);
    ASSERT_NEAR(report.expiries[0].worst.pnl, -20000.0, 1e-6);
    ASSERT_TRUE(report.expiries[1].worst.scenario.name == "up");
    ASSERT_NEAR(report.expiries[1].worst.pnl, -8000.0, 1e-6);

    // Freshness gate: old sequence, session/position changes, time reversal
    // and TTL expiry are all rejected before they can trigger Panic.
    ASSERT_TRUE(AsyncStress::isFresh(report, 41, 20260605, 7, 100.1, 5.0));
    ASSERT_TRUE(!AsyncStress::isFresh(report, 42, 20260605, 7, 100.1, 5.0));
    ASSERT_TRUE(!AsyncStress::isFresh(report, 41, 20260606, 7, 100.1, 5.0));
    ASSERT_TRUE(!AsyncStress::isFresh(report, 41, 20260605, 8, 100.1, 5.0));
    ASSERT_TRUE(!AsyncStress::isFresh(report, 41, 20260605, 7, 99.9, 5.0));
    ASSERT_TRUE(!AsyncStress::isFresh(report, 41, 20260605, 7, 105.1, 5.0));
    ASSERT_TRUE(AsyncStress::isFresh(report, 41, 20260605, 7, 100000.0, 0.0));

    async.stop();
    async.stop();  // idempotent
    ASSERT_TRUE(!async.submit(StressSnapshot{1, 20260605, 7, 100.0, 4000.0,
        {100.0, 0.0, 0.0, 0.0}, {}, {{"up", 0.1, 0.0, 0.0}}}));

    std::cout << "  PASS" << std::endl;
    std::cout << "\nALL TESTS PASSED" << std::endl;
    return 0;
}
