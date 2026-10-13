/*!
 * \file TradingState.h
 * \brief Unified trading-state machine — absorbed-in-spirit from quantbox
 *        mm/TradingState (which itself is named-mode setters + derived enable).
 *
 * R8: single authoritative writer for the enabled/panicked flag pair. The old
 * scatter of `ctx->panicked = ...` across six sites was the root cause of the
 * panic-overwrite bug family (B13 et al.). All writers now go through
 * HftOptionStrategy::setState().
 */
#pragma once

#include <cstdint>
#include <string>

namespace wt_option {

enum class TradingStateType : uint8_t {
    Disabled = 0,       // channel lost / hot-cmd stop / session end / guard halt
    Normal,             // quoting
    SessionStopped,     // B4 mid-day section pause (quotes pulled)
    Panic               // risk-off: quotes cancelled, futures hedge kept alive
};

inline const char* toString(TradingStateType t) {
    switch (t) {
    case TradingStateType::Disabled:       return "Disabled";
    case TradingStateType::Normal:         return "Normal";
    case TradingStateType::SessionStopped: return "SessionStopped";
    case TradingStateType::Panic:          return "Panic";
    }
    return "?";
}

struct TradingState {
    TradingStateType type = TradingStateType::Disabled;
    std::string      reason = "init";

    bool channelReady    = false;   // TD front OK
    bool underlyingReady = false;   // seen a valid underlying tick (deferred enable)

    /// Derived flag pair consumed by ControllableTradingGrid.
    /// Panic stays "enabled" so refresh() can stage risk-reduction cancels
    /// with boosted TPS while new orders are suppressed downstream.
    bool enabledFlag() const {
        switch (type) {
        case TradingStateType::Normal:         return channelReady && underlyingReady;
        case TradingStateType::Panic:          return channelReady;
        case TradingStateType::SessionStopped: return false;
        case TradingStateType::Disabled:       return false;
        }
        return false;
    }
    bool panickedFlag() const { return type == TradingStateType::Panic; }
};

} // namespace wt_option
