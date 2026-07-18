/*!
 * \file OptionAsyncEventProcessor.cpp
 * \brief Async Event Processor implementation (double-queue architecture)
 *
 * P0-A fix: MD events go to SPSC _md_queue (single producer),
 * trader events go to mutex-protected _trader_queue (multi-producer safe).
 */
#include "OptionAsyncEventProcessor.h"
#include "../Share/TimeUtils.hpp"
#include "../WTSTools/WTSLogger.h"
#include "../Includes/WTSDataDef.hpp"
#include <cstring>

namespace wt_option {

//=============================================================================
// AsyncEvent factory methods
//=============================================================================
OptionAsyncEventProcessor::AsyncEvent OptionAsyncEventProcessor::AsyncEvent::make_tick(const char* stdCode, WTSTickData* t)
{
    AsyncEvent ev;
    ev.type = Tick;
    ev.setCode(stdCode);
    ev.tick.price = t->price();
    ev.tick.bid = t->bidprice(0);
    ev.tick.ask = t->askprice(0);
    ev.tick.bidQty = t->bidqty(0);
    ev.tick.askQty = t->askqty(0);
    ev.tick.preClose = t->preclose();
    ev.tick.tradeVolume = t->volume();
    ev.tick.actionTime = t->actiontime();
    ev.tick.updateTime = TimeUtils::getLocalTimeNow();
    return ev;
}

OptionAsyncEventProcessor::AsyncEvent OptionAsyncEventProcessor::AsyncEvent::make_timer(uint32_t d, uint32_t t) {
    AsyncEvent ev;
    ev.type = Timer;
    ev.timer.date = d;
    ev.timer.time = t;
    return ev;
}

OptionAsyncEventProcessor::AsyncEvent OptionAsyncEventProcessor::AsyncEvent::make_trade(const char* stdCode, uint32_t id, bool buy, double v, double p) {
    AsyncEvent ev;
    ev.type = Trade;
    ev.setCode(stdCode);
    ev.trade.localid = id;
    ev.trade.isBuy = buy;
    ev.trade.vol = v;
    ev.trade.price = p;
    return ev;
}

OptionAsyncEventProcessor::AsyncEvent OptionAsyncEventProcessor::AsyncEvent::make_order(const char* stdCode, uint32_t id, bool buy,
                                   double tq, double lq, double p, bool canc) {
    AsyncEvent ev;
    ev.type = Order;
    ev.setCode(stdCode);
    ev.order.localid = id;
    ev.order.isBuy = buy;
    ev.order.totalQty = tq;
    ev.order.leftQty = lq;
    ev.order.price = p;
    ev.order.isCanceled = canc;
    return ev;
}

OptionAsyncEventProcessor::AsyncEvent OptionAsyncEventProcessor::AsyncEvent::make_session(uint32_t tdate, bool isBegin) {
    AsyncEvent ev;
    ev.type = Session;
    ev.session.tdate = tdate;
    ev.session.isBegin = isBegin;
    return ev;
}

OptionAsyncEventProcessor::AsyncEvent OptionAsyncEventProcessor::AsyncEvent::make_position(const char* stdCode, bool isLong, double newvol) {
    AsyncEvent ev;
    ev.type = Position;
    ev.setCode(stdCode);
    ev.position.isLong = isLong;
    ev.position.newvol = newvol;
    return ev;
}

OptionAsyncEventProcessor::AsyncEvent OptionAsyncEventProcessor::AsyncEvent::make_channel(bool isReady) {
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
    _trader_queue.reserve(256);
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
    _worker_running = true;
    _worker = std::thread(&OptionAsyncEventProcessor::worker_loop, this);
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "OptionAsyncEventProcessor started (double-queue: md_spsc + trader_mutex)");
}

void OptionAsyncEventProcessor::stop()
{
    _worker_running = false;
    _worker_cv.notify_all();
    if (_worker.joinable()) _worker.join();
    WTSLogger::log_by_cat("strategy", LL_INFO, "OptionAsyncEventProcessor stopped");
}

//=============================================================================
// MD thread producers (SPSC, single producer = MD callback thread)
//=============================================================================
void OptionAsyncEventProcessor::enqueue_tick(const char* stdCode, WTSTickData* newTick)
{
    if (!_worker_running) return;
    AsyncEvent ev = AsyncEvent::make_tick(stdCode, newTick);
    if (!_md_queue.push(ev)) {
        // Ring full -> overflow
        {
            std::lock_guard<std::mutex> lock(_md_overflow_mtx);
            _md_overflow.push_back(std::move(ev));
        }
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_timer(uint32_t curDate, uint32_t curTime)
{
    if (!_worker_running) return;
    AsyncEvent ev = AsyncEvent::make_timer(curDate, curTime);
    if (!_md_queue.push(ev)) {
        std::lock_guard<std::mutex> lock(_md_overflow_mtx);
        _md_overflow.push_back(std::move(ev));
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

//=============================================================================
// Trader/main thread producers (mutex-protected, multi-producer safe)
//=============================================================================
void OptionAsyncEventProcessor::enqueue_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
    if (!_worker_running) return;
    {
        std::lock_guard<std::mutex> lock(_trader_mtx);
        _trader_queue.push_back(AsyncEvent::make_trade(stdCode, localid, isBuy, vol, price));
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_order(uint32_t localid, const char* stdCode, bool isBuy,
                                                double totalQty, double leftQty, double price, bool isCanceled)
{
    if (!_worker_running) return;
    {
        std::lock_guard<std::mutex> lock(_trader_mtx);
        _trader_queue.push_back(AsyncEvent::make_order(stdCode, localid, isBuy, totalQty, leftQty, price, isCanceled));
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_session(uint32_t tdate, bool isBegin)
{
    if (!_worker_running) return;
    {
        std::lock_guard<std::mutex> lock(_trader_mtx);
        _trader_queue.push_back(AsyncEvent::make_session(tdate, isBegin));
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_position(const char* stdCode, bool isLong, double newvol)
{
    if (!_worker_running) return;
    {
        std::lock_guard<std::mutex> lock(_trader_mtx);
        _trader_queue.push_back(AsyncEvent::make_position(stdCode, isLong, newvol));
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_channel(bool isReady)
{
    if (!_worker_running) return;
    {
        std::lock_guard<std::mutex> lock(_trader_mtx);
        _trader_queue.push_back(AsyncEvent::make_channel(isReady));
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

//=============================================================================
// Helper: check if any queue has events
//=============================================================================
bool OptionAsyncEventProcessor::has_events() const
{
    // Check MD SPSC queue (lock-free read)
    if (!_md_queue.empty()) return true;
    // Check MD overflow (requires lock, but rare)
    // Check trader queue (requires lock)
    // We use try_lock to avoid blocking in the predicate;
    // if we can't get the lock, assume there might be events.
    return true;  // conservative: always return true to avoid missing events
}

//=============================================================================
// Worker loop — drain both queues into unified batch
//=============================================================================
void OptionAsyncEventProcessor::worker_loop()
{
    std::vector<AsyncEvent> events;
    events.reserve(256);

    while (_worker_running) {
        events.clear();

        // Wait for events (cv with 100us timeout as fallback)
        {
            std::unique_lock<std::mutex> lock(_worker_sleep_mtx);
            _worker_cv.wait_for(lock, std::chrono::microseconds(100), [this]() {
                return !_worker_running || !_md_queue.empty() || !_trader_queue.empty();
            });
        }
        if (!_worker_running) break;

        // 1. Drain MD SPSC queue (wait-free, no mutex)
        while (events.size() < 1024) {
            bool got = _md_queue.consume_one([&events](AsyncEvent& ev) {
                events.push_back(std::move(ev));
            });
            if (!got) break;
        }

        // 2. Drain MD overflow (rare, under mutex)
        {
            std::lock_guard<std::mutex> lock(_md_overflow_mtx);
            for (auto& oev : _md_overflow) {
                if (events.size() >= 1024) break;
                events.push_back(std::move(oev));
            }
            _md_overflow.clear();
        }

        // 3. Drain trader queue (swap under mutex for minimal lock time)
        {
            std::vector<AsyncEvent> trader_batch;
            {
                std::lock_guard<std::mutex> lock(_trader_mtx);
                trader_batch.swap(_trader_queue);
            }
            for (auto& tev : trader_batch) {
                if (events.size() >= 1024) {
                    // Put back events that didn't fit (rare)
                    std::lock_guard<std::mutex> lock(_trader_mtx);
                    _trader_queue.insert(_trader_queue.begin(),
                        std::make_move_iterator(trader_batch.begin()),
                        std::make_move_iterator(trader_batch.end()));
                    break;
                }
                events.push_back(std::move(tev));
            }
        }

        if (events.empty()) continue;

        try {
            // O(N) bucket sort
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
            // Process in priority order: session → channel → position → trade → order → timer
            for (auto* ev : bk_sess)  if (_cbs.on_session)  _cbs.on_session(ev->session.tdate, ev->session.isBegin);
            for (auto* ev : bk_chan)  if (_cbs.on_channel) _cbs.on_channel(ev->channel.isReady);
            for (auto* ev : bk_pos)   if (_cbs.on_position) _cbs.on_position(ev->getCode(), ev->position.isLong, ev->position.newvol);
            for (auto* ev : bk_trade) if (_cbs.on_trade)   _cbs.on_trade(ev->getCode(), ev->trade.localid, ev->trade.isBuy, ev->trade.vol, ev->trade.price);
            for (auto* ev : bk_order) if (_cbs.on_order)   _cbs.on_order(ev->getCode(), ev->order.localid, ev->order.isBuy, ev->order.totalQty, ev->order.leftQty, ev->order.price, ev->order.isCanceled);
            for (auto* ev : bk_timer) if (_cbs.on_timer)   _cbs.on_timer(ev->timer.date, ev->timer.time);

            if (has_tick) {
                // string_view key dedup by content
                std::unordered_map<std::string_view, const TickData*> active_ticks;
                active_ticks.reserve(64);
                for (auto& ev : events) {
                    if (ev.type == AsyncEvent::Tick)
                        active_ticks[std::string_view(ev.code)] = &ev.tick;
                }
                if (_cbs.on_tick_batch) _cbs.on_tick_batch();

                for (auto& [code, tick] : active_ticks) {
                    if (_cbs.on_tick) _cbs.on_tick(std::string(code), *tick);
                }
            }

            if (_cbs.on_batch_complete) _cbs.on_batch_complete();
        } catch (const std::exception& e) {
            WTSLogger::log_by_cat("strategy", LL_ERROR,
                "Async worker batch exception: {} (batch skipped)", e.what());
        } catch (...) {
            WTSLogger::log_by_cat("strategy", LL_ERROR,
                "Async worker unknown exception (batch skipped)");
        }

        // Queue saturation monitor
        size_t queue_size = _md_queue.read_available();
        {
            std::lock_guard<std::mutex> lock(_md_overflow_mtx);
            queue_size += _md_overflow.size();
        }
        {
            std::lock_guard<std::mutex> lock(_trader_mtx);
            queue_size += _trader_queue.size();
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
