/*!
 * \file IOptStraCtx.h
 * \project	WonderTrader
 *
 * \brief Option Strategy Context Interface
 * 
 * Enhanced interface following WonderTrader patterns for data access and trading.
 */
#pragma once
#include <stdint.h>
#include <string>
#include "../Includes/WTSMarcos.h"

// Forward declarations for option types
namespace wt_option {
    class OptionGrid;
    class OptionRisk;
    class GridOrderManager;
    class CurveFitter;
}

NS_WTP_BEGIN

class WTSTickData;
class WTSTickSlice;
class WTSKlineSlice;
struct WTSBarStruct;
class WTSCommodityInfo;

// Forward declarations for option types


/**
 * @brief Option Strategy Context Interface
 * 
 * Provides the interface between option strategies and the option engine.
 * Enhanced to include data access and trading methods following IHftStraCtx patterns.
 */
class IOptStraCtx
{
public:
    IOptStraCtx(const char* name) :_name(name){}
    virtual ~IOptStraCtx(){}

    inline const char* name() const{ return _name.c_str(); }

public:
    //==========================================================================
    // Identity
    //==========================================================================
    
    virtual uint32_t id() = 0;

    //==========================================================================
    // Callbacks from Engine
    //==========================================================================
    
    virtual void on_init() = 0;
    virtual void on_session_begin(uint32_t uTDate) = 0;
    virtual void on_session_end(uint32_t uTDate) = 0;
    virtual void on_tick(const char* stdCode, WTSTickData* newTick) = 0;
    virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) = 0;
    virtual void on_calculate(uint32_t curDate, uint32_t curTime) = 0;

    //==========================================================================
    // Option Component Access
    //==========================================================================
    
    virtual wt_option::OptionGrid* stra_get_grid() = 0;
    virtual wt_option::OptionRisk* stra_get_risk() = 0;
    virtual wt_option::GridOrderManager* stra_get_order_manager() = 0;

    //==========================================================================
    // Logging (using WTSLogger)
    //==========================================================================
    
    virtual void stra_log_info(const char* message) = 0;
    virtual void stra_log_error(const char* message) = 0;
    virtual void stra_log_warn(const char* message) = 0;
    virtual void stra_log_debug(const char* message) = 0;

    //==========================================================================
    // Time Access (delegated to WtEngine)
    //==========================================================================
    
    virtual uint32_t stra_get_date() = 0;
    virtual uint32_t stra_get_time() = 0;
    virtual uint32_t stra_get_secs() = 0;
    virtual uint32_t stra_get_tdate() = 0;  ///< Trading date

    //==========================================================================
    // Data Access (via WtDtMgr)
    //==========================================================================
    
    /**
     * @brief Get tick slice for a code
     * @param stdCode Standard code
     * @param count Number of ticks to retrieve
     * @return Tick slice, caller responsible for release
     */
    virtual WTSTickSlice* stra_get_ticks(const char* stdCode, uint32_t count) = 0;
    
    /**
     * @brief Get last tick for a code
     * @param stdCode Standard code
     * @return Last tick data, caller responsible for release
     */
    virtual WTSTickData* stra_get_last_tick(const char* stdCode) = 0;
    
    /**
     * @brief Get kline bars
     * @param stdCode Standard code
     * @param period Period (m1, m5, d1)
     * @param count Number of bars
     * @return Kline slice
     */
    virtual WTSKlineSlice* stra_get_bars(const char* stdCode, const char* period, uint32_t count) = 0;
    
    /**
     * @brief Get current price for a code
     * @param stdCode Standard code
     * @return Current price
     */
    virtual double stra_get_price(const char* stdCode) = 0;

    //==========================================================================
    // Trading Actions (via TraderAdapter)
    //==========================================================================
    
    /**
     * @brief Submit buy order
     * @param stdCode Standard code
     * @param price Order price
     * @param qty Order quantity
     * @param userTag User tag for tracking
     * @return Local order ID
     */
    virtual uint32_t stra_buy(const char* stdCode, double price, double qty, const char* userTag) = 0;
    
    /**
     * @brief Submit sell order
     * @param stdCode Standard code
     * @param price Order price
     * @param qty Order quantity
     * @param userTag User tag for tracking
     * @return Local order ID
     */
    virtual uint32_t stra_sell(const char* stdCode, double price, double qty, const char* userTag) = 0;
    
    /**
     * @brief Submit dual-sided quote (Market Making)
     * @param stdCode Standard code
     * @param bidPrice Bid price
     * @param bidQty Bid quantity
     * @param askPrice Ask price
     * @param askQty Ask quantity
     * @param userTag User tag for tracking
     * @return Local quote ID
     */
    virtual uint32_t stra_quote(const char* stdCode, double bidPrice, uint32_t bidQty, double askPrice, uint32_t askQty, const char* userTag) = 0;
    
    /**
     * @brief Cancel order
     * @param localid Local order ID
     * @return True if cancel request sent
     */
    virtual bool stra_cancel(uint32_t localid) = 0;
    
    /**
     * @brief Cancel a quote by localid
     * @param localid Local quote ID
     * @return True if cancel request sent
     */
    virtual bool stra_cancel_quote(uint32_t localid) = 0;
    
    /**
     * @brief Cancel all orders for a code
     * @param stdCode Standard code
     * @return Number of orders cancelled
     */
    virtual uint32_t stra_cancel_all(const char* stdCode) = 0;
    
    /**
     * @brief Get current position for a code
     * @param stdCode Standard code
     * @return Position quantity (positive for long, negative for short)
     */
    virtual double stra_get_position(const char* stdCode) = 0;

    //==========================================================================
    // Subscription
    //==========================================================================
    
    /**
     * @brief Subscribe to tick data
     * @param stdCode Standard code
     */
    virtual void stra_sub_ticks(const char* stdCode) = 0;

protected:
    std::string _name;
};

NS_WTP_END
