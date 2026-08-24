/*!
 * \file OptionAsyncEventProcessor.h
 * \brief Async Event Processor for Option Market-Making
 *
 * P0-A fix: Double-queue architecture to fix SPSC multi-producer UB.
 * - _md_queue (SPSC): tick + timer events from MD callback thread (single producer)
 * - _trader_queue (mutex vector): trade/order/position/session/channel events
 *   from trader SPI thread and/or framework main thread (multi-producer safe)
 *
 * Worker drains both queues into a unified batch, preserving the original
 * processing order: session → channel → position → trade → order → timer → tick.
 */
#pragma once

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <string>
#include <string_view>
#include <functional>

#include <boost/lockfree/spsc_queue.hpp>

#include "../Includes/WTSMarcos.h"

namespace wtp { class WTSTickData; }
using wtp::WTSTickData;

namespace wt_option {

struct TickData {
    double price = 0;
    double bid = 0;
    double ask = 0;
    double bidQty = 0;
    double askQty = 0;
    double preClose = 0;
    double tradeVolume = 0;
    uint32_t actionTime = 0;
    uint64_t updateTime = 0;
    uint32_t expireDate = 0;   // B28: exact expiry YYYYMMDD (0 = unknown)
};

struct SessionEvent {
    enum Type { Begin, End } type;
    uint32_t tdate = 0;
};

struct AsyncCallbacks {
    std::function<void()> on_tick_batch;
    std::function<void(const std::string& code, const TickData& tick)> on_tick;
    std::function<void(const std::string& code, uint32_t localid, bool isBuy, double vol, double price)> on_trade;
    std::function<void(const std::string& code, uint32_t localid, bool isBuy,
                       double totalQty, double leftQty, double price, bool isCanceled)> on_order;
    std::function<void(uint32_t curDate, uint32_t curTime)> on_timer;
    std::function<void(uint32_t tdate, bool isBegin)> on_session;
    std::function<void(const std::string& code, bool isLong,
                       double prevol, double preavail,
                       double newvol, double newavail)> on_position;
    std::function<void(bool isReady)> on_channel;
    std::function<void()> on_batch_complete;
};

class OptionAsyncEventProcessor
{
public:
    OptionAsyncEventProcessor();
    ~OptionAsyncEventProcessor();

    void start();
    void stop();
    void setCallbacks(const AsyncCallbacks& cbs) { _cbs = cbs; }

    // MD thread producers (single-producer SPSC)
    void enqueue_tick(const char* stdCode, WTSTickData* newTick);
    void enqueue_timer(uint32_t curDate, uint32_t curTime);

    // Trader/main thread producers (multi-producer, mutex-protected)
    void enqueue_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price);
    void enqueue_order(uint32_t localid, const char* stdCode, bool isBuy,
                       double totalQty, double leftQty, double price, bool isCanceled);
    void enqueue_session(uint32_t tdate, bool isBegin);
    void enqueue_position(const char* stdCode, bool isLong,
                          double prevol, double preavail,
                          double newvol, double newavail);
    void enqueue_channel(bool isReady);

    uint64_t totalEvents() const { return _total_events.load(std::memory_order_relaxed); }
    uint64_t queueDrops() const { return _queue_drops.load(std::memory_order_relaxed); }

public:
    // AsyncEvent is public so factory methods can be defined in .cpp
    struct AsyncEvent {
        enum Type { Tick, Timer, Trade, Order, Session, Position, Channel, Custom } type;
        uint32_t optId;

        TickData tick;
        struct { uint32_t date; uint32_t time; } timer;
        struct { uint32_t localid; bool isBuy; double vol; double price; } trade;
        struct { uint32_t localid; bool isBuy; double totalQty; double leftQty; double price; bool isCanceled; } order;
        struct { bool isBegin; uint32_t tdate; } session;
        struct { bool isLong; double prevol; double preavail; double newvol; double newavail; } position;
        struct { bool isReady; } channel;
        char code[32];

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
    static AsyncEvent make_position(const char* stdCode, bool isLong,
                                    double prevol, double preavail,
                                    double newvol, double newavail);
    static AsyncEvent make_channel(bool isReady);
    };

    // === MD queue: SPSC (single producer = MD thread) ===
    static constexpr size_t QUEUE_CAPACITY = 4096;
    mutable boost::lockfree::spsc_queue<AsyncEvent, boost::lockfree::capacity<QUEUE_CAPACITY>> _md_queue;

    // MD overflow fallback (rare: ring full)
    std::mutex _md_overflow_mtx;
    std::vector<AsyncEvent> _md_overflow;

    // === Trader queue: mutex-protected (multi-producer safe) ===
    // Trader events (trade/order/position/session/channel) are ~100-500/s,
    // negligible compared to tick ~10000/s. Mutex contention is near-zero.
    std::mutex _trader_mtx;
    mutable std::vector<AsyncEvent> _trader_queue;

    // Worker thread
    std::thread _worker;
    std::atomic<bool> _worker_running;
    std::condition_variable _worker_cv;
    std::mutex _worker_sleep_mtx;  // mutex for cv wait only (not for queue access)

    // Statistics
    std::atomic<uint64_t> _queue_drops{0};
    std::atomic<uint64_t> _total_events{0};
    uint64_t _last_warning_time{0};

    // Callbacks
    AsyncCallbacks _cbs;

    // Worker loop
    void worker_loop();

    // Helper: check if any queue has events
    bool has_events() const;
};

using OptionAsyncEventProcessorPtr = std::shared_ptr<OptionAsyncEventProcessor>;

} // namespace wt_option
