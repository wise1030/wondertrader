/*!
 * \file OptStrategy.h
 * \brief Base class for Option Strategies
 */
#pragma once
#include "../Includes/IOptStraCtx.h"

NS_WTP_BEGIN

class OptStrategy
{
public:
    OptStrategy(const char* id) : _id(id) {}
    virtual ~OptStrategy() {}

    const char* id() const { return _id.c_str(); }

    void set_ctx(IOptStraCtx* ctx) { _ctx = ctx; }

public:
    // Callbacks
    virtual void on_init(IOptStraCtx* ctx) = 0;
    virtual void on_session_begin(IOptStraCtx* ctx, uint32_t uTDate) {}
    virtual void on_session_end(IOptStraCtx* ctx, uint32_t uTDate) {}
    virtual void on_tick(IOptStraCtx* ctx, const char* stdCode, WTSTickData* newTick) {}
    virtual void on_tick_batch(IOptStraCtx* ctx) {}
    virtual void on_calculate(IOptStraCtx* ctx, uint32_t curDate, uint32_t curTime) {}

protected:
    std::string _id;
    IOptStraCtx* _ctx;
};

NS_WTP_END
