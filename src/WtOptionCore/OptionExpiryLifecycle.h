/*!
 * \file OptionExpiryLifecycle.h
 * \brief Phase 2 seam: expiry roll/cleanup and session-end hard stop.
 *
 * Owns the "session boundary" actions that used to be inline in
 * HftOptionStrategy: refresh/purge/roll expiries, cancel resting quotes and
 * report residual gross positions.
 */
#pragma once

#include "OptionGrid.h"
#include "IHftStraCtx.h"

#include <cstddef>
#include <functional>
#include <unordered_map>
#include <string>

namespace wt_option {

class OptionExpiryLifecycle {
public:
    using AlertFn = std::function<void(double gross, size_t codes)>;

    /// refresh days → purge expired expiries → roll the front month.
    static void onSessionBegin(OptionGrid& grid);

    /// Cancel all strategy orders and report residual gross position. `ctx`
    /// may be null in tests/backtest.
    static void onSessionEnd(wtp::IHftStraCtx* ctx,
                             const std::unordered_map<std::string, double>& positions,
                             const AlertFn& alert = {});

    /// Sum |position| across codes (helper, unit-tested).
    static double grossPosition(const std::unordered_map<std::string, double>& positions);
};

} // namespace wt_option
