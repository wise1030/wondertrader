// Phase 2: DeltaHedgeCoordinator in-flight/cooldown acceptance.
#include "../OptionHedgeCoordinator.h"
#include <iostream>
#include <cstdlib>

using namespace wt_option;

#define ASSERT_TRUE(x) do { if (!(x)) { std::cerr << "FAIL: " #x << std::endl; std::exit(1); } } while (0)
#define ASSERT_EQ(a,b) do { auto _a=(a); auto _b=(b); if (!(_a==_b)) { \
    std::cerr << "FAIL: " #a " != " #b << std::endl; std::exit(1); } } while (0)

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "WtOptionCore Hedge Coordinator Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    DeltaHedgeExecutor::Config cfg;
    cfg.thresholdDelta = 10.0;
    cfg.maxOrderSize = 5;
    cfg.cooldownSec = 3.0;

    int sent = 0;
    uint32_t next_id = 100;
    uint32_t last_qty = 0;
    OptionHedgeCoordinator hc;
    hc.configure(cfg, 300.0, "IF",
        [&](const std::string&, bool, double, uint32_t qty) {
            ++sent; last_qty = qty; return std::vector<uint32_t>{next_id++};
        },
        [](const std::string&) { return 4000.0; });

    // 150 delta / 300 per lot = 1 lot (not 150).
    ASSERT_TRUE(hc.check(100.0, 150.0));
    ASSERT_EQ(sent, 1);
    ASSERT_EQ(last_qty, 1u);

    // In-flight: no second batch.
    ASSERT_TRUE(!hc.check(101.0, 150.0));
    ASSERT_EQ(sent, 1);

    // Terminal releases the latch, but cooldown still blocks.
    hc.onTerminal(100);
    ASSERT_TRUE(!hc.check(101.5, 150.0));
    ASSERT_EQ(sent, 1);

    // Cooldown elapsed -> next batch allowed.
    ASSERT_TRUE(hc.check(104.0, 150.0));
    ASSERT_EQ(sent, 2);

    // Below threshold: no fire.
    hc.onTerminal(101);
    ASSERT_TRUE(!hc.check(200.0, 5.0));

    std::cout << "  PASS" << std::endl;
    std::cout << "\nALL TESTS PASSED" << std::endl;
    return 0;
}
