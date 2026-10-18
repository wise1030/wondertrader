// Phase 2: OptionCommandHandlers shell seam.
#include "../OptionCommandHandlers.h"
#include "../OptionPricerMode.h"
#include <iostream>
#include <cstdlib>

using namespace wt_option;

#define ASSERT_TRUE(x) do { if (!(x)) { std::cerr << "FAIL: " #x << std::endl; std::exit(1); } } while (0)
#define ASSERT_EQ(a,b) do { auto _a=(a); auto _b=(b); if (!(_a==_b)) { \
    std::cerr << "FAIL: " #a " != " #b << std::endl; std::exit(1); } } while (0)

int main()
{
    std::cout << "WtOptionCore Command Handler Tests" << std::endl;

    CommandBus bus;
    OptionCommandHandlers handlers;
    int stops = 0, panics = 0, resumes = 0;
    int qmode_calls = 0, order_calls = 0, cancel_calls = 0;
    std::string last_mode, last_cancel;
    bool channel_ready = true;

    OptionCommandHandlers::Hooks hooks;
    hooks.setState = [&](TradingStateType t, const std::string&) {
        if (t == TradingStateType::Disabled) ++stops;
        else if (t == TradingStateType::Panic) ++panics;
        else if (t == TradingStateType::Normal) ++resumes;
    };
    hooks.channelReady = [&]() { return channel_ready; };
    hooks.setQModeAll = [&](const std::string& mode, std::string& err) {
        if (mode != "on" && mode != "off" && mode != "close") {
            err = "bad mode";
            return false;
        }
        ++qmode_calls;
        last_mode = mode;
        return true;
    };
    hooks.executeOrder = [&](const std::string&, bool, double, uint32_t) {
        ++order_calls;
        return 1;
    };
    hooks.cancelAll = [&](const std::string& code) { ++cancel_calls; last_cancel = code; };
    handlers.registerAll(bus, hooks);

    std::string err;
    ASSERT_TRUE(bus.dispatch("STOP", err));
    ASSERT_EQ(stops, 1);
    ASSERT_TRUE(bus.dispatch("PANIC", err));
    ASSERT_EQ(panics, 1);
    ASSERT_TRUE(bus.dispatch("RESUME", err));
    ASSERT_EQ(resumes, 1);
    channel_ready = false;
    ASSERT_TRUE(bus.dispatch("RESUME", err));
    ASSERT_EQ(stops, 2);   // not ready -> Disabled

    ASSERT_TRUE(bus.dispatch("QMODE,close", err));
    ASSERT_EQ(qmode_calls, 1);
    ASSERT_TRUE(last_mode == "close");
    ASSERT_TRUE(!bus.dispatch("QMODE,bad", err));

    ASSERT_TRUE(bus.dispatch("ORDER,B,cu2600,45000,2", err));
    ASSERT_EQ(order_calls, 1);
    ASSERT_TRUE(!bus.dispatch("ORDER,B,cu2600,notanumber,2", err));

    ASSERT_TRUE(bus.dispatch("CANCEL,cu2600", err));
    ASSERT_EQ(cancel_calls, 1);
    ASSERT_TRUE(last_cancel == "cu2600");

    // Composite and Gamma are independent strategy pricers, not duplicates.
    OptionPricerMode mode = OptionPricerMode::CompositeMM;
    ASSERT_TRUE(parseOptionPricerMode("composite_mm", mode));
    ASSERT_TRUE(mode == OptionPricerMode::CompositeMM);
    ASSERT_TRUE(parseOptionPricerMode("gammascalp", mode));
    ASSERT_TRUE(mode == OptionPricerMode::GammaScalp);
    ASSERT_TRUE(!parseOptionPricerMode("unknown", mode));

    std::cout << "  PASS" << std::endl;
    std::cout << "\nALL TESTS PASSED" << std::endl;
    return 0;
}
