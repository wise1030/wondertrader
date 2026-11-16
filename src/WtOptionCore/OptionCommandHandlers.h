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
    /// P2: cancel one order by framework localid (UI double-click cancel).
    using CancelOneFn = std::function<bool(uint32_t localid, std::string& err)>;
    /// P2: hot parameter write. The strategy validates the whitelist and
    /// applies via its existing hot-update path (sync_param ptr when present,
    /// member + applyHotParams otherwise). Only truly hot-effective keys are
    /// accepted (5 alpha weights + max_tps).
    using SetHotParamFn = std::function<bool(const std::string& key, double value, std::string& err)>;
    /// P3: single-code quote mode (UI double-click pos toggle).
    using SetQModeOneFn = std::function<bool(const std::string& code, const std::string& mode, std::string& err)>;
    /// P3: cancel every live order (UI Cancel All).
    using CancelAllLiveFn = std::function<uint32_t()>;   // returns cancels issued

    struct Hooks {
        SetStateFn setState;
        ChannelReadyFn channelReady;
        SetQModeFn setQModeAll;
        ExecuteOrderFn executeOrder;
        CancelAllFn cancelAll;
        CancelOneFn cancelOne;
        SetHotParamFn setHotParam;
        SetQModeOneFn setQModeOne;
        CancelAllLiveFn cancelAllLive;
    };

    void registerAll(CommandBus& bus, Hooks hooks);

private:
    Hooks m_hooks;
};

} // namespace wt_option
