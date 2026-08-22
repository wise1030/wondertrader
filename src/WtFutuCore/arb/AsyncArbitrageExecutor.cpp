/*!
 * \file AsyncArbitrageExecutor.cpp
 * \brief Asynchronous Arbitrage Execution Engine Implementation
 */
#include "AsyncArbitrageExecutor.h"
#include "SpreadArbitrageManager.h"
#include "../../WTSTools/WTSLogger.h"
#include "SpinLockGuard.h"

#include <chrono>
#include <thread>

namespace {
// V8-R4/A5: replay 时钟优先 (µs), 首帧前兜底墙钟
uint64_t arbNowUs(const std::atomic<uint64_t>& replay_us)
{
    uint64_t r = replay_us.load(std::memory_order_relaxed);
    if (r > 0)
        return r;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::high_resolution_clock::now().time_since_epoch())
                                     .count());
}
} // namespace

namespace futu
{

//==============================================================================
// Construction
//==============================================================================

AsyncArbitrageExecutor::AsyncArbitrageExecutor()
    : _arb_manager(nullptr), _stp(nullptr), _tick_queue(std::make_unique<TickQueue>()),
      _order_queue(std::make_unique<OrderQueue>()), _running(false), _stop_requested(false), _tick_available(false),
      _tick_count(0), _last_signal_time(0), _next_request_id(1), _signals_generated(0), _orders_executed(0)
{}

AsyncArbitrageExecutor::~AsyncArbitrageExecutor()
{
    stop();
}

//==============================================================================
// Control
//==============================================================================

void AsyncArbitrageExecutor::start()
{
    if (_running.load(std::memory_order_acquire))
        return;

    _stop_requested.store(false, std::memory_order_release);
    _arb_thread = std::thread(&AsyncArbitrageExecutor::arbThreadFunc, this);
    _running.store(true, std::memory_order_release);

    WTSLogger::info("AsyncArbitrageExecutor started");
}

void AsyncArbitrageExecutor::stop()
{
    if (!_running.load(std::memory_order_acquire))
        return;

    _stop_requested.store(true, std::memory_order_release);

    // 唤醒线程以便快速退出
    _tick_available.store(true, std::memory_order_release);

    if (_arb_thread.joinable()) {
        _arb_thread.join();
    }

    _running.store(false, std::memory_order_release);

    WTSLogger::info(
        "AsyncArbitrageExecutor stopped, signals={}, orders={}", _signals_generated.load(), _orders_executed.load());
}

//==============================================================================
// Main Thread Interface
//==============================================================================

bool AsyncArbitrageExecutor::pushTick(const std::string& code, double price, double multiplier, uint64_t timestamp)
{
    ArbTickData tick(code, price, multiplier, timestamp);

    // 回测模式: 同步执行 (不开 arb 线程, 避免 data race)
    // 实盘模式: push 到队列由 arb 线程异步处理
    if (!_running.load(std::memory_order_acquire)) {
        // 回测: 直接同步处理 (不开 arb 线程, 避免 data race)
        processTick(tick);

        // V8-R4/A5: 回测同步路径用 replay 时钟 (墙钟下 TIMEOUT_EXIT 不可复现)
        uint64_t current_time = arbNowUs(_replay_now_us);
        processSignals(current_time);
        return true;
    }

    bool success = _tick_queue->tryPush(tick);

    if (success) {
        // 增加tick计数
        _tick_count.fetch_add(1, std::memory_order_relaxed);

        // 唤醒套利线程 (busy polling 将检测到此标志)
        _tick_available.store(true, std::memory_order_release);
    }

    return success;
}

size_t AsyncArbitrageExecutor::processPendingOrders(OrderCallback callback)
{
    size_t count = 0;

    ArbOrderRequest order;
    while (_order_queue->tryPop(order)) {
        // Wrap callback in try-catch to prevent one exception
        // from blocking processing of subsequent orders in the queue
        try {
            callback(order);
            ++count;
        } catch (const std::exception& e) {
            WTSLogger::error("AsyncArb: order callback exception for pair={}, "
                             "code={}, price={}, qty={}: {}",
                             order.pair_id,
                             order.code,
                             order.price,
                             order.qty,
                             e.what());
            // Still count as executed (order was popped from queue)
            ++count;
        } catch (...) {
            WTSLogger::error(
                "AsyncArb: order callback unknown exception for pair={}, code={}", order.pair_id, order.code);
            ++count;
        }
        _orders_executed.fetch_add(1, std::memory_order_relaxed);
    }

    return count;
}

void AsyncArbitrageExecutor::updateMMOrders(const std::string& code,
                                            const std::vector<ActiveOrder>& buy_orders,
                                            const std::vector<ActiveOrder>& sell_orders)
{
    // P7: 预计算全局 最高买价/最低卖价 标量, 不再存储整个 vector.
    // 空 vector → 擦除条目, 避免残留陈旧 min/max.
    double max_buy = 0;
    for (const auto& o : buy_orders)
        if (o.price > max_buy)
            max_buy = o.price;
    double min_sell = std::numeric_limits<double>::max();
    for (const auto& o : sell_orders)
        if (o.price > 0 && o.price < min_sell)
            min_sell = o.price;

    SpinLockGuard lock(_mm_orders_spin);
    if (max_buy > 0)
        _mm_max_buy[code] = max_buy;
    else
        _mm_max_buy.erase(code);
    if (min_sell < std::numeric_limits<double>::max())
        _mm_min_sell[code] = min_sell;
    else
        _mm_min_sell.erase(code);
}

void AsyncArbitrageExecutor::updateTickSize(const std::string& code, double tickSize)
{
    SpinLockGuard lock(_tick_size_spin);
    _tick_sizes[code] = tickSize;
}

//==============================================================================
// Scheme B-3: Order → Pair Tagging
//
// All three methods are MAIN-THREAD only (called from processPendingOrders
// callback and on_trade). No locks needed.
//==============================================================================

void AsyncArbitrageExecutor::tagOrderPair(uint32_t localid, const std::string& pair_id)
{
    if (pair_id.empty())
        return;
    _oid_to_pair[localid] = pair_id;
}

bool AsyncArbitrageExecutor::consumePairTag(uint32_t localid, std::string& out_pair_id) const
{
    auto it = _oid_to_pair.find(localid);
    if (it == _oid_to_pair.end())
        return false;
    out_pair_id = it->second;
    return true;
}

void AsyncArbitrageExecutor::onOrderFinalized(uint32_t localid)
{
    _oid_to_pair.erase(localid);
}

//==============================================================================
// Arb Thread
//==============================================================================

void AsyncArbitrageExecutor::arbThreadFunc()
{
    WTSLogger::info("AsyncArbitrageExecutor thread started (condition variable mode)");

    uint64_t last_process_time = 0;
    uint32_t tick_counter = 0;
    uint32_t consecutive_errors = 0; // C6 fix: 连续异常计数, ≤3 重试, >3 才禁用
    const uint32_t signal_interval_us = _config.signal_interval_us;
    const uint32_t ticks_per_signal = _config.ticks_per_signal;
    const uint32_t max_wait_us = _config.max_wait_us;

    while (!_stop_requested.load(std::memory_order_acquire)) {
        // ================================================================
        // 自适应自旋轮询 (Adaptive Spin-Polling) 替代 condition_variable
        // ================================================================
        uint32_t spin_count = 0;
        while (!_tick_available.load(std::memory_order_acquire) && !_stop_requested.load(std::memory_order_acquire)) {
            if (spin_count < 1000) {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
                _mm_pause();
#endif
                spin_count++;
            } else if (spin_count < 2000) {
                std::this_thread::yield();
                spin_count++;
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                spin_count = 0;
            }
        }

        // 重置标志
        _tick_available.store(false, std::memory_order_release);

        // ================================================================
        // 异常兜底: 任何处理异常都不能让 arb 线程退出 (主线程仍 pushTick,
        // 线程一死队列塞满后无声丢信号). 异常时禁用套利 + 告警, 保线程存活
        // 以便 _stop_requested 干净关闭.
        // ================================================================
        try {
            // ================================================================
            // 处理所有待处理的 tick 数据
            // ================================================================
            size_t ticks_processed = _tick_queue->popAll([this](const ArbTickData& tick) { processTick(tick); });

            tick_counter += static_cast<uint32_t>(ticks_processed);

            // ================================================================
            // 获取当前时间
            // ================================================================
            uint64_t current_time = arbNowUs(_replay_now_us);

            // ================================================================
            // 生成信号条件：
            // 1. 累计 tick 数达到阈值，或
            // 2. 时间间隔达到阈值
            // ================================================================
            bool should_generate = false;

            if (tick_counter >= ticks_per_signal) {
                should_generate = true;
                tick_counter = 0;
            } else if (current_time - last_process_time >= signal_interval_us) {
                should_generate = true;
            }

            if (should_generate && !_stop_requested.load(std::memory_order_acquire)) {
                processSignals(current_time);
                last_process_time = current_time;
            }
            consecutive_errors = 0; // C6 fix: 本轮处理成功, 清零重试计数
        } catch (const std::exception& e) {
            // C6 fix: 瞬态异常(单笔坏 tick 等)不再永久杀死套利 — 连续 >3 次才禁用
            ++consecutive_errors;
            if (consecutive_errors <= 3) {
                WTSLogger::error("AsyncArb thread exception ({}/3): {} — retrying", consecutive_errors, e.what());
            } else {
                WTSLogger::error("AsyncArb thread exception persistent ({} consecutive): {} — disabling arbitrage",
                                 consecutive_errors,
                                 e.what());
                _config.enabled.store(false, std::memory_order_release);
            }
        } catch (...) {
            ++consecutive_errors;
            if (consecutive_errors <= 3) {
                WTSLogger::error("AsyncArb thread unknown exception ({}/3) — retrying", consecutive_errors);
            } else {
                WTSLogger::error("AsyncArb thread unknown exception persistent — disabling arbitrage");
                _config.enabled.store(false, std::memory_order_release);
            }
        }
    }

    WTSLogger::info("AsyncArbitrageExecutor thread exiting");
}

void AsyncArbitrageExecutor::processTick(const ArbTickData& tick)
{
    if (!_arb_manager)
        return;

    // Update arbitrage manager with tick data
    _arb_manager->onTick(tick.code, tick.price, tick.multiplier, tick.timestamp);
}

void AsyncArbitrageExecutor::processSignals(uint64_t current_time)
{
    if (!_arb_manager || !_config.enabled.load(std::memory_order_relaxed))
        return;

    // Generate signals
    auto signals = _arb_manager->generateSignals(current_time);

    for (const auto& signal : signals) {
        // V8-R3: 双层置信度阈值统一 -- 原硬编码 0.5 与 Manager 放行阈
        // min_signal_confidence(0.3) 不一致, [0.3,0.5) 信号每 arb 周期
        // 空转"生成->过闸->丢弃->释放"; 统一读 Manager 同一配置
        double min_conf = _arb_manager ? _arb_manager->getConfig().min_signal_confidence : 0.5;
        // 注意: OPEN 类信号在 B-3 门已置 in_flight; 被 confidence 过滤丢弃时必须释放,
        // 否则 pair 卡死至 60s 超时. 用 >= 保留最弱有效信号.
        if (signal.confidence >= min_conf) {
            executeSignal(signal);
            _signals_generated.fetch_add(1, std::memory_order_relaxed);
        } else {
            // B7 fix: 无论 OPEN 还是 CLOSE, 低 confidence 丢弃时都必须释放 in_flight,
            // 否则 STOP_LOSS/TIMEOUT 等 CLOSE 信号 confidence<0.5 时会卡 5s 超时.
            _arb_manager->onArbSignalDropped(signal.pair_id);
        }
    }
}

void AsyncArbitrageExecutor::executeSignal(const SpreadSignal& signal)
{
    // Determine buy/sell direction
    bool leg1_is_buy;
    if (signal.type == SpreadSignalType::OPEN_LONG_SPREAD || signal.type == SpreadSignalType::CLOSE_SHORT_SPREAD) {
        leg1_is_buy = true;
    } else if (signal.type == SpreadSignalType::OPEN_SHORT_SPREAD ||
               signal.type == SpreadSignalType::CLOSE_LONG_SPREAD) {
        leg1_is_buy = false;
    } else {
        // STOP_LOSS / TIMEOUT_EXIT: 方向由当前价差持仓符号决定 —
        // 旧代码硬编码按"平多价差"(卖leg1买leg2)处理, 对空头价差止损=加仓.
        // 多头价差平仓 = 卖 leg1; 空头价差平仓 = 买 leg1.
        auto st = _arb_manager->getSpreadState(signal.pair_id);
        // A5: 零持仓时无法确定平仓方向, 降级 NONE 防止误开仓 (止损单错向=新开仓)
        if (st.spread_position == 0.0) {
            WTSLogger::warn("Arb[{}]: close/stop signal with zero spread_position, degraded to NONE", signal.pair_id);
            _arb_manager->onArbSignalDropped(signal.pair_id);
            return;
        }
        leg1_is_buy = (st.spread_position < 0);
    }
    bool leg2_is_buy = !leg1_is_buy;

    double leg1_price = signal.leg1_price;
    double leg2_price = signal.leg2_price;
    double leg1_qty = signal.leg1_qty > 0 ? signal.leg1_qty : signal.suggested_size;
    double leg2_qty = signal.leg2_qty > 0 ? signal.leg2_qty : signal.suggested_size;

    // Get tick sizes for price adjustment
    double leg1_tick_size = 0.2; // Default
    double leg2_tick_size = 0.2;
    {
        SpinLockGuard lock(_tick_size_spin);
        auto it1 = _tick_sizes.find(signal.leg1_code);
        if (it1 != _tick_sizes.end())
            leg1_tick_size = it1->second;
        auto it2 = _tick_sizes.find(signal.leg2_code);
        if (it2 != _tick_sizes.end())
            leg2_tick_size = it2->second;
    }

    // Store original prices for profit recalculation
    double orig_leg1_price = leg1_price;
    double orig_leg2_price = leg2_price;
    bool price_adjusted = false;

    // Self-trade check (P7: O(1) 预计算标量 — 全局 min_sell/max_buy 即约束边界,
    // 与原"扫全部订单取最严格价"等价, 消除锁内 O(n) 线性扫描).
    {
        SpinLockGuard lock(_mm_orders_spin);

        // Check leg1
        if (leg1_is_buy) {
            auto it = _mm_min_sell.find(signal.leg1_code);
            if (it != _mm_min_sell.end() && it->second > 0 && it->second <= leg1_price) {
                leg1_price = it->second - leg1_tick_size;
                price_adjusted = true;
                WTSLogger::warn("Self-trade risk on {} BUY, adjusted price from {} to {}",
                                signal.leg1_code,
                                orig_leg1_price,
                                leg1_price);
            }
        } else {
            auto it = _mm_max_buy.find(signal.leg1_code);
            if (it != _mm_max_buy.end() && it->second > 0 && it->second >= leg1_price) {
                leg1_price = it->second + leg1_tick_size;
                price_adjusted = true;
                WTSLogger::warn("Self-trade risk on {} SELL, adjusted price from {} to {}",
                                signal.leg1_code,
                                orig_leg1_price,
                                leg1_price);
            }
        }

        // Check leg2 (same logic)
        if (leg2_is_buy) {
            auto it = _mm_min_sell.find(signal.leg2_code);
            if (it != _mm_min_sell.end() && it->second > 0 && it->second <= leg2_price) {
                leg2_price = it->second - leg2_tick_size;
                price_adjusted = true;
                WTSLogger::warn("Self-trade risk on {} BUY, adjusted price from {} to {}",
                                signal.leg2_code,
                                orig_leg2_price,
                                leg2_price);
            }
        } else {
            auto it = _mm_max_buy.find(signal.leg2_code);
            if (it != _mm_max_buy.end() && it->second > 0 && it->second >= leg2_price) {
                leg2_price = it->second + leg2_tick_size;
                price_adjusted = true;
                WTSLogger::warn("Self-trade risk on {} SELL, adjusted price from {} to {}",
                                signal.leg2_code,
                                orig_leg2_price,
                                leg2_price);
            }
        }
    }

    // If price was adjusted, recalculate profit and check threshold
    if (price_adjusted && _min_profit_threshold.load(std::memory_order_relaxed) > 0) {
        // Calculate spread impact from price adjustment
        // For LONG_SPREAD: buy leg1, sell leg2
        //   Profit change = (leg2_price - orig_leg2) - (leg1_price - orig_leg1)
        // For SHORT_SPREAD: sell leg1, buy leg2
        //   Profit change = (leg1_price - orig_leg1) - (leg2_price - orig_leg2)
        // V8-A1: 原短价差分支第二项为 + 号 (两腿损失互抵成差值 b-a 而非 -(a+b)),
        // 真实损失超阈的止损/平仓单被放行, 净有利单可能被误拒; 注释公式同步修正

        double price_impact_leg1 = leg1_price - orig_leg1_price;
        double price_impact_leg2 = leg2_price - orig_leg2_price;

        // Spread impact (negative means profit reduced)
        double spread_impact = 0;
        if (leg1_is_buy) {
            // Long spread: paying more for leg1 (bad), getting less for leg2 (bad)
            spread_impact = -(price_impact_leg1) - (leg2_is_buy ? price_impact_leg2 : -price_impact_leg2);
        } else {
            // Short spread: getting less for leg1 (bad), paying more for leg2 (bad)
            spread_impact = price_impact_leg1 - (leg2_is_buy ? price_impact_leg2 : -price_impact_leg2);
        }

        // Convert to ticks (using average tick size)
        double avg_tick_size = (leg1_tick_size + leg2_tick_size) / 2.0;
        double spread_impact_ticks = spread_impact / avg_tick_size;

        // If spread impact exceeds minimum profit threshold, reject signal
        if (-spread_impact_ticks > _min_profit_threshold.load(std::memory_order_relaxed)) {
            WTSLogger::warn("Arb signal REJECTED: price adjustment would cost {:.2f} ticks > threshold {:.2f}",
                            -spread_impact_ticks,
                            _min_profit_threshold.load(std::memory_order_relaxed));
            _arb_manager->onArbSignalDropped(signal.pair_id); // 释放 B-3 in_flight
            return;
        }

        WTSLogger::info("Arb signal adjusted: spread impact = {:.2f} ticks", spread_impact_ticks);
    }

    // Create order requests
    uint32_t req_id = _next_request_id.fetch_add(2, std::memory_order_relaxed);

    // C0: 平仓单执行策略 (STOP_LOSS → FAK 对手价; TIMEOUT → GFD mid)
    bool is_close = is_close_signal(signal.type);
    int order_flag = 0;
    if (is_close && _arb_manager) {
        order_flag = _arb_manager->getArbCloseConfig().policy_for(signal.type).order_flag;
    }

    // Leg 1 order
    ArbOrderRequest order1;
    order1.pair_id = signal.pair_id;
    order1.code = signal.leg1_code;
    order1.is_buy = leg1_is_buy;
    order1.price = leg1_price;
    order1.qty = leg1_qty;
    order1.timestamp = signal.timestamp;
    order1.request_id = req_id;
    order1.order_flag = order_flag;
    order1.is_close = is_close;

    // Leg 2 order
    ArbOrderRequest order2;
    order2.pair_id = signal.pair_id;
    order2.code = signal.leg2_code;
    order2.is_buy = leg2_is_buy;
    order2.price = leg2_price;
    order2.qty = leg2_qty;
    order2.timestamp = signal.timestamp;
    order2.request_id = req_id + 1; // 同一对两腿使用连续ID
    order2.order_flag = order_flag;
    order2.is_close = is_close;

    // 原子提交：先检查队列是否有足够空间放两腿
    const size_t needed_slots = 2;
    const size_t available = _order_queue->capacity() - _order_queue->size();

    if (available < needed_slots) {
        WTSLogger::warn("AsyncArb signal DROPPED: order queue full "
                        "(need={}, avail={}), pair={}",
                        needed_slots,
                        available,
                        signal.pair_id);
        _arb_manager->onArbSignalDropped(signal.pair_id); // 释放 B-3 in_flight
        return;                                           // 两腿都不提交，避免单腿风险
    }

    // 先push第一腿
    bool push1 = _order_queue->tryPush(order1);
    if (!push1) {
        WTSLogger::error("AsyncArb: leg1 push failed after pre-check! pair={}", signal.pair_id);
        _arb_manager->onArbSignalDropped(signal.pair_id); // 释放 B-3 in_flight
        return;                                           // 第一腿失败，不提交第二腿
    }

    // 再push第二腿
    bool push2 = _order_queue->tryPush(order2);
    if (!push2) {
        // 极端情况：预检通过但第二腿push失败
        // 第一腿已在队列中无法撤回，记录严重告警
        WTSLogger::error("AsyncArb CRITICAL: leg2 push failed after leg1 "
                         "succeeded! pair={}, leg1_req_id={}",
                         signal.pair_id,
                         req_id);

        // 记录单腿敞口，供processPendingOrders检测并自动对冲
        // Arb线程push到_from_arb队列(SPSC安全)，主线程pop
        // 传入delta_ratio=0(arb线程无法获取portfolio delta，主线程回调时补充)
        if (!_orphan_legs_from_arb.tryPush({signal.pair_id,
                                            req_id,
                                            signal.leg1_code,
                                            signal.leg2_code,
                                            leg1_is_buy,
                                            leg1_qty,
                                            leg1_price,
                                            arbNowUs(_replay_now_us),
                                            0.0})) {
            WTSLogger::error("AsyncArb CRITICAL: orphan queue FULL (64), "
                             "leg1 exposed! pair={}, leg1={}",
                             signal.pair_id,
                             signal.leg1_code);
            _arb_manager->onArbSignalDropped(signal.pair_id);
        } else {
            WTSLogger::warn("AsyncArb: orphan leg recorded for auto-hedge, "
                            "pair={}, leg1={}",
                            signal.pair_id,
                            signal.leg1_code);
        }
    }

    WTSLogger::debug("AsyncArb signal: pair={}, leg1={} {}@{} ({}), leg2={} {}@{} ({})",
                    signal.pair_id,
                    leg1_is_buy ? "BUY" : "SELL",
                    signal.leg1_code,
                    leg1_qty,
                    leg1_price,
                    leg2_is_buy ? "BUY" : "SELL",
                    signal.leg2_code,
                    leg2_qty,
                    leg2_price);
}

//==============================================================================
// Orphan Leg Auto-Hedge
//==============================================================================

size_t AsyncArbitrageExecutor::processOrphanLegs(OrphanHedgeCallback callback,
                                                 uint64_t timeout_ms,
                                                 uint64_t force_ms,
                                                 double current_delta_ratio)
{
    size_t processed = 0;
    uint64_t now = arbNowUs(_replay_now_us); // V8-R4/A5: replay µs

    // Dual-queue SPSC-safe design:
    // 1. Pop all new orphan legs from arb thread (SPSC queue)
    // 2. Merge with existing deferred legs
    // 3. Process all: hedge if timeout, defer if not
    // This avoids the old bug where main thread tryPush back into SPSC queue
    // violated single-consumer invariant.

    OrphanLeg orphan;
    while (_orphan_legs_from_arb.tryPop(orphan)) {
        // arb线程无法获取portfolio delta，主线程传入current_delta_ratio
        if (orphan.delta_ratio == 0.0 && current_delta_ratio > 0.0) {
            orphan.delta_ratio = current_delta_ratio;
        }
        _orphan_legs_deferred.push_back(std::move(orphan));
    }

    // Process all deferred legs
    std::vector<OrphanLeg> still_deferred;
    for (auto& leg : _orphan_legs_deferred) {
        int64_t age_ms = (now > leg.timestamp) ? static_cast<int64_t>((now - leg.timestamp) / 1000) : 0;

        // 根据delta_ratio动态调整超时
        // delta_ratio越高(接近或超过max_delta)，超时越短，对冲越积极
        // delta_ratio=0: 无delta限制，使用原始超时
        // delta_ratio>=1.0: 已超delta限制，立即强制对冲
        uint64_t effective_timeout = timeout_ms;
        uint64_t effective_force = force_ms;
        if (leg.delta_ratio > 0) {
            if (leg.delta_ratio >= 1.0) {
                // 已超delta限制: 立即强制对冲
                effective_timeout = 0;
                effective_force = 0;
            } else {
                // delta_ratio在(0,1): 线性缩短超时
                // ratio=0.5时timeout减半, ratio=0.8时timeout只剩20%
                double shrink = 1.0 - leg.delta_ratio;
                effective_timeout = static_cast<uint64_t>(timeout_ms * shrink);
                effective_force = static_cast<uint64_t>(force_ms * shrink);
                // 最低保证1秒grace period
                if (effective_timeout < 1000)
                    effective_timeout = 1000;
                if (effective_force < 2000)
                    effective_force = 2000;
            }
        }

        if (age_ms >= static_cast<int64_t>(effective_force)) {
            // Level 3: 超过force_ms → 强制市价对冲（最紧急）
            bool hedge_is_buy = !leg.leg1_is_buy;
            double hedge_price = leg.leg1_price; // fallback, callback会覆盖

            WTSLogger::error("OrphanLeg URGENT: pair={}, leg1={}{}@{}, "
                             "age={}ms > force={}ms (delta_ratio={:.2f}) → force market hedge on {}",
                             leg.pair_id,
                             leg.leg1_is_buy ? "BUY" : "SELL",
                             leg.leg1_code,
                             leg.leg1_qty,
                             leg.leg1_price,
                             age_ms,
                             effective_force,
                             leg.delta_ratio,
                             leg.leg2_code);

            callback(leg.leg2_code, hedge_is_buy, hedge_price, leg.leg1_qty, /*urgent=*/true);
            ++processed;
        } else if (age_ms >= static_cast<int64_t>(effective_timeout)) {
            // Level 2: 超过timeout_ms → 对手价对冲（积极减仓）
            bool hedge_is_buy = !leg.leg1_is_buy;
            double hedge_price = leg.leg1_price; // fallback, callback会覆盖

            WTSLogger::warn("OrphanLeg HEDGE: pair={}, leg1={}{}@{}, "
                            "age={}ms > timeout={}ms (delta_ratio={:.2f}) → aggressive hedge on {}",
                            leg.pair_id,
                            leg.leg1_is_buy ? "BUY" : "SELL",
                            leg.leg1_code,
                            leg.leg1_qty,
                            leg.leg1_price,
                            age_ms,
                            effective_timeout,
                            leg.delta_ratio,
                            leg.leg2_code);

            callback(leg.leg2_code, hedge_is_buy, hedge_price, leg.leg1_qty, /*urgent=*/false);
            ++processed;
        } else {
            // Level 1: 未超时 → 保留等待（给leg2重试机会）
            WTSLogger::debug("OrphanLeg DEFERRED: pair={}, leg1={}{}@{}, "
                             "age={}ms < timeout_ms={}ms, waiting",
                             leg.pair_id,
                             leg.leg1_is_buy ? "BUY" : "SELL",
                             leg.leg1_code,
                             leg.leg1_qty,
                             leg.leg1_price,
                             age_ms,
                             timeout_ms);
            still_deferred.push_back(std::move(leg));
        }
    }

    // Swap: keep only still-deferred legs for next call
    _orphan_legs_deferred = std::move(still_deferred);

    return processed;
}

} // namespace futu
