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
#include "../Includes/WTSContractInfo.hpp"
#include <cstring>
#include <chrono>

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
    // Epoch milliseconds are used only for market freshness checks; duration
    // logic uses OptionTimeService instead of this wall-clock value.
    ev.tick.updateTime = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    // B28: capture exact expiry date from contract info when available
    auto* ci = t->getContractInfo();
    ev.tick.expireDate = (ci && ci->getExpireDate() > 0) ? ci->getExpireDate() : 0;
    // R1: exchange price limits straight off the feed
    ev.tick.upperLimit = t->upperlimit();
    ev.tick.lowerLimit = t->lowerlimit();
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

OptionAsyncEventProcessor::AsyncEvent OptionAsyncEventProcessor::AsyncEvent::make_position(const char* stdCode, bool isLong,
                                                                                             double prevol, double preavail,
                                                                                             double newvol, double newavail) {
    AsyncEvent ev;
    ev.type = Position;
    ev.setCode(stdCode);
    ev.position.isLong = isLong;
    ev.position.prevol = prevol;
    ev.position.preavail = preavail;
    ev.position.newvol = newvol;
    ev.position.newavail = newavail;
    return ev;
}

OptionAsyncEventProcessor::AsyncEvent OptionAsyncEventProcessor::AsyncEvent::make_channel(bool isReady) {
    AsyncEvent ev;
    ev.type = Channel;
    ev.channel.isReady = isReady;
    return ev;
}

OptionAsyncEventProcessor::AsyncEvent OptionAsyncEventProcessor::AsyncEvent::make_entrust(uint32_t localid, bool success) {
    AsyncEvent ev;
    ev.type = Entrust;
    ev.entrust.localid = localid;
    ev.entrust.success = success;
    return ev;
}

OptionAsyncEventProcessor::AsyncEvent OptionAsyncEventProcessor::AsyncEvent::make_control() {
    AsyncEvent ev;
    ev.type = Custom;
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
    {
        std::lock_guard<std::mutex> lock(_trader_mtx);
        _trader_cv.notify_all();
    }
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
        // Ring full -> bounded overflow.  Ticks are coalesced by code so a
        // slow worker retains the latest market for each contract.
        push_md_overflow(std::move(ev));
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

void OptionAsyncEventProcessor::enqueue_timer(uint32_t curDate, uint32_t curTime)
{
    if (!_worker_running) return;
    AsyncEvent ev = AsyncEvent::make_timer(curDate, curTime);
    if (!_md_queue.push(ev)) {
        // Timers are periodic and do not carry per-contract state; a bounded
        // drop under saturation is acceptable and observable via drops.
        std::lock_guard<std::mutex> lock(_md_overflow_mtx);
        if (_md_overflow.size() < kMaxOverflow) {
            _md_overflow.push_back(std::move(ev));
        } else {
            _queue_drops.fetch_add(1, std::memory_order_relaxed);
        }
    }
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
}

//=============================================================================
// Trader/main thread producers (mutex-protected, multi-producer safe)
//=============================================================================
void OptionAsyncEventProcessor::push_md_overflow(AsyncEvent&& ev)
{
    std::lock_guard<std::mutex> lock(_md_overflow_mtx);

    if (ev.type == AsyncEvent::Tick) {
        // Coalesce by contract so the overflow retains the newest market even
        // when the worker falls behind.
        for (auto& old : _md_overflow) {
            if (old.type == AsyncEvent::Tick && old.getCode() == ev.getCode()) {
                old.tick = ev.tick;
                return;
            }
        }

        if (_md_overflow.size() < kMaxOverflow) {
            _md_overflow.push_back(std::move(ev));
            return;
        }

        // Overflow full: prefer evicting the oldest tick; if none is present,
        // evict the oldest event so the newest market is still represented.
        auto it = _md_overflow.begin();
        for (; it != _md_overflow.end(); ++it) {
            if (it->type == AsyncEvent::Tick) break;
        }
        if (it == _md_overflow.end()) it = _md_overflow.begin();
        _md_overflow.erase(it);
        _md_overflow.push_back(std::move(ev));
        _queue_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (_md_overflow.size() < kMaxOverflow) {
        _md_overflow.push_back(std::move(ev));
    } else {
        _queue_drops.fetch_add(1, std::memory_order_relaxed);
    }
}

bool OptionAsyncEventProcessor::push_trader(AsyncEvent&& ev)
{
    // D: control facts must survive a full queue — dropping a channel_lost
    // keeps quoting on a dead channel; dropping an entrust-reject leaves a
    // phantom order that blocks its price level forever.  They bypass the
    // capacity gate entirely (they are rare and tiny, so the queue only
    // exceeds the cap by their count).
    const bool isControl = ev.type == AsyncEvent::Session || ev.type == AsyncEvent::Channel
                           || ev.type == AsyncEvent::Entrust  || ev.type == AsyncEvent::Custom;

    std::unique_lock<std::mutex> lock(_trader_mtx);

    if (!isControl) {
        // Trade/order/position events are high-volume.  Block briefly instead
        // of silently dropping them; the queue is normally tiny and the
        // worker drains it every batch.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
        while (_trader_queue.size() >= kMaxTraderQueue
               && _worker_running.load(std::memory_order_acquire)) {
            if (!_trader_cv.wait_until(lock, deadline, [this]() {
                    return _trader_queue.size() < kMaxTraderQueue
                        || !_worker_running.load(std::memory_order_acquire);
                })) {
                break;
            }
        }

        if (_trader_queue.size() >= kMaxTraderQueue) {
            _queue_drops.fetch_add(1, std::memory_order_relaxed);
            WTSLogger::log_by_cat("strategy", LL_ERROR,
                "AEP trader queue full after backpressure wait; trader event dropped");
            return false;
        }
    }

    _trader_queue.push_back(std::move(ev));
    _trader_count.fetch_add(1, std::memory_order_release);
    return true;
}

bool OptionAsyncEventProcessor::enqueue_trader_event(AsyncEvent&& ev)
{
    if (!_worker_running.load(std::memory_order_acquire)) return false;
    if (!push_trader(std::move(ev))) return false;
    _total_events.fetch_add(1, std::memory_order_relaxed);
    _worker_cv.notify_one();
    return true;
}

void OptionAsyncEventProcessor::enqueue_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
    enqueue_trader_event(AsyncEvent::make_trade(stdCode, localid, isBuy, vol, price));
}

void OptionAsyncEventProcessor::enqueue_order(uint32_t localid, const char* stdCode, bool isBuy,
                                                double totalQty, double leftQty, double price, bool isCanceled)
{
    enqueue_trader_event(AsyncEvent::make_order(stdCode, localid, isBuy, totalQty, leftQty, price, isCanceled));
}

bool OptionAsyncEventProcessor::enqueue_session(uint32_t tdate, bool isBegin)
{
    return enqueue_trader_event(AsyncEvent::make_session(tdate, isBegin));
}

void OptionAsyncEventProcessor::enqueue_position(const char* stdCode, bool isLong,
                                                  double prevol, double preavail,
                                                  double newvol, double newavail)
{
    enqueue_trader_event(AsyncEvent::make_position(stdCode, isLong, prevol, preavail, newvol, newavail));
}

bool OptionAsyncEventProcessor::enqueue_entrust(uint32_t localid, bool success)
{
    return enqueue_trader_event(AsyncEvent::make_entrust(localid, success));
}

bool OptionAsyncEventProcessor::enqueue_channel(bool isReady)
{
    return enqueue_trader_event(AsyncEvent::make_channel(isReady));
}

void OptionAsyncEventProcessor::enqueue_control()
{
    enqueue_trader_event(AsyncEvent::make_control());
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
                return !_worker_running.load(std::memory_order_acquire)
                    || !_md_queue.empty()
                    || _trader_count.load(std::memory_order_acquire) > 0;
            });
        }
        if (!_worker_running) break;

        // 1. Drain MD overflow first (rare, under mutex).  Overflow events are
        // older than anything currently entering the SPSC ring, so preserving
        // this order prevents a stale overflow tick from overwriting a newer
        // ring tick during dedup.
        {
            std::lock_guard<std::mutex> lock(_md_overflow_mtx);
            size_t consumed = 0;
            for (; consumed < _md_overflow.size(); ++consumed) {
                if (events.size() >= 1024) break;
                events.push_back(std::move(_md_overflow[consumed]));
            }
            if (consumed == _md_overflow.size()) {
                _md_overflow.clear();
            } else {
                _md_overflow.erase(_md_overflow.begin(),
                                   _md_overflow.begin() + consumed);
            }
        }

        // 2. Drain MD SPSC queue (wait-free, no mutex)
        while (events.size() < 1024) {
            bool got = _md_queue.consume_one([&events](AsyncEvent& ev) {
                events.push_back(std::move(ev));
            });
            if (!got) break;
        }

        // 3. Drain trader queue (swap under mutex for minimal lock time)
        {
            std::vector<AsyncEvent> trader_batch;
            {
                std::lock_guard<std::mutex> lock(_trader_mtx);
                trader_batch.swap(_trader_queue);
                _trader_count.store(0, std::memory_order_release);
            }
            _trader_cv.notify_all();
            size_t firstUnprocessed = 0;
            for (; firstUnprocessed < trader_batch.size(); ++firstUnprocessed) {
                if (events.size() >= 1024) {
                    // Requeue only the unconsumed suffix.  Reinserting the whole
                    // batch would repeat trade/order/position events that were
                    // already moved into this batch.
                    std::lock_guard<std::mutex> lock(_trader_mtx);
                    _trader_queue.insert(_trader_queue.begin(),
                        std::make_move_iterator(trader_batch.begin() + firstUnprocessed),
                        std::make_move_iterator(trader_batch.end()));
                    _trader_count.fetch_add(trader_batch.size() - firstUnprocessed,
                                            std::memory_order_release);
                    break;
                }
                events.push_back(std::move(trader_batch[firstUnprocessed]));
            }
        }

        if (events.empty()) continue;

        try {
            // Trader/control events preserve enqueue/FIFO order.  Reordering
            // position before trade destroyed the controlled bridge between
            // broker snapshots and fills whenever both arrived in one batch.
            std::vector<AsyncEvent*> bk_trader, bk_timer;
            bool has_tick = false;
            for (auto& ev : events) {
                switch (ev.type) {
                    case AsyncEvent::Session:
                    case AsyncEvent::Channel:
                    case AsyncEvent::Position:
                    case AsyncEvent::Trade:
                    case AsyncEvent::Order:
                    case AsyncEvent::Entrust:
                        bk_trader.push_back(&ev);
                        break;
                    case AsyncEvent::Timer:
                        bk_timer.push_back(&ev);
                        break;
                    case AsyncEvent::Tick:
                        has_tick = true;
                        break;
                    default: break;
                }
            }
            for (auto* ev : bk_trader) {
                switch (ev->type) {
                    case AsyncEvent::Session:
                        if (_cbs.on_session) _cbs.on_session(ev->session.tdate, ev->session.isBegin);
                        break;
                    case AsyncEvent::Channel:
                        if (_cbs.on_channel) _cbs.on_channel(ev->channel.isReady);
                        break;
                    case AsyncEvent::Position:
                        if (_cbs.on_position) _cbs.on_position(ev->getCode(), ev->position.isLong,
                                                                ev->position.prevol, ev->position.preavail,
                                                                ev->position.newvol, ev->position.newavail);
                        break;
                    case AsyncEvent::Trade:
                        if (_cbs.on_trade) _cbs.on_trade(ev->getCode(), ev->trade.localid, ev->trade.isBuy, ev->trade.vol, ev->trade.price);
                        break;
                    case AsyncEvent::Order:
                        if (_cbs.on_order) _cbs.on_order(ev->getCode(), ev->order.localid, ev->order.isBuy,
                                                         ev->order.totalQty, ev->order.leftQty, ev->order.price,
                                                         ev->order.isCanceled);
                        break;
                    case AsyncEvent::Entrust:
                        if (_cbs.on_entrust) _cbs.on_entrust(ev->entrust.localid, ev->entrust.success);
                        break;
                    default: break;
                }
            }
            for (auto* ev : bk_timer) if (_cbs.on_timer) _cbs.on_timer(ev->timer.date, ev->timer.time);

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
            // TimeUtils::getLocalTimeNow() is milliseconds; throttle at 5s.
            // A large negative diff means the local ms-of-day clock wrapped.
            const int64_t elapsed = static_cast<int64_t>(now) -
                                    static_cast<int64_t>(_last_warning_time);
            if (elapsed > 5000 || elapsed < -3600000) {
                WTSLogger::log_by_cat("strategy", LL_WARN,
                    "OPTION ASYNC QUEUE BOTTLENECK! size={}, drops={}",
                    queue_size, _queue_drops.load(std::memory_order_relaxed));
                _last_warning_time = now;
            }
        }
    }
}

} // namespace wt_option
