/*!
 * \file WtOptEngine.h
 * \project	WonderTrader
 *
 * \brief Option Trading Engine - Multi-threaded Market-Making Engine
 * 
 * Features:
 * - Inherits from WtEngine for core infrastructure
 * - Thread pool for parallel order management
 * - Async risk calculation with lock-free cache
 * - TraderAdapter integration for order execution
 */
#pragma once

#include "../WtCore/ITrdNotifySink.h"
#include "../WtCore/WtEngine.h"
#include "../WtCore/TraderAdapter.h"
#include "../Includes/IOptStraCtx.h"
#include "../Share/StdUtils.hpp"
#include "../Share/threadpool.hpp"

// ... (includes remain same)

// Option Core Includes
#include "OptionGreeks.h"
#include "OptionGrid.h"
#include "OptionRisk.h"
#include "OrderManager.h"
#include "CurveFitter.h"
#include "IOptionPricer.h"
#include "../Includes/FasterDefs.h"

// Forward Declarations
NS_WTP_BEGIN
class TraderAdapter;
class WtOptRtTicker;
class WTSVariant;
using OptContextPtr = std::shared_ptr<IOptStraCtx>;
using ThreadPoolPtr = std::shared_ptr<boost::threadpool::pool>;


class WtOptEngine : public WtEngine, public ITrdNotifySink
{
public:
    WtOptEngine();
    virtual ~WtOptEngine();

    virtual void on_init() override;
    virtual void on_session_begin() override;
    virtual void on_session_end() override;
    virtual void on_tick(const char* stdCode, WTSTickData* curTick) override;
    virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override;

    // Core Engine overrides
    void init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier);
    void run();
    void handle_push_quote(WTSTickData* newTick);


public:
    //==========================================================================
    // ITrdNotifySink overrides
    //==========================================================================
    
    virtual void on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price) override;
    virtual void on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled = false) override;
    virtual void on_channel_ready() override;
    virtual void on_channel_lost() override;
    virtual void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override;
    virtual void on_account(const char* currency, double prebalance, double balance, double dynbalance, double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw) override;
    
    //==========================================================================
    // Option Engine specific methods
    //==========================================================================
    
    void stop();
    void addContext(OptContextPtr ctx);
    OptContextPtr getContext(uint32_t id);

    //==========================================================================
    // Timer/Schedule events
    //==========================================================================
    
    void on_timer(uint32_t curDate, uint32_t curTime);
    // void on_minute_end(uint32_t curDate, uint32_t curTime);
    
    //==========================================================================
    // TraderAdapter integration
    //==========================================================================
    
    void setTraderAdapter(TraderAdapter* trader);
    TraderAdapter* getTraderAdapter() { return _trader; }

    //==========================================================================
    // Threading Access for Contexts
    //==========================================================================
    ThreadPoolPtr get_order_pool() { return _order_pool; }

private:
    //==========================================================================
    // Ticker
    //==========================================================================
    
    WtOptRtTicker*      _tm_ticker;
    
    //==========================================================================
    // Contexts
    //==========================================================================
    
    typedef wt_hashmap<uint32_t, OptContextPtr> ContextMap;
    ContextMap          _ctx_map;
    
    //==========================================================================
    // Config
    //==========================================================================
    
    WTSVariant*         _opt_cfg;
    bool                _is_initialized;
    
    //==========================================================================
    // Threading Infrastructure
    //==========================================================================
    
    // Thread pool for parallel order management
    ThreadPoolPtr       _order_pool;
    uint32_t            _order_pool_size;
    
    //==========================================================================
    // TraderAdapter
    //==========================================================================
    
    TraderAdapter*      _trader;
    
    //==========================================================================
    // Internal Methods
    //==========================================================================
    
    void init_threading();
    void stop_threads();
    
    // TraderAdapter callbacks
    void init_order_executors();
};

NS_WTP_END
