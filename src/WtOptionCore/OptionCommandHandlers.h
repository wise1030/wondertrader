/*!
 * \file OptionCommandHandlers.h
 * \brief Phase 2 seam: STOP/RESUME/PANIC/QMODE/ORDER/CANCEL command wiring.
 *
 * The handlers depend only on small hooks, so they are testable without the
 * HftOptionStrategy shell and the shell no longer owns command parsing.
 */
#pragma once

#include "CommandBus.h"
#include "TradingState.h"

#include <cstdint>
#include <functional>
#include <string>

namespace wt_option {

class OptionCommandHandlers {
public:
    using SetStateFn = std::function<void(TradingStateType, const std::string&)>;
    using ChannelReadyFn = std::function<bool()>;
    using SetQModeFn = std::function<bool(const std::string& mode, std::string& err)>;
    using ExecuteOrderFn = std::function<int32_t(const std::string& code, bool isBuy,
                                                double price, uint32_t qty)>;
    using CancelAllFn = std::function<void(const std::string& code)>;

    struct Hooks {
        SetStateFn setState;
        ChannelReadyFn channelReady;
        SetQModeFn setQModeAll;
        ExecuteOrderFn executeOrder;
        CancelAllFn cancelAll;
    };

    void registerAll(CommandBus& bus, Hooks hooks);

private:
    Hooks m_hooks;
};

} // namespace wt_option
