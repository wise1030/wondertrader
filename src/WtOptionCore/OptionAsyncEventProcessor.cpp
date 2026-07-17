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
        ev.tick.preClose = t->preclose();
        ev.tick.tradeVolume = t->volume();  // trade volume for vega/delta flow
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

OptionAsyncEventProcessor::AsyncEvent
OptionAsyncEventProcessor::AsyncEvent::make_position(const char* code, bool isLong, double newvol)
{
    AsyncEvent ev;
    ev.type = Position;
    ev.setCode(code);
    ev.position.isLong = isLong;
    ev.position.newvol = newvol;
    return ev;
}

OptionAsyncEventProcessor::AsyncEvent
OptionAsyncEventProcessor::AsyncEvent::make_channel(bool isReady)
{
    AsyncEvent ev;
    ev.type = Channel;
    ev.channel.isReady = isReady;
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
// Producer interface (lock-free, called from CTP callback threads)
//=============================================================================

inline bool OptionAsyncEventProcessor::lf_push(const AsyncEvent& ev)
{
    // Try lock-free push first (wait-free, no allocation)
    if (_lf_queue.push(ev))
        return true;
    // Ring buffer full -> fall back to overflow queue under mutex
    {
        std::lock_guard<std::mutex> lock(_overflow_mtx);
        _overflow_queue.push_back(ev);
    }
    return true;
}

void OptionAsyncEventProcessor::enqueue_tick(const char* stdCode, WTSTickData* newTick)
{
    if (!_worker_running) return;
    if (!lf_push(AsyncEvent::make_tick(stdCode, newTick))) {
        _queue_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_timer(uint32_t curDate, uint32_t curTime)
{
    if (!_worker_running) return;
    if (!lf_push(AsyncEvent::make_timer(curDate, curTime))) {
        _queue_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
    if (!_worker_running) return;
    if (!lf_push(AsyncEvent::make_trade(stdCode, localid, isBuy, vol, price))) {
        _queue_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_order(uint32_t localid, const char* stdCode, bool isBuy,
                                               double totalQty, double leftQty, double price, bool isCanceled)
{
    if (!_worker_running) return;
    if (!lf_push(AsyncEvent::make_order(stdCode, localid, isBuy, totalQty, leftQty, price, isCanceled))) {
        _queue_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_session(uint32_t tdate, bool isBegin)
{
    if (!_worker_running) return;
    lf_push(AsyncEvent::make_session(tdate, isBegin));
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_position(const char* stdCode, bool isLong, double newvol)
{
    if (!_worker_running) return;
    lf_push(AsyncEvent::make_position(stdCode, isLong, newvol));
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_channel(bool isReady)
{
    if (!_worker_running) return;
    lf_push(AsyncEvent::make_channel(isReady));
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

//=============================================================================
// Worker loop — batch drain + tick dedup + process + batch complete
//=============================================================================

void OptionAsyncEventProcessor::worker_loop()
{
    // Pre-allocate reusable event buffer to eliminate per-batch heap allocation
    std::vector<AsyncEvent> events;
    events.reserve(256);

    while (_worker_running) {
        events.clear();

        // Wait for events using condition variable (no mutex on the queue itself)
        {
            std::unique_lock<std::mutex> lock(_overflow_mtx);
            _worker_cv.wait_for(lock, std::chrono::microseconds(100), [this]() {
                return !_worker_running || !_lf_queue.empty() || !_overflow_queue.empty();
            });
        }
        if (!_worker_running) break;

        // Drain lock-free queue using consume_one (wait-free, no mutex)
        while (events.size() < 1024) {
            bool got = _lf_queue.consume_one([&events](AsyncEvent& ev) {
                events.push_back(std::move(ev));
            });
            if (!got) break;
        }

        // Drain overflow queue (rare path, under mutex)
        {
            std::lock_guard<std::mutex> lock(_overflow_mtx);
            for (auto& oev : _overflow_queue) {
                if (events.size() >= 1024) break;
                events.push_back(std::move(oev));
            }
            _overflow_queue.clear();
        }

        if (events.empty()) continue;

        // E1: try/catch prevents worker crash from BlackCalc exceptions
        try {
            // P3: O(N) bucket sort replaces O(N log N) stable_sort
            std::vector<AsyncEvent*> bk_sess, bk_chan, bk_pos, bk_trade, bk_order, bk_timer;
            bool has_tick = false;
            for (auto& ev : events) {
                switch (ev.type) {
                    case AsyncEvent::Session:   bk_sess.push_back(&ev); break;
                    case AsyncEvent::Channel:   bk_chan.push_back(&ev); break;
                    case AsyncEvent::Position:  bk_pos.push_back(&ev); break;
                    case AsyncEvent::Trade:     bk_trade.push_back(&ev); break;
                    case AsyncEvent::Order:     bk_order.push_back(&ev); break;
                    case AsyncEvent::Timer:     bk_timer.push_back(&ev); break;
                    case AsyncEvent::Tick:      has_tick = true; break;
                    default: break;
                }
            }
            for (auto* ev : bk_sess)  if (_cbs.on_session)  _cbs.on_session(ev->session.tdate, ev->session.isBegin);
            for (auto* ev : bk_chan)  if (_cbs.on_channel) _cbs.on_channel(ev->channel.isReady);
            for (auto* ev : bk_pos)   if (_cbs.on_position) _cbs.on_position(ev->getCode(), ev->position.isLong, ev->position.newvol);
            for (auto* ev : bk_trade) if (_cbs.on_trade)   _cbs.on_trade(ev->getCode(), ev->trade.localid, ev->trade.isBuy, ev->trade.vol, ev->trade.price);
            for (auto* ev : bk_order) if (_cbs.on_order)   _cbs.on_order(ev->getCode(), ev->order.localid, ev->order.isBuy, ev->order.totalQty, ev->order.leftQty, ev->order.price, ev->order.isCanceled);
            for (auto* ev : bk_timer) if (_cbs.on_timer)   _cbs.on_timer(ev->timer.date, ev->timer.time);

            if (has_tick) {
                // P0-4 fix: string_view key dedup by content (not pointer address)
                std::unordered_map<std::string_view, const TickData*> active_ticks;
                active_ticks.reserve(64);
                for (auto& ev : events) {
                    if (ev.type == AsyncEvent::Tick)
                        active_ticks[std::string_view(ev.code)] = &ev.tick;
                }
                if (_cbs.on_tick_batch) _cbs.on_tick_batch();

                // Process underlying ticks first, then option ticks.
                // This ensures computeValues (triggered by underlying tick)
                // uses the latest option market snapshots.
                // We can't distinguish underlying from option here, so we
                // rely on the consumer (HftOptionStrategy::on_tick) to handle
                // ordering internally. The unordered_map iteration order is
                // non-deterministic, but on_tick for the underlying sets
                // _underlyingChanged=true, and computeValues is deferred to
                // on_batch_complete (debounced), so all ticks are applied
                // before compute runs. This is safe.
                for (auto& [code, tick] : active_ticks) {
                    if (_cbs.on_tick) _cbs.on_tick(std::string(code), *tick);
                }
            }

            // on_batch_complete always runs (even for tickless batches) so that
            // position/trade/order/session/channel events are properly reflected
            // via computeValues + refresh + drainPendingQuotes.
            if (_cbs.on_batch_complete) _cbs.on_batch_complete();
        } catch (const std::exception& e) {
            WTSLogger::log_by_cat("strategy", LL_ERROR,
                "Async worker batch exception: {} (batch skipped)", e.what());
        } catch (...) {
            WTSLogger::log_by_cat("strategy", LL_ERROR,
                "Async worker unknown exception (batch skipped)");
        }

        // Queue saturation monitor (lock-free read, no mutex needed)
        size_t queue_size = _lf_queue.read_available();
        {
            std::lock_guard<std::mutex> lock(_overflow_mtx);
            queue_size += _overflow_queue.size();
        }
        if (queue_size > 3200) {
            uint64_t now = TimeUtils::getLocalTimeNow();
            if (now - _last_warning_time > 5000000) {
                WTSLogger::log_by_cat("strategy", LL_WARN,
                    "OPTION ASYNC QUEUE BOTTLENECK! size={}, drops={}",
                    queue_size, _queue_drops.load(std::memory_order_relaxed));
                _last_warning_time = now;
            }
        }
    }
}

} // namespace wt_option
