/*!
 * \file WtOptContext.h
 * \project	WonderTrader
 *
 * \brief Option Context Implementation
 * 
 * Enhanced to implement full IOptStraCtx interface with data access
 * and trading methods via WtOptEngine.
 */
#pragma once

#include "../Includes/IOptStraCtx.h"
#include "WtOptEngine.h"

#include "OptionGrid.h"
#include "OptionRisk.h"
#include "OrderManager.h"
#include "CurveFitter.h"
#include "IOptionPricer.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <atomic>
#include <boost/lockfree/spsc_queue.hpp>
#include "../Includes/WTSStruct.h"

NS_WTP_BEGIN

class OptStrategy;
class TraderAdapter;

/**
 * @brief Option Strategy Context
 * 
 * Provides the runtime context for option strategies, implementing
 * the IOptStraCtx interface. Delegates data access and trading to
 * the WtOptEngine infrastructure.
 */
class WtOptContext : public IOptStraCtx
{
public:
    WtOptContext(WtOptEngine* engine, const char* name, OptStrategy* stra);
    virtual ~WtOptContext();

public:
    //==========================================================================
    // IOptStraCtx - Identity
    //==========================================================================
    
    virtual uint32_t id() override;
    
    //==========================================================================
    // IOptStraCtx - Callbacks
    //==========================================================================
    
    virtual void on_init() override;
    virtual void on_session_begin(uint32_t uTDate) override;
    virtual void on_session_end(uint32_t uTDate) override;
    virtual void on_tick(const char* stdCode, WTSTickData* newTick) override;
    virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;
    virtual void on_calculate(uint32_t curDate, uint32_t curTime) override;

    //==========================================================================
    // IOptStraCtx - Option Component Access
    //==========================================================================
    
    virtual wt_option::OptionGrid* stra_get_grid() override;
    virtual wt_option::OptionRisk* stra_get_risk() override;
    virtual wt_option::GridOrderManager* stra_get_order_manager() override;

    //==========================================================================
    // IOptStraCtx - Logging
    //==========================================================================
    
    virtual void stra_log_info(const char* message) override;
    virtual void stra_log_error(const char* message) override;
    virtual void stra_log_warn(const char* message) override;
    virtual void stra_log_debug(const char* message) override;

    //==========================================================================
    // IOptStraCtx - Time Access
    //==========================================================================
    
    virtual uint32_t stra_get_date() override;
    virtual uint32_t stra_get_time() override;
    virtual uint32_t stra_get_secs() override;
    virtual uint32_t stra_get_tdate() override;

    //==========================================================================
    // IOptStraCtx - Data Access
    //==========================================================================
    
    virtual WTSTickSlice* stra_get_ticks(const char* stdCode, uint32_t count) override;
    virtual WTSTickData* stra_get_last_tick(const char* stdCode) override;
    virtual WTSKlineSlice* stra_get_bars(const char* stdCode, const char* period, uint32_t count) override;
    virtual double stra_get_price(const char* stdCode) override;

    //==========================================================================
    // IOptStraCtx - Trading Actions
    //==========================================================================
    
    virtual uint32_t stra_buy(const char* stdCode, double price, double qty, const char* userTag) override;
    virtual uint32_t stra_sell(const char* stdCode, double price, double qty, const char* userTag) override;
    
    /**
     * @brief Submit dual-sided quote (Market Making)
     * @param stdCode Standard code
     * @param bidPrice Bid price
     * @param bidQty Bid quantity
     * @param askPrice Ask price
     * @param askQty Ask quantity
     * @param userTag User tag for tracking
     * @return Local quote ID (or bulk id structure depending on underlying)
     */
    virtual uint32_t stra_quote(const char* stdCode, double bidPrice, uint32_t bidQty, double askPrice, uint32_t askQty, const char* userTag) override;
    virtual bool stra_cancel(uint32_t localid) override;
    virtual bool stra_cancel_quote(uint32_t localid) override;
    virtual uint32_t stra_cancel_all(const char* stdCode) override;
    virtual double stra_get_position(const char* stdCode) override;

    //==========================================================================
    // IOptStraCtx - Subscription
    //==========================================================================
    
    virtual void stra_sub_ticks(const char* stdCode) override;

    //==========================================================================
    // Internal Lifecycle
    //==========================================================================
    
    /**
     * @brief Initialize context components from config
     * @param cfg Configuration variant
     * @return true if successful
     */
    bool init(WTSVariant* cfg);
    
    //==========================================================================
    // Internal Thread Callbacks (called by Engine threads)
    //==========================================================================
    
    void update_risk();
    void update_pnl();
    
    // Checkpoint/Recovery mechanism
    void save_data();
    void load_data();

public:
    //==========================================================================
    // Context-specific methods
    //==========================================================================
    
    /**
     * @brief Set trader adapter for order execution
     * @param trader Trader adapter pointer
     */
    void setTrader(TraderAdapter* trader) { _trader = trader; }
    
    /**
     * @brief Get trader adapter
     * @return Trader adapter pointer
     */
    TraderAdapter* getTrader() const { return _trader; }

    //==========================================================================
    // Async Worker Infrastructure
    //==========================================================================
    
    void start();
    void stop();
    
    // Async dispatch
    void enqueue_tick(const char* stdCode, WTSTickData* newTick);
    void enqueue_timer(uint32_t curDate, uint32_t curTime);
    void enqueue_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price);
    void enqueue_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled);
    
    // Internal 
    void update_orders();
    
private:
    void worker_loop();

private:
    std::thread             _worker;
    std::atomic<bool>       _worker_running;
    std::mutex              _worker_mtx;
    std::condition_variable _worker_cv;
    
    std::atomic<uint64_t>   _queue_drops;
    std::atomic<uint64_t>   _total_events;
    uint64_t                _last_warning_time;
    

    struct AsyncEvent {
        enum Type {
            Tick,
            Timer,
            Trade,
            Order,
            Custom
        } type;
        
        uint32_t optId; // Replaces char code[32]. UINT32_MAX means underlying or non-option.
        
        union {
            WTSTickStruct tick;
            struct {
                uint32_t date;
                uint32_t time;
            } timer;
            struct {
                uint32_t localid;
                bool isBuy;
                double vol;
                double price;
            } trade;
            struct {
                uint32_t localid;
                bool isBuy;
                double totalQty;
                double leftQty;
                double price;
                bool isCanceled;
            } order;
        };
        
        AsyncEvent() : type(Custom), optId(UINT32_MAX) {}
        
        // Constructor for Tick
        static AsyncEvent make_tick(uint32_t id, WTSTickData* t) {
            AsyncEvent ev;
            ev.type = Tick;
            ev.optId = id;
            if (t) ev.tick = t->getTickStruct();
            return ev;
        }

        // Constructor for Timer
        static AsyncEvent make_timer(uint32_t d, uint32_t t) {
            AsyncEvent ev;
            ev.type = Timer;
            ev.timer.date = d;
            ev.timer.time = t;
            return ev;
        }

        // Constructor for Trade
        static AsyncEvent make_trade(uint32_t id, uint32_t trId, bool buy, double v, double p) {
            AsyncEvent ev;
            ev.type = Trade;
            ev.optId = id;
            ev.trade.localid = trId;
            ev.trade.isBuy = buy;
            ev.trade.vol = v;
            ev.trade.price = p;
            return ev;
        }

        // Constructor for Order
        static AsyncEvent make_order(uint32_t id, uint32_t orId, bool buy, double tq, double lq, double p, bool canc) {
            AsyncEvent ev;
            ev.type = Order;
            ev.optId = id;
            ev.order.localid = orId;
            ev.order.isBuy = buy;
            ev.order.totalQty = tq;
            ev.order.leftQty = lq;
            ev.order.price = p;
            ev.order.isCanceled = canc;
            return ev;
        }
    };
    
    // Lock-free Single-Producer-Single-Consumer queue
    // Capacity 4096 to handle bursts. Since AsyncEvent is ~300 bytes, this is ~1.2MB.
    boost::lockfree::spsc_queue<AsyncEvent, boost::lockfree::capacity<4096>> _task_queue;

public:
    // Internal accessors for Engine
    wt_option::OptionGridPtr       get_grid() { return _grid; }
    wt_option::OptionRiskPtr       get_risk() { return _risk; }
    wt_option::GridOrderManagerPtr get_order_mgr() { return _order_mgr; }

private:
    WtOptEngine*    _engine;
    OptStrategy*    _stra;
    TraderAdapter*  _trader;
    uint32_t        _id;
    
    std::string     _name_with_prefix;  ///< Name with stra prefix for logging

    wt_option::OptionGridPtr       _grid;
    wt_option::OptionRiskPtr       _risk;
    wt_option::GridOrderManagerPtr _order_mgr;
};

NS_WTP_END
