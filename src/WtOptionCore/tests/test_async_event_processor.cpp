// Phase-0 regressions for OptionAsyncEventProcessor event integrity.
#include "../OptionAsyncEventProcessor.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

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

// MD overflow must stay bounded while retaining the newest tick per contract.
void test_md_overflow_coalesces_ticks_by_code() {
    std::cout << "=== test_md_overflow_coalesces_ticks_by_code ===" << std::endl;

    OptionAsyncEventProcessor proc;
    for (size_t i = 0; i < OptionAsyncEventProcessor::kMaxOverflow; ++i)
        proc.push_md_overflow(OptionAsyncEventProcessor::AsyncEvent::make_timer(20261030, 900));

    ASSERT_EQ(proc._md_overflow.size(), OptionAsyncEventProcessor::kMaxOverflow);

    auto tick1 = OptionAsyncEventProcessor::AsyncEvent::make_control();
    tick1.type = OptionAsyncEventProcessor::AsyncEvent::Tick;
    tick1.setCode("SHFE.ag2601");
    tick1.tick.bid = 1.0;
    proc.push_md_overflow(std::move(tick1));
    ASSERT_EQ(proc._md_overflow.size(), OptionAsyncEventProcessor::kMaxOverflow);
    ASSERT_EQ(proc.queueDrops(), 1u);

    auto tick2 = OptionAsyncEventProcessor::AsyncEvent::make_control();
    tick2.type = OptionAsyncEventProcessor::AsyncEvent::Tick;
    tick2.setCode("SHFE.ag2601");
    tick2.tick.bid = 2.0;
    proc.push_md_overflow(std::move(tick2));
    ASSERT_EQ(proc._md_overflow.size(), OptionAsyncEventProcessor::kMaxOverflow);
    ASSERT_EQ(proc.queueDrops(), 1u);  // coalesced, not dropped

    bool found = false;
    for (const auto& ev : proc._md_overflow) {
        if (ev.type == OptionAsyncEventProcessor::AsyncEvent::Tick &&
            ev.getCode() == "SHFE.ag2601") {
            ASSERT_EQ(ev.tick.bid, 2.0);
            found = true;
        }
    }
    ASSERT_TRUE(found);
    std::cout << "  PASS" << std::endl;
}

// Trader events must retain enqueue order; position-before-trade reordering
// breaks PositionBook snapshot/fill bridging.
void test_trader_events_preserve_fifo_order() {
    std::cout << "=== test_trader_events_preserve_fifo_order ===" << std::endl;

    OptionAsyncEventProcessor proc;
    std::vector<int> order;
    std::atomic<uint32_t> done{0};

    AsyncCallbacks cbs;
    cbs.on_trade = [&](const std::string&, uint32_t, bool, double, double) {
        order.push_back(1);
        done.fetch_add(1);
    };
    cbs.on_position = [&](const std::string&, bool, double, double, double, double) {
        order.push_back(2);
        done.fetch_add(1);
    };
    proc.setCallbacks(cbs);
    proc.start();

    proc.enqueue_trade(1, "A", true, 1, 10.0);
    proc.enqueue_position("A", true, 0, 0, 1, 1);
    ASSERT_TRUE(wait_until(done, 2));
    proc.stop();

    ASSERT_EQ(order.size(), (size_t)2);
    ASSERT_EQ(order[0], 1);
    ASSERT_EQ(order[1], 2);
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

// D: control events (session/channel/entrust) must NEVER be dropped on a
// full trader queue — push_trader bypasses the capacity gate for them.
// Regression: before the fix a channel_lost could be discarded at 65536+
// depth, leaving the strategy quoting on a dead channel.
void test_control_events_survive_full_queue() {
    std::cout << "=== test_control_events_survive_full_queue ===" << std::endl;

    OptionAsyncEventProcessor proc;
    std::atomic<uint32_t> channelCount{0}, entrustCount{0};
    AsyncCallbacks cbs;
    cbs.on_channel = [&](bool) { channelCount.fetch_add(1); };
    cbs.on_entrust = [&](uint32_t, bool) { entrustCount.fetch_add(1); };
    proc.setCallbacks(cbs);

    // Fill the bounded trader queue with the worker STOPPED: trade events
    // saturate the 65536 cap and start failing, control events must not.
    for (size_t i = 0; i < OptionAsyncEventProcessor::kMaxTraderQueue; ++i)
        ASSERT_TRUE(proc.push_trader(OptionAsyncEventProcessor::AsyncEvent::make_trade(
            "SHFE.cu2607", 1000 + i, true, 1, 50000.0)));
    ASSERT_TRUE(!proc.push_trader(OptionAsyncEventProcessor::AsyncEvent::make_trade(
        "SHFE.cu2607", 9999, true, 1, 50000.0)));   // cap reached: trader event refused

    // Control events still accepted at max depth (worker not running yet).
    ASSERT_TRUE(proc.push_trader(OptionAsyncEventProcessor::AsyncEvent::make_channel(false)));
    ASSERT_TRUE(proc.push_trader(OptionAsyncEventProcessor::AsyncEvent::make_entrust(77, false)));
    ASSERT_TRUE(proc.push_trader(OptionAsyncEventProcessor::AsyncEvent::make_session(20260605, true)));

    // Now drain: worker processes everything FIFO.
    proc.start();
    for (int i = 0; i < 500; ++i) {
        if (channelCount.load() >= 1 && entrustCount.load() >= 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(channelCount.load(), 1u);
    ASSERT_EQ(entrustCount.load(), 1u);
    proc.stop();
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "WtOptionCore Async Event Processor Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_trader_suffix_requeued_exactly_once();
    test_md_overflow_coalesces_ticks_by_code();
    test_trader_events_preserve_fifo_order();
    test_control_event_wakes_quiet_worker();
    test_control_events_survive_full_queue();

    std::cout << "\nALL TESTS PASSED" << std::endl;
    return 0;
}
