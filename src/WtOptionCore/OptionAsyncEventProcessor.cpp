/*!
 * \file OptionAsyncEventProcessor.cpp
 * \brief Async Event Processor Implementation (Phase 2 — Full Async Layer)
 *
 * Worker loop: batch drain → tick dedup → process events → batch complete callback
 * All session events also go through queue (fixes WtOptEngine sync session bug)
 */
#include "OptionAsyncEventProcessor.h"

#include "../Includes/WTSDataDef.hpp"
#include "../WTSTools/WTSLogger.h"
#include "../Share/TimeUtils.hpp"

#include <algorithm>

namespace wt_option {

//=============================================================================
// AsyncEvent factory methods
//=============================================================================

OptionAsyncEventProcessor::AsyncEvent
OptionAsyncEventProcessor::AsyncEvent::make_tick(const char* code, WTSTickData* t)
{
    AsyncEvent ev;
    ev.type = Tick;
    ev.setCode(code);
    if (t) {
        ev.tick.price = t->price();
        ev.tick.bid = t->bidprice(0);
        ev.tick.ask = t->askprice(0);
        ev.tick.bidQty = t->bidqty(0);
        ev.tick.askQty = t->askqty(0);
        ev.tick.actionTime = t->actiontime();
        ev.tick.updateTime = TimeUtils::getLocalTimeNow();
    }
    return ev;
}

OptionAsyncEventProcessor::AsyncEvent
OptionAsyncEventProcessor::AsyncEvent::make_timer(uint32_t d, uint32_t t)
{
    AsyncEvent ev;
    ev.type = Timer;
    ev.timer.date = d;
    ev.timer.time = t;
    return ev;
}

OptionAsyncEventProcessor::AsyncEvent
OptionAsyncEventProcessor::AsyncEvent::make_trade(const char* code, uint32_t id, bool buy, double v, double p)
{
    AsyncEvent ev;
    ev.type = Trade;
    ev.setCode(code);
    ev.trade.localid = id;
    ev.trade.isBuy = buy;
    ev.trade.vol = v;
    ev.trade.price = p;
    return ev;
}

OptionAsyncEventProcessor::AsyncEvent
OptionAsyncEventProcessor::AsyncEvent::make_order(const char* code, uint32_t id, bool buy,
                                                   double tq, double lq, double p, bool canc)
{
    AsyncEvent ev;
    ev.type = Order;
    ev.setCode(code);
    ev.order.localid = id;
    ev.order.isBuy = buy;
    ev.order.totalQty = tq;
    ev.order.leftQty = lq;
    ev.order.price = p;
    ev.order.isCanceled = canc;
    return ev;
}

OptionAsyncEventProcessor::AsyncEvent
OptionAsyncEventProcessor::AsyncEvent::make_session(uint32_t tdate, bool isBegin)
{
    AsyncEvent ev;
    ev.type = Session;
    ev.session.isBegin = isBegin;
    ev.session.tdate = tdate;
    return ev;
}

//=============================================================================
// Constructor / Destructor
//=============================================================================

OptionAsyncEventProcessor::OptionAsyncEventProcessor()
    : _worker_running(false)
{
}

OptionAsyncEventProcessor::~OptionAsyncEventProcessor()
{
    stop();
}

//=============================================================================
// Control
//=============================================================================

void OptionAsyncEventProcessor::start()
{
    if (_worker_running) return;
    _worker_running = true;
    _worker = std::thread(&OptionAsyncEventProcessor::worker_loop, this);
    WTSLogger::log_by_cat("strategy", LL_INFO, "OptionAsyncEventProcessor started");
}

void OptionAsyncEventProcessor::stop()
{
    if (!_worker_running) return;
    _worker_running = false;
    _worker_cv.notify_all();
    if (_worker.joinable()) _worker.join();
    WTSLogger::log_by_cat("strategy", LL_INFO, "OptionAsyncEventProcessor stopped");
}

//=============================================================================
// Producer interface (non-blocking, called from CTP callback threads)
//=============================================================================

void OptionAsyncEventProcessor::enqueue_tick(const char* stdCode, WTSTickData* newTick)
{
    if (!_worker_running) return;
    {
        std::lock_guard<std::mutex> lock(_queue_mtx);
        if (_task_queue.size() >= MAX_QUEUE_SIZE) {
            _queue_drops.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        _task_queue.push_back(AsyncEvent::make_tick(stdCode, newTick));
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_timer(uint32_t curDate, uint32_t curTime)
{
    if (!_worker_running) return;
    {
        std::lock_guard<std::mutex> lock(_queue_mtx);
        if (_task_queue.size() >= MAX_QUEUE_SIZE) {
            _queue_drops.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        _task_queue.push_back(AsyncEvent::make_timer(curDate, curTime));
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
    if (!_worker_running) return;
    {
        std::lock_guard<std::mutex> lock(_queue_mtx);
        if (_task_queue.size() >= MAX_QUEUE_SIZE) {
            _queue_drops.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        _task_queue.push_back(AsyncEvent::make_trade(stdCode, localid, isBuy, vol, price));
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_order(uint32_t localid, const char* stdCode, bool isBuy,
                                               double totalQty, double leftQty, double price, bool isCanceled)
{
    if (!_worker_running) return;
    {
        std::lock_guard<std::mutex> lock(_queue_mtx);
        if (_task_queue.size() >= MAX_QUEUE_SIZE) {
            _queue_drops.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        _task_queue.push_back(AsyncEvent::make_order(stdCode, localid, isBuy, totalQty, leftQty, price, isCanceled));
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_session(uint32_t tdate, bool isBegin)
{
    if (!_worker_running) return;
    {
        std::lock_guard<std::mutex> lock(_queue_mtx);
        if (_task_queue.size() >= MAX_QUEUE_SIZE) {
            _queue_drops.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        _task_queue.push_back(AsyncEvent::make_session(tdate, isBegin));
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

//=============================================================================
// Worker loop — batch drain + tick dedup + process + batch complete
//=============================================================================

void OptionAsyncEventProcessor::worker_loop()
{
    while (_worker_running) {
        // 1. Drain queue (batch up to 1024 events)
        std::vector<AsyncEvent> events;
        events.reserve(128);
        {
            std::unique_lock<std::mutex> lock(_queue_mtx);
            _worker_cv.wait(lock, [this]() {
                return !_worker_running || !_task_queue.empty();
            });
            if (!_worker_running) break;

            while (!_task_queue.empty() && events.size() < 1024) {
                events.push_back(std::move(_task_queue.front()));
                _task_queue.pop_front();
            }
        }

        if (events.empty()) continue;

        // 2. Process non-tick events first (session/timer/trade/order)
        bool has_tick = false;
        for (auto& ev : events) {
            switch (ev.type) {
                case AsyncEvent::Session:
                    if (_cbs.on_session)
                        _cbs.on_session(ev.session.tdate, ev.session.isBegin);
                    break;
                case AsyncEvent::Timer:
                    if (_cbs.on_timer)
                        _cbs.on_timer(ev.timer.date, ev.timer.time);
                    break;
                case AsyncEvent::Trade:
                    if (_cbs.on_trade)
                        _cbs.on_trade(ev.getCode(), ev.trade.localid, ev.trade.isBuy,
                                      ev.trade.vol, ev.trade.price);
                    break;
                case AsyncEvent::Order:
                    if (_cbs.on_order)
                        _cbs.on_order(ev.getCode(), ev.order.localid, ev.order.isBuy,
                                      ev.order.totalQty, ev.order.leftQty,
                                      ev.order.price, ev.order.isCanceled);
                    break;
                case AsyncEvent::Tick:
                    has_tick = true;
                    break;
                default:
                    break;
            }
        }

        // 3. Batch tick processing with deduplication
        if (has_tick) {
            // Dedup: keep only latest tick per code
            std::map<std::string, const TickData*> active_ticks;
            for (auto& ev : events) {
                if (ev.type == AsyncEvent::Tick) {
                    active_ticks[ev.getCode()] = &ev.tick; // last one wins
                }
            }

            // Batch start callback (before individual ticks)
            if (_cbs.on_tick_batch)
                _cbs.on_tick_batch();

            // Per-code tick callback (deduped — each code called once with latest)
            for (auto& [code, tick] : active_ticks) {
                if (_cbs.on_tick)
                    _cbs.on_tick(code, *tick);
            }

            // Batch complete: risk update + order update (single pass, once)
            if (_cbs.on_batch_complete)
                _cbs.on_batch_complete();
        }

        // 4. Queue saturation monitor
        size_t queue_size;
        {
            std::lock_guard<std::mutex> lock(_queue_mtx);
            queue_size = _task_queue.size();
        }
        if (queue_size > 3200) { // 78% of 4096
            uint64_t now = TimeUtils::getLocalTimeNow();
            if (now - _last_warning_time > 5000000) { // 5s throttle
                WTSLogger::log_by_cat("strategy", LL_WARN,
                    "OPTION ASYNC QUEUE BOTTLENECK! size={}, drops={}",
                    queue_size, _queue_drops.load(std::memory_order_relaxed));
                _last_warning_time = now;
            }
        }
    }
}

} // namespace wt_option
