/*!
 * \file WtOptEngine.cpp
 * \project	WonderTrader
 *
 * \brief Option Trading Engine - Multi-threaded Implementation
 */
#define WIN32_LEAN_AND_MEAN

#include "WtOptEngine.h"
#include "WtOptContext.h" 
#include "StandardOptionPricer.h"
#include "CompositeOptionPricer.h"
#include "SignalFactory.h"
#include "OptionGrid.h"
#include "OptionRisk.h"
#include "OrderManager.h"
#include "CurveFitter.h"

#include "../WtCore/WtDtMgr.h"
#include "../WtCore/TraderAdapter.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSDataDef.hpp"

#include "../Share/TimeUtils.hpp"
#include "../WTSTools/WTSLogger.h"

USING_NS_WTP;

#include "WtOptTicker.h"

//=============================================================================
// WtOptEngine Implementation
//=============================================================================

WtOptEngine::WtOptEngine()
    : WtEngine()
    , _tm_ticker(nullptr)
    , _opt_cfg(nullptr)
    , _is_initialized(false)
    , _order_pool_size(4)
    , _trader(nullptr)
{
}

WtOptEngine::~WtOptEngine()
{
    stop();
    
    if (_opt_cfg) {
        _opt_cfg->release();
        _opt_cfg = nullptr;
    }
}

void WtOptEngine::init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, 
                       IHotMgr* hotMgr, EventNotifier* notifier)
{
    WtEngine::init(cfg, bdMgr, dataMgr, hotMgr, notifier);
    
    WTSLogger::info("Initializing Multi-Threaded Option Trading Engine...");
    
    if (cfg && cfg->has("option")) {
        _opt_cfg = cfg->get("option");
        if (_opt_cfg) _opt_cfg->retain();
    }
    
    // Initialize threading infrastructure
    init_threading();
    
    WTSLogger::info("Option Trading Engine initialized with {} order threads", _order_pool_size);
    _is_initialized = true;
}

void WtOptEngine::addContext(OptContextPtr ctx)
{
    if (ctx) {
        _ctx_map[ctx->id()] = ctx;

        // Initialize context components with engine config
        // Note: Casting to WtOptContext to access init method which is not in IOptStraCtx interface
        auto wtCtx = std::dynamic_pointer_cast<WtOptContext>(ctx);
        if (wtCtx) {
            wtCtx->init(_opt_cfg);
        }
        
        WTSLogger::debug("Option context added, id: {}, name: {}", ctx->id(), ctx->name());
    }
}

void WtOptEngine::init_threading()
{
    // Configure thread pool size from config
    if (_opt_cfg && _opt_cfg->has("threading")) {
        WTSVariant* threadCfg = _opt_cfg->get("threading");
        _order_pool_size = threadCfg->getUInt32("order_pool_size");
        if (_order_pool_size == 0) _order_pool_size = 4;
    }
    
    // Create order thread pool
    _order_pool.reset(new boost::threadpool::pool(_order_pool_size));
    WTSLogger::info("Order thread pool created with {} threads", _order_pool_size);
}

void WtOptEngine::stop_threads()
{
    // Wait for order pool to finish
    if (_order_pool) {
        _order_pool->wait();
        _order_pool.reset();
    }
}

void WtOptEngine::run()
{
    _tm_ticker = new WtOptRtTicker(this);
    _tm_ticker->run();
    
    WTSLogger::info("Option Engine started, {} contexts registered", _ctx_map.size());
}

void WtOptEngine::stop()
{
    // Stop background threads first
    stop_threads();
    
    // Stop ticker
    if (_tm_ticker) {
        _tm_ticker->stop();
        delete _tm_ticker;
        _tm_ticker = nullptr;
    }
    
    WTSLogger::info("Option Engine stopped");
}

void WtOptEngine::setTraderAdapter(TraderAdapter* trader)
{
    _trader = trader;
    
    if (_trader) {
        _trader->addSink(this);
        WTSLogger::info("TraderAdapter set: {}", _trader->id());
    }
}

void WtOptEngine::on_init()
{
    WtEngine::on_init();
    
    for (auto& v : _ctx_map) {
        if (v.second) {
            v.second->on_init();
        }
    }
    
    WTSLogger::info("Option Engine on_init completed, {} contexts initialized", _ctx_map.size());
}

void WtOptEngine::handle_push_quote(WTSTickData* newTick)
{
    if (newTick == nullptr) return;
    
    WtEngine::handle_push_quote(newTick);
    
    const char* code = newTick->code();
    on_tick(code, newTick);
}

void WtOptEngine::on_tick(const char* stdCode, WTSTickData* curTick)
{
    if (!_is_initialized || curTick == nullptr) return;
    
    // ==========================================================================
    // CRITICAL PATH - Keep synchronous for low latency (<200μs)
    // ==========================================================================
    
    // 1. Dispatch to contexts (Async Queue)
    for (auto& v : _ctx_map) {
        auto ctx = std::dynamic_pointer_cast<WtOptContext>(v.second);
        if (ctx) {
            ctx->enqueue_tick(stdCode, curTick);
        }
    }
}


void WtOptEngine::on_bar(const char* stdCode, const char* period, 
                         uint32_t times, WTSBarStruct* newBar)
{
    for (auto& v : _ctx_map) {
        if (v.second) {
            v.second->on_bar(stdCode, period, times, newBar);
        }
    }
}

void WtOptEngine::on_session_begin()
{
    WtEngine::on_session_begin();
    
    uint32_t curTDate = get_trading_date();
    WTSLogger::info("Option Engine session begin, trading date: {}", curTDate);
    
    for (auto& v : _ctx_map) {
        if (v.second) {
            v.second->on_session_begin(curTDate);
            
            // Also trigger risk/grid session begin if you expose those methods on Context 
            // or if Context.on_session_begin handles it. 
            // Assuming WtOptContext::on_session_begin propagates to stra only?
            // Ideally WtOptContext methods should handle internal components too.
            // But for now, WtOptContext::on_session_begin only calls strategy.
            // We should probably update WtOptContext to propagate to risk/grid.
            // But let's stick to the interface.
        }
    }
}

void WtOptEngine::on_session_end()
{
    WtEngine::on_session_end();
    
    uint32_t curTDate = get_trading_date();
    WTSLogger::info("Option Engine session end, trading date: {}", curTDate);
    
    for (auto& v : _ctx_map) {
        if (v.second) {
            v.second->on_session_end(curTDate);
        }
    }
}

void WtOptEngine::on_timer(uint32_t curDate, uint32_t curTime)
{
    for (auto& v : _ctx_map) {
        auto ctx = std::dynamic_pointer_cast<WtOptContext>(v.second);
        if (ctx) {
            ctx->enqueue_timer(curDate, curTime);
        }
    }
}

// void WtOptEngine::on_minute_end(uint32_t curDate, uint32_t curTime)
// {
//     // TODO: Dispatch minute end event to contexts for curve fitting if needed
// }

OptContextPtr WtOptEngine::getContext(uint32_t id)
{
    auto it = _ctx_map.find(id);
    if (it != _ctx_map.end()) {
        return it->second;
    }
    return OptContextPtr();
}

//=============================================================================
// ITrdNotifySink Implementation
//=============================================================================

void WtOptEngine::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
    // Dispatch to all contexts asynchronously
    for (auto& v : _ctx_map) {
        auto ctx = std::dynamic_pointer_cast<WtOptContext>(v.second);
        if (ctx) {
            ctx->enqueue_trade(localid, stdCode, isBuy, vol, price);
        }
    }
}

void WtOptEngine::on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled)
{
    for (auto& v : _ctx_map) {
        auto ctx = std::dynamic_pointer_cast<WtOptContext>(v.second);
        if (ctx) {
            ctx->enqueue_order(localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled);
        }
    }
}

void WtOptEngine::on_channel_ready()
{
    WTSLogger::info("Trading channel ready");
    // Trigger position sync for all contexts
    for (auto& v : _ctx_map) {
         auto ctx = std::dynamic_pointer_cast<WtOptContext>(v.second);
         if (ctx && ctx->get_risk() && _trader) {
             ctx->get_risk()->syncPositionsFromTrader(_trader);
         }
    }
}

void WtOptEngine::on_channel_lost()
{
    WTSLogger::warn("Trading channel lost");
}

void WtOptEngine::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
    if (!bSuccess) {
        WTSLogger::error("Order entrust failed: {}, {}, {}", localid, stdCode, message);
    }
}

void WtOptEngine::on_account(const char* currency, double prebalance, double balance, double dynbalance, double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw)
{
// WTSLogger::info("Account update: Balance {}", balance);
}
