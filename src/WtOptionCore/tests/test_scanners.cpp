// Scanner acceptance tests: factory registration, enable/start lifecycle,
// per-scanner numeric config, signal dispatch, panic gate, GARCH kernel.
#include "../Scanners/ButterflyScanner.h"
#include "../Scanners/GarchScanner.h"
#include "../Scanners/LowBidsScanner.h"
#include "../Scanners/MMScanner.h"
#include "../Scanners/OpenScanner.h"
#include "../Scanners/SimplexScanner.h"
#include "../Scanners/StrikeSpreadScanner.h"
#include "../Scanners/SyntheticFutureScanner.h"
#include "../Scanners/VolSpreadScanner.h"
#include "../OptionData.h"
#include "../OptionScanCoordinator.h"
#include "../OptionExpiryLifecycle.h"
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace wt_option;

#define ASSERT_TRUE(x) do { \
    if (!(x)) { std::cerr << "FAIL: " << #x << " is false" << std::endl; std::exit(1); } \
} while (0)

#define ASSERT_EQ(a,b) do { \
    auto _a=(a); auto _b=(b); \
    if (!(_a==_b)) { std::cerr << "FAIL: " << #a << " != " << #b << std::endl; std::exit(1); } \
} while (0)

namespace {

struct HitCounter : IScannerListener {
    int hits = 0;
    ScannerHitEvent last;
    void onScannerHit(const ScannerHitEvent& e) override { ++hits; last = e; }
};

std::shared_ptr<OptionData> makeOption(double bid, double ask, double theo,
                      double delta, double iv = 0.2, double vega = 10.0)
{
    OptionInfo info;
    info.code = "TEST.OPT";
    info.expiry = 202612;
    info.multiplier = 100;
    auto od = std::make_shared<OptionData>(info);
    OptionMarket m;
    m.bid = bid;
    m.ask = ask;
    m.last = (bid + ask) * 0.5;
    od->updateMarket(m);
    od->values(0).setTheo(theo);
    od->values(0).setImpliedVol(iv);
    od->values(0).greeks().delta() = delta;
    od->values(0).greeks().vega() = vega;
    return od;
}

void test_factory_and_edge_scanners()
{
    std::cout << "=== test_factory_and_edge_scanners ===" << std::endl;
    const std::vector<std::string> types = {
        "ButterflyScanner", "LowBidsScanner", "OpenScanner", "SimplexScanner",
        "StrikeSpreadScanner", "SyntheticFutureScanner", "VolSpreadScanner", "MMScanner"
    };

    for (const auto& type : types) {
        ScannerConfig sc;
        sc.name = type;
        sc.enabled = true;
        auto scanner = ScannerFactory::instance().createScanner(type, sc);
        ASSERT_TRUE(scanner != nullptr);
        ASSERT_EQ(scanner->getName(), type);

        HitCounter hits;
        scanner->addListener(&hits);
        scanner->setEnabled(true);
        scanner->onStart();
        ASSERT_TRUE(scanner->isActive());

        // mid=10, theo=12 → edge 2.0 > 0.5, delta 0.5 in [0.1,0.9]
        auto opt = makeOption(9.0, 11.0, 12.0, 0.5);
        scanner->onOptionUpdate(opt.get());
        ASSERT_EQ(hits.hits, 1);
        ASSERT_TRUE(hits.last.option == opt.get());
        ASSERT_TRUE(!hits.last.reason.empty());
        ASSERT_TRUE(hits.last.signal > 0.0);

        // Low edge (0.2 < 0.5) must be filtered.
        auto thin = makeOption(9.0, 11.0, 10.2, 0.5);
        scanner->onOptionUpdate(thin.get());
        ASSERT_EQ(hits.hits, 1);

        // Delta outside [0.1,0.9] must be filtered.
        auto wing = makeOption(9.0, 11.0, 12.0, 0.95);
        scanner->onOptionUpdate(wing.get());
        ASSERT_EQ(hits.hits, 1);

        // Panic disables signal dispatch.
        scanner->onPanic();
        scanner->onOptionUpdate(opt.get());
        ASSERT_EQ(hits.hits, 1);

        scanner->removeListener(&hits);
        scanner->onStop();
        ASSERT_TRUE(!scanner->isActive());
    }
    std::cout << "  PASS" << std::endl;
}

void test_scanner_numeric_config()
{
    std::cout << "=== test_scanner_numeric_config ===" << std::endl;

    ScannerConfig sc;
    sc.name = "config";
    sc.enabled = true;
    sc.min_edge = 5.0;          // raise threshold above the 2.0 edge
    sc.delta_min = 0.2;
    sc.delta_max = 0.6;
    auto scanner = ScannerFactory::instance().createScanner("ButterflyScanner", sc);
    ASSERT_TRUE(scanner != nullptr);
    scanner->setEnabled(true);
    scanner->onStart();

    HitCounter hits;
    scanner->addListener(&hits);
    auto opt = makeOption(9.0, 11.0, 12.0, 0.5);
    scanner->onOptionUpdate(opt.get());
    ASSERT_EQ(hits.hits, 0);    // min_edge override honored

    sc.min_edge = 0.5;
    auto scanner2 = ScannerFactory::instance().createScanner("ButterflyScanner", sc);
    scanner2->setEnabled(true);
    scanner2->onStart();
    HitCounter hits2;
    scanner2->addListener(&hits2);
    scanner2->onOptionUpdate(opt.get());
    ASSERT_EQ(hits2.hits, 1);

    std::cout << "  PASS" << std::endl;
}

void test_garch_scanner()
{
    std::cout << "=== test_garch_scanner ===" << std::endl;

    ScannerConfig sc;
    sc.name = "garch";
    sc.enabled = true;
    sc.warmup_returns = 5;
    sc.annualization_factor = 1.0;   // keep sigma in return units for the test
    sc.min_edge = 1e-2;
    sc.garch_omega = 1e-8;
    sc.garch_alpha = 0.10;
    sc.garch_beta = 0.80;

    auto scanner = ScannerFactory::instance().createScanner("GarchScanner", sc);
    ASSERT_TRUE(scanner != nullptr);
    scanner->setEnabled(true);
    scanner->onStart();

    // Feed alternating returns; after warmup the GARCH state must be ready.
    double px = 100.0;
    for (int i = 0; i < 20; ++i) {
        px *= (i % 2 == 0) ? 1.01 : 0.99;
        scanner->onUnderlyingUpdate(px);
    }
    auto* garch = dynamic_cast<GarchScanner*>(scanner.get());
    ASSERT_TRUE(garch != nullptr);
    ASSERT_TRUE(garch->volReady());
    ASSERT_TRUE(garch->forecastVol() > 0.0);

    // GARCH vs very low IV → positive vega-weighted edge.
    HitCounter hits;
    scanner->addListener(&hits);
    auto opt = makeOption(9.0, 11.0, 10.0, 0.5, 0.001, 100.0);
    scanner->onOptionUpdate(opt.get());
    ASSERT_EQ(hits.hits, 1);
    ASSERT_EQ(hits.last.reason, "garch_vol_edge");

    std::cout << "  PASS" << std::endl;
}

struct MockScanner : IScanModule {
    int starts = 0, stops = 0, panics = 0;
    int option_updates = 0, underlying_updates = 0;
    void onStart() override { ++starts; }
    void onStop() override { ++stops; }
    void onPanic() override { ++panics; }
    void onOptionUpdate(OptionData*) override { ++option_updates; }
    void onUnderlyingUpdate(double) override { ++underlying_updates; }
};

void test_scan_coordinator()
{
    std::cout << "=== test_scan_coordinator ===" << std::endl;
    OptionScanCoordinator coord;
    auto mock = std::make_shared<MockScanner>();
    coord.add(mock);
    ASSERT_EQ(coord.size(), 1u);

    coord.startAll();
    ASSERT_EQ(mock->starts, 1);

    // Disabled scanners are skipped by the coordinator.
    mock->setEnabled(false);
    auto opt = makeOption(9.0, 11.0, 12.0, 0.5);
    coord.dispatchOption(opt.get());
    coord.dispatchUnderlying(100.0);
    ASSERT_EQ(mock->option_updates, 0);
    ASSERT_EQ(mock->underlying_updates, 0);

    mock->setEnabled(true);
    mock->setExpiryEnabled(202612, false);
    coord.dispatchOption(opt.get());
    ASSERT_EQ(mock->option_updates, 0);   // per-expiry override honored
    mock->setExpiryEnabled(202612, true);
    coord.dispatchOption(opt.get());
    coord.dispatchUnderlying(100.0);
    ASSERT_EQ(mock->option_updates, 1);
    ASSERT_EQ(mock->underlying_updates, 1);

    coord.panicAll();
    ASSERT_EQ(mock->panics, 1);
    coord.stopAll();
    ASSERT_EQ(mock->stops, 1);
    std::cout << "  PASS" << std::endl;
}

void test_expiry_lifecycle()
{
    std::cout << "=== test_expiry_lifecycle ===" << std::endl;
    std::unordered_map<std::string, double> pos = {{"A", 3}, {"B", -4}, {"C", 0}};
    ASSERT_TRUE(std::fabs(OptionExpiryLifecycle::grossPosition(pos) - 7.0) < 1e-9);
    // Null ctx + no alert must be a no-op (backtest/test safety).
    OptionExpiryLifecycle::onSessionEnd(nullptr, pos);
    double seen = 0;
    OptionExpiryLifecycle::onSessionEnd(nullptr, pos,
        [&](double gross, size_t codes) { seen = gross + codes; });
    ASSERT_TRUE(std::fabs(seen - 10.0) < 1e-9);   // 7 + 3 codes
    std::cout << "  PASS" << std::endl;
}

} // namespace

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "WtOptionCore Scanner Acceptance Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    test_factory_and_edge_scanners();
    test_scanner_numeric_config();
    test_garch_scanner();
    test_scan_coordinator();
    test_expiry_lifecycle();
    std::cout << "\nALL TESTS PASSED" << std::endl;
    return 0;
}
