/*!
 * \file WtOptTicker.h
 * \project WonderTrader
 *
 * \brief Option engine's background ticker thread. Extracted from WtOptEngine.
 */
#pragma once

#include <thread>
#include <atomic>
#include <chrono>

#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN

class WtOptEngine;

class WtOptRtTicker
{
public:
    WtOptRtTicker(WtOptEngine* engine);
    ~WtOptRtTicker();

    void run();
    void stop();

private:
    WtOptEngine* _engine;
    std::atomic<bool> _stopped;
    std::thread* _thread;
};

NS_WTP_END
