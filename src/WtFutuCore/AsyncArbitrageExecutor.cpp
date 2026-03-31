/*!
 * \file AsyncArbitrageExecutor.cpp
 * \brief Asynchronous Arbitrage Execution Engine Implementation
 */
#include "AsyncArbitrageExecutor.h"
#include "SpreadArbitrageManager.h"
#include "../WTSTools/WTSLogger.h"

#include <chrono>

namespace futu {

//==============================================================================
// Construction
//==============================================================================

AsyncArbitrageExecutor::AsyncArbitrageExecutor()
    : _arb_manager(nullptr)
    , _stp(nullptr)
    , _tick_queue(std::make_unique<TickQueue>())
    , _order_queue(std::make_unique<OrderQueue>())
    , _running(false)
    , _stop_requested(false)
    , _tick_available(false)
    , _tick_count(0)
    , _last_signal_time(0)
    , _next_request_id(1)
    , _signals_generated(0)
    , _orders_executed(0)
{
}

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
    _cv.notify_one();
    
    if (_arb_thread.joinable())
    {
        _arb_thread.join();
    }
    
    _running.store(false, std::memory_order_release);
    
    WTSLogger::info("AsyncArbitrageExecutor stopped, signals={}, orders={}",
        _signals_generated.load(), _orders_executed.load());
}

//==============================================================================
// Main Thread Interface
//==============================================================================

bool AsyncArbitrageExecutor::pushTick(const std::string& code, double price, 
                                       double multiplier, uint64_t timestamp)
{
    ArbTickData tick(code, price, multiplier, timestamp);
    bool success = _tick_queue->tryPush(tick);
    
    if (success)
    {
        // 增加tick计数
        _tick_count.fetch_add(1, std::memory_order_relaxed);
        
        // 唤醒套利线程
        _tick_available.store(true, std::memory_order_release);
        _cv.notify_one();
    }
    
    return success;
}

size_t AsyncArbitrageExecutor::processPendingOrders(OrderCallback callback)
{
    size_t count = 0;
    
    ArbOrderRequest order;
    while (_order_queue->tryPop(order))
    {
        callback(order);
        ++count;
        _orders_executed.fetch_add(1, std::memory_order_relaxed);
    }
    
    return count;
}

void AsyncArbitrageExecutor::updateMMOrders(const std::string& code,
                                             const std::vector<ActiveOrder>& buy_orders,
                                             const std::vector<ActiveOrder>& sell_orders)
{
    std::lock_guard<std::mutex> lock(_mm_orders_mutex);
    _mm_buy_orders[code] = buy_orders;
    _mm_sell_orders[code] = sell_orders;
}

void AsyncArbitrageExecutor::updateTickSize(const std::string& code, double tickSize)
{
    std::lock_guard<std::mutex> lock(_tick_size_mutex);
    _tick_sizes[code] = tickSize;
}

//==============================================================================
// Arb Thread
//==============================================================================

void AsyncArbitrageExecutor::arbThreadFunc()
{
    WTSLogger::info("AsyncArbitrageExecutor thread started (condition variable mode)");
    
    uint64_t last_process_time = 0;
    uint32_t tick_counter = 0;
    const uint32_t signal_interval_us = _config.signal_interval_us;
    const uint32_t ticks_per_signal = _config.ticks_per_signal;
    const uint32_t max_wait_us = _config.max_wait_us;
    
    while (!_stop_requested.load(std::memory_order_acquire))
    {
        // ================================================================
        // 使用条件变量等待新 tick 数据（超时保护）
        // ================================================================
        {
            std::unique_lock<std::mutex> lock(_cv_mutex);
            _cv.wait_for(lock, std::chrono::microseconds(max_wait_us), 
                [this] { 
                    return _tick_available.load(std::memory_order_acquire) || 
                           _stop_requested.load(std::memory_order_acquire); 
                });
            
            // 重置标志
            _tick_available.store(false, std::memory_order_release);
        }
        
        // ================================================================
        // 处理所有待处理的 tick 数据
        // ================================================================
        size_t ticks_processed = _tick_queue->popAll([this](const ArbTickData& tick) {
            processTick(tick);
        });
        
        tick_counter += static_cast<uint32_t>(ticks_processed);
        
        // ================================================================
        // 获取当前时间
        // ================================================================
        auto now = std::chrono::high_resolution_clock::now();
        uint64_t current_time = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        
        // ================================================================
        // 生成信号条件：
        // 1. 累计 tick 数达到阈值，或
        // 2. 时间间隔达到阈值
        // ================================================================
        bool should_generate = false;
        
        if (tick_counter >= ticks_per_signal)
        {
            should_generate = true;
            tick_counter = 0;
        }
        else if (current_time - last_process_time >= signal_interval_us)
        {
            should_generate = true;
        }
        
        if (should_generate && !_stop_requested.load(std::memory_order_acquire))
        {
            processSignals(current_time);
            last_process_time = current_time;
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
    if (!_arb_manager || !_config.enabled)
        return;
    
    // Generate signals
    auto signals = _arb_manager->generateSignals(current_time);
    
    for (const auto& signal : signals)
    {
        // Only process high-confidence signals
        if (signal.confidence > 0.5)
        {
            executeSignal(signal);
            _signals_generated.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void AsyncArbitrageExecutor::executeSignal(const SpreadSignal& signal)
{
    // Determine buy/sell direction
    bool leg1_is_buy = (signal.type == SpreadSignalType::OPEN_LONG_SPREAD || 
                        signal.type == SpreadSignalType::CLOSE_SHORT_SPREAD);
    bool leg2_is_buy = !leg1_is_buy;
    
    double leg1_price = signal.leg1_price;
    double leg2_price = signal.leg2_price;
    double leg1_qty = signal.leg1_qty > 0 ? signal.leg1_qty : signal.suggested_size;
    double leg2_qty = signal.leg2_qty > 0 ? signal.leg2_qty : signal.suggested_size;
    
    // Get tick sizes for price adjustment
    double leg1_tick_size = 0.2;  // Default
    double leg2_tick_size = 0.2;
    {
        std::lock_guard<std::mutex> lock(_tick_size_mutex);
        auto it1 = _tick_sizes.find(signal.leg1_code);
        if (it1 != _tick_sizes.end()) leg1_tick_size = it1->second;
        auto it2 = _tick_sizes.find(signal.leg2_code);
        if (it2 != _tick_sizes.end()) leg2_tick_size = it2->second;
    }
    
    // Store original prices for profit recalculation
    double orig_leg1_price = leg1_price;
    double orig_leg2_price = leg2_price;
    bool price_adjusted = false;
    
    // Self-trade check (using local copy of MM orders)
    {
        std::lock_guard<std::mutex> lock(_mm_orders_mutex);
        
        // Check leg1
        auto buy_it = _mm_buy_orders.find(signal.leg1_code);
        auto sell_it = _mm_sell_orders.find(signal.leg1_code);
        
        if (leg1_is_buy && sell_it != _mm_sell_orders.end())
        {
            // Buying, check if we have sell orders at or below our buy price
            for (const auto& order : sell_it->second)
            {
                if (order.price <= leg1_price)
                {
                    // Potential self-trade, adjust price by 1 tick
                    leg1_price = order.price - leg1_tick_size;
                    price_adjusted = true;
                    WTSLogger::warn("Self-trade risk on {} BUY, adjusted price from {} to {}",
                        signal.leg1_code, orig_leg1_price, leg1_price);
                    break;
                }
            }
        }
        else if (!leg1_is_buy && buy_it != _mm_buy_orders.end())
        {
            // Selling, check if we have buy orders at or above our sell price
            for (const auto& order : buy_it->second)
            {
                if (order.price >= leg1_price)
                {
                    leg1_price = order.price + leg1_tick_size;
                    price_adjusted = true;
                    WTSLogger::warn("Self-trade risk on {} SELL, adjusted price from {} to {}",
                        signal.leg1_code, orig_leg1_price, leg1_price);
                    break;
                }
            }
        }
        
        // Check leg2 (same logic)
        buy_it = _mm_buy_orders.find(signal.leg2_code);
        sell_it = _mm_sell_orders.find(signal.leg2_code);
        
        if (leg2_is_buy && sell_it != _mm_sell_orders.end())
        {
            for (const auto& order : sell_it->second)
            {
                if (order.price <= leg2_price)
                {
                    leg2_price = order.price - leg2_tick_size;
                    price_adjusted = true;
                    WTSLogger::warn("Self-trade risk on {} BUY, adjusted price from {} to {}",
                        signal.leg2_code, orig_leg2_price, leg2_price);
                    break;
                }
            }
        }
        else if (!leg2_is_buy && buy_it != _mm_buy_orders.end())
        {
            for (const auto& order : buy_it->second)
            {
                if (order.price >= leg2_price)
                {
                    leg2_price = order.price + leg2_tick_size;
                    price_adjusted = true;
                    WTSLogger::warn("Self-trade risk on {} SELL, adjusted price from {} to {}",
                        signal.leg2_code, orig_leg2_price, leg2_price);
                    break;
                }
            }
        }
    }
    
    // If price was adjusted, recalculate profit and check threshold
    if (price_adjusted && _min_profit_threshold > 0)
    {
        // Calculate spread impact from price adjustment
        // For LONG_SPREAD: buy leg1, sell leg2
        // Profit = (leg2_price - orig_leg2) - (leg1_price - orig_leg1)
        // For SHORT_SPREAD: sell leg1, buy leg2
        // Profit = (orig_leg1 - leg1_price) - (orig_leg2 - leg2_price)
        
        double price_impact_leg1 = leg1_price - orig_leg1_price;
        double price_impact_leg2 = leg2_price - orig_leg2_price;
        
        // Spread impact (negative means profit reduced)
        double spread_impact = 0;
        if (leg1_is_buy)
        {
            // Long spread: paying more for leg1 (bad), getting less for leg2 (bad)
            spread_impact = -(price_impact_leg1) - (leg2_is_buy ? price_impact_leg2 : -price_impact_leg2);
        }
        else
        {
            // Short spread: getting less for leg1 (bad), paying more for leg2 (bad)
            spread_impact = price_impact_leg1 + (leg2_is_buy ? price_impact_leg2 : -price_impact_leg2);
        }
        
        // Convert to ticks (using average tick size)
        double avg_tick_size = (leg1_tick_size + leg2_tick_size) / 2.0;
        double spread_impact_ticks = spread_impact / avg_tick_size;
        
        // If spread impact exceeds minimum profit threshold, reject signal
        if (-spread_impact_ticks > _min_profit_threshold)
        {
            WTSLogger::warn("Arb signal REJECTED: price adjustment would cost {:.2f} ticks > threshold {:.2f}",
                -spread_impact_ticks, _min_profit_threshold);
            return;
        }
        
        WTSLogger::info("Arb signal adjusted: spread impact = {:.2f} ticks", spread_impact_ticks);
    }
    
    // Create order requests
    uint32_t req_id = _next_request_id.fetch_add(1, std::memory_order_relaxed);
    
    // Leg 1 order
    ArbOrderRequest order1;
    order1.pair_id = signal.pair_id;
    order1.code = signal.leg1_code;
    order1.is_buy = leg1_is_buy;
    order1.price = leg1_price;
    order1.qty = leg1_qty;
    order1.timestamp = signal.timestamp;
    order1.request_id = req_id;
    
    // Leg 2 order
    ArbOrderRequest order2;
    order2.pair_id = signal.pair_id;
    order2.code = signal.leg2_code;
    order2.is_buy = leg2_is_buy;
    order2.price = leg2_price;
    order2.qty = leg2_qty;
    order2.timestamp = signal.timestamp;
    order2.request_id = req_id + 1;
    
    // Push to order queue (non-blocking)
    _order_queue->tryPush(order1);
    _order_queue->tryPush(order2);
    
    WTSLogger::info("AsyncArb signal: pair={}, leg1={}{}@{}, leg2={}{}@{}",
        signal.pair_id,
        leg1_is_buy ? "BUY" : "SELL", signal.leg1_code, leg1_qty, leg1_price,
        leg2_is_buy ? "BUY" : "SELL", signal.leg2_code, leg2_qty, leg2_price);
}

} // namespace futu
