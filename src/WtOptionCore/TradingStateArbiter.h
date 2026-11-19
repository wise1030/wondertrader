/*!
 * \file TradingStateArbiter.h
 * \brief Single decision point for the trading mode (P1 batch-3.1).
 *
 * Previously each signal writer called setState directly and later writers
 * silently overwrote earlier decisions (observed: channel_ready overwrote
 * SessionStopped 4s after startup reconcile; a stale session event flipped
 * a close-stop into Recovering). The arbiter collects every input and
 * derives the ONE effective mode with explicit priorities, so any
 * combination of signals is deterministic and log-traceable.
 *
 * Pure function — unit-testable without WT runtime. HftOptionStrategy
 * feeds the inputs; setState applies the result.
 */
#pragma once

#include "TradingState.h"

#include <cstdint>
#include <set>
#include <string>

namespace wt_option {

struct TradingStateInputs {
    bool channelReady     = true;   // false -> Disabled (channel lost)
    bool sessionStopped   = false;  // mid-day break / session stop zone
    bool panicked         = false;  // pricer/checker/limits panic OR-chain
    bool recovering       = false;  // restart/reconnect recovery gate open
    bool getFlat          = false;  // close-flatten armed (timed or immediate)
};

enum class ArbiterMode : uint8_t {
    Disabled = 0,
    Panic,
    SessionStopped,
    Recovering,
    GetFlat,
    Normal,
};

struct ArbiterDecision {
    ArbiterMode mode = ArbiterMode::Normal;
    const char* reason = "normal";
};

/// Priority: Disabled > Panic > SessionStopped > Recovering > GetFlat > Normal.
/// A GetFlat request never masks a panic/stop — it only refines Normal.
inline ArbiterDecision evaluateTradingState(const TradingStateInputs& in) {
    if (!in.channelReady)   return { ArbiterMode::Disabled, "channel_lost" };
    if (in.panicked)        return { ArbiterMode::Panic, "risk_signal" };
    if (in.sessionStopped)  return { ArbiterMode::SessionStopped, "session_stop" };
    if (in.recovering)      return { ArbiterMode::Recovering, "recovering" };
    if (in.getFlat)         return { ArbiterMode::GetFlat, "close_flatten" };
    return { ArbiterMode::Normal, "normal" };
}

// ---------------------------------------------------------------------------
// Transition guard — single deterministic downgrade policy used by
// HftOptionStrategy::setState. Priority ranks below; a downgrade is honoured
// only for an explicit recovery reason.
// ---------------------------------------------------------------------------
inline int tradingModeRank(TradingStateType m) {
    switch (m) {
    case TradingStateType::Disabled:       return 6;
    case TradingStateType::Panic:          return 5;
    case TradingStateType::SessionStopped: return 4;
    case TradingStateType::Recovering:     return 3;
    case TradingStateType::GetFlat:        return 2;
    case TradingStateType::Normal:         return 1;
    }
    return 0;
}

inline bool shouldAllowTransition(TradingStateType prev, TradingStateType next,
                                  const std::string& reason) {
    if (tradingModeRank(next) >= tradingModeRank(prev)) return true;  // upgrade/same
    // Downgrades are matched on (origin, reason) — a reason that is a legal
    // recovery from ONE mode must not unlock an unrelated downgrade from
    // another (e.g. "channel_ready" is Disabled->Recovering only; it must
    // not flip SessionStopped back to Recovering).
    switch (prev) {
    case TradingStateType::Disabled:
        return reason == "channel_ready";
    case TradingStateType::SessionStopped:
        return reason == "session_resume";
    case TradingStateType::Panic:
        return reason == "blackout_expired";
    case TradingStateType::Recovering:
        return reason == "recovery_first_underlying_tick";
    case TradingStateType::GetFlat:
        return reason == "close_flatten_disarmed";
    case TradingStateType::Normal:
        return false;
    }
    // Operator hot-commands are always honoured regardless of origin.
    return reason.rfind("hotcmd_", 0) == 0;
}

} // namespace wt_option
