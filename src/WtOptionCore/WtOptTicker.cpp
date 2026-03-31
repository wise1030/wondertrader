/*!
 * \file WtOptTicker.cpp
 * \project WonderTrader
 *
 * \brief Option engine's background ticker thread implementation.
 */
#include "WtOptTicker.h"
#include "WtOptEngine.h"
#include "../Share/TimeUtils.hpp"

NS_WTP_BEGIN

WtOptRtTicker::WtOptRtTicker(WtOptEngine* engine) 
    : _engine(engine)
    , _stopped(false)
    , _thread(nullptr)
{
}

WtOptRtTicker::~WtOptRtTicker()
{
    stop();
}

void WtOptRtTicker::run() 
{
    _thread = new std::thread([this]() {
        while (!_stopped) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            if (_stopped) break;
            
            uint64_t now = TimeUtils::getLocalTimeNow();
            uint32_t curDate = (uint32_t)(now / 1000000000);
            uint32_t curTime = (uint32_t)((now % 1000000000) / 100000);
            
            if (_engine) {
                _engine->on_timer(curDate, curTime);
            }
        }
    });
}

void WtOptRtTicker::stop() 
{ 
    _stopped = true; 
    if (_thread && _thread->joinable()) {
        _thread->join();
        delete _thread;
        _thread = nullptr;
    }
}

NS_WTP_END
