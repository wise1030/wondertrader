/*!
 * \file AsyncArbitrageExecutor.h
 * \brief Asynchronous Arbitrage Execution Engine
 * 
 * Runs arbitrage logic in a separate thread to minimize impact on
 * market-making latency. Uses lock-free queues for communication.
 * 
 * Architecture:
 *   Main Thread (Quoting):
 *     1. on_tick() → Push tick data to queue (~50ns)
 *     2. on_tick() → Check order request queue and execute
 *   
 *   Arb Thread:
 *     1. Process tick data, update spread calculators
 *     2. Generate signals, perform self-trade check
 *     3. Push order requests to result queue
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include "LockFreeQueue.hpp"
#include "SpreadArbitrageTypes.h"
#include "SelfTradePrevention.h"
#include "../Includes/FasterDefs.h"

#include <thread>
#include <atomic>
#include <functional>
#include <memory>

NS_WTP_BEGIN
class IUftStraCtx;
NS_WTP_END

namespace futu {

class SpreadArbitrageManager;

//==============================================================================
// Tick Data for Arb Thread
//==============================================================================

struct ArbTickData
{
    std::string code;
    double price;
    double multiplier;
    uint64_t timestamp;
    
    ArbTickData() : price(0), multiplier(1), timestamp(0) {}
    ArbTickData(const std::string& c, double p, double m, uint64_t t)
        : code(c), price(p), multiplier(m), timestamp(t) {}
};

//==============================================================================
// Order Request from Arb Thread
//==============================================================================

struct ArbOrderRequest
{
    std::string pair_id;            ///< Spread pair ID
    std::string code;               ///< Contract code
    bool is_buy;                    ///< Direction
    double price;                   ///< Order price
    double qty;                     ///< Order quantity
    uint64_t timestamp;             ///< Request timestamp
    uint32_t request_id;            ///< Unique request ID
    
    ArbOrderRequest()
        : is_buy(true), price(0), qty(0), timestamp(0), request_id(0) {}
};

//==============================================================================
// Position Sync Data
//==============================================================================

struct ArbPositionSync
{
    std::string pair_id;
    std::string code;
    double position;                ///< Position after trade
    double unrealized_pnl;
    uint64_t timestamp;
};

//==============================================================================
// Async Arbitrage Executor Configuration
//==============================================================================

struct AsyncArbConfig
{
    uint32_t tick_queue_size;       ///< Tick queue size (power of 2)
    uint32_t order_queue_size;      ///< Order queue size (power of 2)
    uint32_t signal_interval_us;    ///< Signal generation interval (microseconds)
    uint32_t max_wait_us;           ///< Max wait time for condition variable (microseconds)
    uint32_t ticks_per_signal;      ///< Generate signal every N ticks
    std::atomic<bool> enabled{true}; ///< FIX: atomic — arb线程读, 主线程写(setConfig)
    
    AsyncArbConfig()
        : tick_queue_size(1024)
        , order_queue_size(256)
        , signal_interval_us(5000)      // 5ms 信号检查间隔
        , max_wait_us(10000)            // 10ms 最大等待
        , ticks_per_signal(5)           // 每5个tick检查一次信号
    {}

    // FIX: copy/move需特殊处理atomic字段
    AsyncArbConfig(const AsyncArbConfig& other)
        : tick_queue_size(other.tick_queue_size)
        , order_queue_size(other.order_queue_size)
        , signal_interval_us(other.signal_interval_us)
        , max_wait_us(other.max_wait_us)
        , ticks_per_signal(other.ticks_per_signal)
        , enabled(other.enabled.load())
    {}

    AsyncArbConfig& operator=(const AsyncArbConfig& other)
    {
        tick_queue_size = other.tick_queue_size;
        order_queue_size = other.order_queue_size;
        signal_interval_us = other.signal_interval_us;
        max_wait_us = other.max_wait_us;
        ticks_per_signal = other.ticks_per_signal;
        enabled.store(other.enabled.load());
        return *this;
    }
};

//==============================================================================
// Async Arbitrage Executor
//==============================================================================

class AsyncArbitrageExecutor
{
public:
    using OrderCallback = std::function<void(const ArbOrderRequest&)>;
    using PositionCallback = std::function<void(const ArbPositionSync&)>;
    
    AsyncArbitrageExecutor();
    ~AsyncArbitrageExecutor();
    
    // Non-copyable
    AsyncArbitrageExecutor(const AsyncArbitrageExecutor&) = delete;
    AsyncArbitrageExecutor& operator=(const AsyncArbitrageExecutor&) = delete;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const AsyncArbConfig& config) { _config = config; }
    const AsyncArbConfig& getConfig() const { return _config; }
    
    void setArbitrageManager(SpreadArbitrageManager* manager) { _arb_manager = manager; }
    void setSelfTradePrevention(SelfTradePrevention* stp) { _stp = stp; }
    
    //==========================================================================
    // Callbacks
    //==========================================================================
    
    void setOrderCallback(OrderCallback callback) { _order_callback = callback; }
    void setPositionCallback(PositionCallback callback) { _position_callback = callback; }
    
    //==========================================================================
    // Control
    //==========================================================================
    
    /// Start the arb thread
    void start();
    
    /// Stop the arb thread
    void stop();
    
    /// Check if running
    bool isRunning() const { return _running.load(std::memory_order_acquire); }
    
    //==========================================================================
    // Main Thread Interface
    //==========================================================================
    
    /// Push tick data (called from main thread, non-blocking)
    bool pushTick(const std::string& code, double price, double multiplier, uint64_t timestamp);
    
    /// Get pending orders (called from main thread)
    /// @param callback Function to call for each order
    /// @return Number of orders processed
    size_t processPendingOrders(OrderCallback callback);
    
    /// Update MM orders for self-trade prevention
    void updateMMOrders(const std::string& code, 
                        const std::vector<ActiveOrder>& buy_orders,
                        const std::vector<ActiveOrder>& sell_orders);
    
    /// Update tick size for a contract (for self-trade price adjustment)
    void updateTickSize(const std::string& code, double tickSize);
    
    /// Set minimum profit threshold for arbitrage (reject if below)
    void setMinProfitThreshold(double threshold) { _min_profit_threshold = threshold; }
    
    //==========================================================================
    // Orphan Leg Auto-Hedge (Main Thread Interface)
    //==========================================================================
    
    /// Callback for orphan leg hedge orders
    /// @param code     Contract code to hedge (leg2_code)
    /// @param is_buy   Hedge direction (opposite of leg1)
    /// @param price    Hedge price (aggressive: counter-price)
    /// @param qty      Hedge quantity (same as leg1_qty)
    /// @param urgent   True if timeout exceeded → force market order
    using OrphanHedgeCallback = std::function<void(const std::string& code,
                                                    bool is_buy,
                                                    double price,
                                                    double qty,
                                                    bool urgent)>;
    
    /// Process orphan legs and generate hedge orders (called from main thread)
    /// @param callback  Called for each orphan leg that needs hedging
    /// @param timeout_ms  Grace period before forcing aggressive hedge (default 5000ms)
    /// @param force_ms   Force market-order deadline (default 30000ms)
    /// @param current_delta_ratio  Current portfolio delta ratio (abs(delta)/max_delta), 0=no limit
    /// @return Number of orphan legs processed
    size_t processOrphanLegs(OrphanHedgeCallback callback,
                              uint64_t timeout_ms = 5000,
                              uint64_t force_ms = 30000,
                              double current_delta_ratio = 0.0);
    
    //==========================================================================
    // Statistics
    //==========================================================================
    
    size_t ticksQueued() const { return _tick_queue->size(); }
    size_t ordersPending() const { return _order_queue->size(); }
    uint64_t signalsGenerated() const { return _signals_generated.load(); }
    uint64_t ordersExecuted() const { return _orders_executed.load(); }
    
private:
    //==========================================================================
    // Arb Thread
    //==========================================================================
    
    void arbThreadFunc();
    void processTick(const ArbTickData& tick);
    void processSignals(uint64_t current_time);
    void executeSignal(const SpreadSignal& signal);
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    AsyncArbConfig _config;
    
    //==========================================================================
    // Components
    //==========================================================================
    
    SpreadArbitrageManager* _arb_manager;
    SelfTradePrevention* _stp;
    
    //==========================================================================
    // Queues
    //==========================================================================
    
    // Tick data queue (main thread → arb thread)
    using TickQueue = LockFreeQueue<ArbTickData, 1024>;
    std::unique_ptr<TickQueue> _tick_queue;
    
    // Order request queue (arb thread → main thread)
    using OrderQueue = LockFreeQueue<ArbOrderRequest, 256>;
    std::unique_ptr<OrderQueue> _order_queue;
    
    //==========================================================================
    // FIX P0-4: Orphan leg tracking for auto-hedge (dual-queue for SPSC safety)
    //==========================================================================
    struct OrphanLeg {
        std::string pair_id;
        uint32_t leg1_req_id;
        std::string leg1_code;
        std::string leg2_code;
        bool leg1_is_buy;
        double leg1_qty;
        double leg1_price;
        std::chrono::steady_clock::time_point timestamp;
        // FIX P2-7: delta_ratio用于动态调整对冲超时
        // = abs(current_delta / max_delta), 0表示无delta限制
        double delta_ratio = 0.0;
    };
    
    // Queue from arb thread → main thread (SPSC: arb pushes, main pops)
    LockFreeQueue<OrphanLeg, 64> _orphan_legs_from_arb;
    
    // Deferred legs (main-thread-only, no contention)
    std::vector<OrphanLeg> _orphan_legs_deferred;
    
    //==========================================================================
    // Thread
    //==========================================================================
    
    std::thread _arb_thread;
    std::atomic<bool> _running;
    std::atomic<bool> _stop_requested;
    
    // Flag for tick notification
    std::atomic<bool> _tick_available;
    
    //==========================================================================
    // State (for arb thread)
    //==========================================================================
    
    // MM order state copy for self-trade check (using atomic_flag as spinlock)
    std::atomic_flag _mm_orders_spin = ATOMIC_FLAG_INIT;
    wtp::wt_hashmap<std::string, std::vector<ActiveOrder>> _mm_buy_orders;
    wtp::wt_hashmap<std::string, std::vector<ActiveOrder>> _mm_sell_orders;
    
    // Tick sizes for price adjustment (using atomic_flag as spinlock)
    std::atomic_flag _tick_size_spin = ATOMIC_FLAG_INIT;
    wtp::wt_hashmap<std::string, double> _tick_sizes;
    double _min_profit_threshold = 0.0;  // Minimum profit threshold in ticks
    
    // Tick counter for signal generation
    std::atomic<uint32_t> _tick_count;
    
    // Last signal time
    uint64_t _last_signal_time;
    std::atomic<uint32_t> _next_request_id;
    
    //==========================================================================
    // Statistics
    //==========================================================================
    
    std::atomic<uint64_t> _signals_generated;
    std::atomic<uint64_t> _orders_executed;
    
    //==========================================================================
    // Callbacks
    //==========================================================================
    
    OrderCallback _order_callback;
    PositionCallback _position_callback;
};

} // namespace futu
