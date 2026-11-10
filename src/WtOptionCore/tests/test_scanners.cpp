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
#include "../OptionGrid.h"
#include "../StrikeData.h"
#include "../ExpiryData.h"
#include "../Share/fmtlib.h"
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

        // Structural scanners need full grid context; isolated option updates
        // must not pretend to be a butterfly/calendar/parity signal.
        const bool structural = type == "ButterflyScanner" ||
                                type == "VolSpreadScanner" ||
                                type == "SyntheticFutureScanner";
        const int baseHits = structural ? 0 : 1;

        auto opt = makeOption(9.0, 11.0, 12.0, 0.5);
        scanner->onOptionUpdate(opt.get());
        ASSERT_EQ(hits.hits, baseHits);
        if (!structural) {
            ASSERT_TRUE(hits.last.option == opt.get());
            ASSERT_TRUE(!hits.last.reason.empty());
            ASSERT_TRUE(hits.last.signal > 0.0);
        }

        auto thin = makeOption(9.0, 11.0, 10.2, 0.5);
        scanner->onOptionUpdate(thin.get());
        ASSERT_EQ(hits.hits, baseHits);

        auto wing = makeOption(9.0, 11.0, 12.0, 0.95);
        scanner->onOptionUpdate(wing.get());
        ASSERT_EQ(hits.hits, baseHits);

        // Panic disables signal dispatch.
        scanner->onPanic();
        scanner->onOptionUpdate(opt.get());
        ASSERT_EQ(hits.hits, baseHits);

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
    auto scanner = ScannerFactory::instance().createScanner("LowBidsScanner", sc);
    ASSERT_TRUE(scanner != nullptr);
    scanner->setEnabled(true);
    scanner->onStart();

    HitCounter hits;
    scanner->addListener(&hits);
    auto opt = makeOption(9.0, 11.0, 12.0, 0.5);
    scanner->onOptionUpdate(opt.get());
    ASSERT_EQ(hits.hits, 0);    // min_edge override honored

    sc.min_edge = 0.5;
    auto scanner2 = ScannerFactory::instance().createScanner("LowBidsScanner", sc);
    scanner2->setEnabled(true);
    scanner2->onStart();
    HitCounter hits2;
    scanner2->addListener(&hits2);
    scanner2->onOptionUpdate(opt.get());
    ASSERT_EQ(hits2.hits, 1);

    std::cout << "  PASS" << std::endl;
}

// Real parity semantics: C-P+K vs underlying forward.
void test_synthetic_future_scanner_semantics()
{
    std::cout << "=== test_synthetic_future_scanner_semantics ===" << std::endl;
    auto grid = std::make_shared<OptionGrid>("IO", "CFFEX.000300", nullptr, nullptr);
    auto sd = grid->findOrCreateStrike(202612, 4000.0);
    auto ed = sd->getExpiryData();
    ed->setUnderlyingPrice(4000.0);

    OptionInfo ci; ci.code = "C4000"; ci.expiry = 202612; ci.strike = 4000; ci.right = OR_Call;
    OptionInfo pi; pi.code = "P4000"; pi.expiry = 202612; pi.strike = 4000; pi.right = OR_Put;
    auto call = std::make_shared<OptionData>(ci);
    auto put = std::make_shared<OptionData>(pi);
    OptionMarket cm, pm;
    cm.bid = 10.0; cm.ask = 12.0; call->updateMarket(cm);
    pm.bid = 9.0;  pm.ask = 11.0; put->updateMarket(pm);
    call->setExpiryData(ed); put->setExpiryData(ed);
    sd->setCall(call); sd->setPut(put);
    call->setStrikeData(sd); put->setStrikeData(sd);

    ScannerConfig sc; sc.name = "syn"; sc.enabled = true; sc.min_edge = 0.5;
    auto scanner = ScannerFactory::instance().createScanner("SyntheticFutureScanner", sc);
    scanner->setEnabled(true); scanner->onStart();
    HitCounter hits; scanner->addListener(&hits);

    // synthetic forward = 11 - 10 + 4000 = 4001, edge = 1
    scanner->onOptionUpdate(call.get());
    ASSERT_EQ(hits.hits, 1);
    ASSERT_TRUE(hits.last.reason.find("parity") != std::string::npos);
    ASSERT_TRUE(std::fabs(hits.last.signal - 1.0) < 1e-9);
    std::cout << "  PASS" << std::endl;
}

// Real butterfly semantics: lower - 2*middle + upper.
void test_butterfly_scanner_semantics()
{
    std::cout << "=== test_butterfly_scanner_semantics ===" << std::endl;
    auto grid = std::make_shared<OptionGrid>("IO", "CFFEX.000300", nullptr, nullptr);
    grid->getExpiryData(202612);

    auto mk = [&](uint32_t exp, double strike, double bid, double ask) {
        auto sd = grid->findOrCreateStrike(exp, strike);
        OptionInfo ii; ii.code = fmt::format("C{}", (int)strike); ii.expiry = exp; ii.strike = strike; ii.right = OR_Call;
        auto od = std::make_shared<OptionData>(ii);
        OptionMarket m; m.bid = bid; m.ask = ask; od->updateMarket(m);
        sd->setCall(od); od->setStrikeData(sd);
        return od;
    };
    mk(202612, 3900, 0.9, 1.1);   // mid 1
    mk(202612, 4000, 0.0, 0.0);   // invalid middle: no hit
    mk(202612, 4100, 1.5, 1.7);   // mid 1.6

    ScannerConfig sc; sc.name = "fly"; sc.enabled = true; sc.min_edge = 0.5;
    auto scanner = ScannerFactory::instance().createScanner("ButterflyScanner", sc);
    scanner->setEnabled(true); scanner->onStart();
    HitCounter hits; scanner->addListener(&hits);
    scanner->onTick(grid.get());
    ASSERT_EQ(hits.hits, 0);       // middle book missing

    // Fix middle book: fly = 1 - 2*1 + 1.6 = 0.6 → cheap short edge 0.6
    auto midSd = grid->findOrCreateStrike(202612, 4000);
    OptionInfo mi; mi.code = "C4000"; mi.expiry = 202612; mi.strike = 4000; mi.right = OR_Call;
    auto mid = std::make_shared<OptionData>(mi);
    OptionMarket mm; mm.bid = 0.8; mm.ask = 1.2; mid->updateMarket(mm);
    midSd->setCall(mid); mid->setStrikeData(midSd);
    scanner->onTick(grid.get());
    ASSERT_EQ(hits.hits, 1);
    ASSERT_TRUE(std::fabs(hits.last.signal - 0.6) < 1e-9);
    std::cout << "  PASS" << std::endl;
}

// Real vol-spread semantics: far-expiry ATM IV minus front ATM IV.
void test_volspread_scanner_semantics()
{
    std::cout << "=== test_volspread_scanner_semantics ===" << std::endl;
    auto grid = std::make_shared<OptionGrid>("IO", "CFFEX.000300", nullptr, nullptr);
    grid->setUnderlyingPrice(4000.0);

    auto mk = [&](uint32_t exp, double iv) {
        auto sd = grid->findOrCreateStrike(exp, 4000.0);
        auto ed = sd->getExpiryData();
        ed->setForwardReady(true); ed->setFitReady(true);
        OptionInfo ii; ii.code = fmt::format("C{}_{}", exp, (int)iv*100); ii.expiry = exp; ii.strike = 4000; ii.right = OR_Call;
        auto od = std::make_shared<OptionData>(ii);
        OptionMarket m; m.bid = 10; m.ask = 12; od->updateMarket(m);
        od->values(0).setImpliedVol(iv);
        sd->setCall(od); od->setStrikeData(sd);
    };
    mk(202612, 0.18);
    mk(202701, 0.22);

    ScannerConfig sc; sc.name = "volspread"; sc.enabled = true; sc.min_edge = 0.01;
    auto scanner = ScannerFactory::instance().createScanner("VolSpreadScanner", sc);
    scanner->setEnabled(true); scanner->onStart();
    HitCounter hits; scanner->addListener(&hits);
    scanner->onTick(grid.get());
    ASSERT_EQ(hits.hits, 1);
    ASSERT_TRUE(std::fabs(hits.last.signal - 0.04) < 1e-9);
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

    // Market-readiness: unpriced options are not dispatched (the pricer
    // marks values priced=false while an expiry has no accepted fit).
    opt->values(0).setPriced(false);
    coord.dispatchOption(opt.get());
    ASSERT_EQ(mock->option_updates, 0);
    opt->values(0).setPriced(true);

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
    test_synthetic_future_scanner_semantics();
    test_butterfly_scanner_semantics();
    test_volspread_scanner_semantics();
    test_garch_scanner();
    test_scan_coordinator();
    test_expiry_lifecycle();
    std::cout << "\nALL TESTS PASSED" << std::endl;
    return 0;
}
