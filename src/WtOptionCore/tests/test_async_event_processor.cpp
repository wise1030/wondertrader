// Phase-0 regressions for OptionAsyncEventProcessor event integrity.
#include "../OptionAsyncEventProcessor.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace wt_option;

#define ASSERT_TRUE(x) do { \
    if (!(x)) { \
        std::cerr << "FAIL: " #x << std::endl; \
        std::exit(1); \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    if (!(_va == _vb)) { \
        std::cerr << "FAIL: " #a "=" << _va << " expected " << _vb << std::endl; \
        std::exit(1); \
    } \
} while (0)

static bool wait_until(const std::atomic<uint32_t>& value, uint32_t target) {
    for (int i = 0; i < 500; ++i) {
        if (value.load() >= target)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return value.load() >= target;
}

// Regression: when MD fills 1023 of the 1024 batch slots, the trader suffix
// must be requeued without repeating the already consumed first trade.
void test_trader_suffix_requeued_exactly_once() {
    std::cout << "=== test_trader_suffix_requeued_exactly_once ===" << std::endl;

    OptionAsyncEventProcessor proc;
    std::atomic<uint32_t> timerCount{0};
    std::atomic<uint32_t> tradeCount{0};
    std::atomic<uint32_t> processedCount{0};

    AsyncCallbacks cbs;
    cbs.on_timer = [&](uint32_t, uint32_t) { timerCount.fetch_add(1); };
    cbs.on_trade = [&](const std::string&, uint32_t, bool, double, double) {
        tradeCount.fetch_add(1);
    };
    cbs.on_batch_complete = [&]() { processedCount.fetch_add(1); };
    proc.setCallbacks(cbs);
    proc.start();

    for (uint32_t i = 0; i < 1023; ++i)
        proc.enqueue_timer(20261030, 900 + i);
    proc.enqueue_trade(501, "SHFE.ag2601", true, 1, 4000.0);
    proc.enqueue_trade(502, "SHFE.ag2601", false, 1, 4010.0);
    proc.enqueue_trade(503, "SHFE.ag2601", true, 1, 4020.0);

    ASSERT_TRUE(wait_until(processedCount, 2));
    ASSERT_TRUE(wait_until(timerCount, 1023));
    ASSERT_TRUE(wait_until(tradeCount, 3));
    proc.stop();

    ASSERT_EQ(timerCount.load(), 1023u);
    ASSERT_EQ(tradeCount.load(), 3u);
    ASSERT_EQ(proc.queueDrops(), 0u);
    std::cout << "  PASS" << std::endl;
}

// Hot STOP/control must wake a completely quiet worker.
void test_control_event_wakes_quiet_worker() {
    std::cout << "=== test_control_event_wakes_quiet_worker ===" << std::endl;

    OptionAsyncEventProcessor proc;
    std::atomic<uint32_t> batchCount{0};
    AsyncCallbacks cbs;
    cbs.on_batch_complete = [&]() { batchCount.fetch_add(1); };
    proc.setCallbacks(cbs);
    proc.start();

    proc.enqueue_control();
    ASSERT_TRUE(wait_until(batchCount, 1));
    proc.stop();
    ASSERT_EQ(batchCount.load(), 1u);
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "WtOptionCore Async Event Processor Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_trader_suffix_requeued_exactly_once();
    test_control_event_wakes_quiet_worker();

    std::cout << "\nALL TESTS PASSED" << std::endl;
    return 0;
}
