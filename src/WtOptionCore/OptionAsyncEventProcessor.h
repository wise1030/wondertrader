/*!
 * \file OptionAsyncEventProcessor.h
 * \brief Async Event Processor for Option Market-Making
 *
 * Extracted from WtOptContext's queue + worker_loop pattern.
 * All events (tick/trade/order/timer/session) go through a queue,
 * processed in batch by a single worker thread with tick deduplication.
 *
 * Key design (preserved from WtOptContext):
 * 1. Callback threads (CTP md/trader) only enqueue, zero blocking
 * 2. Worker drains batch, dedup ticks by code (last wins), process once
 * 3. After batch: risk update once + order update once (single pass)
 * 4. Queue saturation monitor (78% threshold)
 *
 * Phase 2: Full async layer with callback hooks
 * Phase 3: Business integration (pricing/risk/orders in worker)
 */
#pragma once

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <map>
#include <deque>
#include <cstdint>
#include <string>
#include <functional>

#include "../Includes/WTSMarcos.h"

namespace wtp { class WTSTickData; }
using wtp::WTSTickData;

namespace wt_option {

// Lightweight tick data for queue (slimmed from ~300B WTSTickStruct to ~64B)
struct TickData {
    double price = 0;
    double bid = 0;
    double ask = 0;
    double bidQty = 0;
    double askQty = 0;
    uint32_t actionTime = 0;
    uint64_t updateTime = 0;
};

// Session event data
struct SessionEvent {
    enum Type { Begin, End } type;
    uint32_t tdate = 0;
};

// Callbacks interface — strategy implements these, processor calls them in worker thread
struct AsyncCallbacks {
    // Batch start: all ticks drained + deduped, before individual on_tick calls
    std::function<void()> on_tick_batch;

    // Per-code tick (called for each code that had an update, with latest tick)
    std::function<void(const std::string& code, const TickData& tick)> on_tick;

    // Trade fill
    std::function<void(const std::string& code, uint32_t localid, bool isBuy, double vol, double price)> on_trade;

    // Order status update
    std::function<void(const std::string& code, uint32_t localid, bool isBuy,
                       double totalQty, double leftQty, double price, bool isCanceled)> on_order;

    // Timer
    std::function<void(uint32_t curDate, uint32_t curTime)> on_timer;

    // Session begin/end
    std::function<void(uint32_t tdate, bool isBegin)> on_session;

    // Post-batch: risk update + order update (single pass, once per batch)
    std::function<void()> on_batch_complete;
};

class OptionAsyncEventProcessor
{
public:
    OptionAsyncEventProcessor();
    ~OptionAsyncEventProcessor();

    // Control
    void start();
    void stop();

    // Set callbacks (must be called before start)
    void setCallbacks(const AsyncCallbacks& cbs) { _cbs = cbs; }

    // Producer interface (called from CTP callback threads, non-blocking)
    void enqueue_tick(const char* stdCode, WTSTickData* newTick);
    void enqueue_timer(uint32_t curDate, uint32_t curTime);
    void enqueue_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price);
    void enqueue_order(uint32_t localid, const char* stdCode, bool isBuy,
                       double totalQty, double leftQty, double price, bool isCanceled);
    void enqueue_session(uint32_t tdate, bool isBegin);

    // Statistics
    uint64_t totalEvents() const { return _total_events.load(std::memory_order_relaxed); }
    uint64_t queueDrops() const { return _queue_drops.load(std::memory_order_relaxed); }

private:
    // Async event — fixed-size for trivially copyable (enables future lock-free)
    // code uses char[32] instead of std::string to avoid heap allocation
    struct AsyncEvent {
        enum Type { Tick, Timer, Trade, Order, Session, Custom } type;
        uint32_t optId; // UINT32_MAX = underlying or non-option

        TickData tick;
        struct { uint32_t date; uint32_t time; } timer;
        struct { uint32_t localid; bool isBuy; double vol; double price; } trade;
        struct { uint32_t localid; bool isBuy; double totalQty; double leftQty; double price; bool isCanceled; } order;
        struct { bool isBegin; uint32_t tdate; } session;
        char code[32]; // fixed-length code (replaces std::string)

        AsyncEvent() : type(Custom), optId(UINT32_MAX) { code[0] = '\0'; }

        void setCode(const char* s) {
            strncpy(code, s ? s : "", 31);
            code[31] = '\0';
        }
        std::string getCode() const { return code; }

        static AsyncEvent make_tick(const char* stdCode, WTSTickData* t);
        static AsyncEvent make_timer(uint32_t d, uint32_t t);
        static AsyncEvent make_trade(const char* stdCode, uint32_t id, bool buy, double v, double p);
        static AsyncEvent make_order(const char* stdCode, uint32_t id, bool buy,
                                      double tq, double lq, double p, bool canc);
        static AsyncEvent make_session(uint32_t tdate, bool isBegin);
    };

    // Queue (mutex-protected deque, Phase 2 keeps for correctness)
    std::mutex _queue_mtx;
    std::deque<AsyncEvent> _task_queue;
    static constexpr size_t MAX_QUEUE_SIZE = 4096;

    // Worker thread
    std::thread _worker;
    std::atomic<bool> _worker_running;
    std::condition_variable _worker_cv;

    // Statistics
    std::atomic<uint64_t> _queue_drops{0};
    std::atomic<uint64_t> _total_events{0};
    uint64_t _last_warning_time{0};

    // Callbacks (set by strategy before start)
    AsyncCallbacks _cbs;

    // Worker loop
    void worker_loop();
};

using OptionAsyncEventProcessorPtr = std::shared_ptr<OptionAsyncEventProcessor>;

} // namespace wt_option
