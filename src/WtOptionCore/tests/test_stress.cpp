// Phase 2: scenario stress analyzer acceptance.
#include "../StressAnalyzer.h"
#include <iostream>
#include <cmath>
#include <cstdlib>

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

    std::cout << "  PASS" << std::endl;
    std::cout << "\nALL TESTS PASSED" << std::endl;
    return 0;
}
