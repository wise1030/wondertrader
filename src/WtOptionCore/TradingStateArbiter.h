namespace wt_option {

/*!
 * \file TradingStateArbiter.h
 * \brief Downgrade guard for the trading-mode state machine (P1 batch-3.1).
 *
 * Audit follow-up (2026-11-20): the original design also carried an
 * "evaluateTradingState" input-collector that derived the effective mode
 * from signal inputs.  Only the transition guard half was ever wired
 * (setState consumes shouldAllowTransition); the evaluator sat dead, so
 * it was removed — git history keeps the design record.  The guard below
 * is the production facility: priority-ranked upgrades always pass, and
 * downgrades are honoured only for an explicit (origin, reason) recovery
 * whitelist — the generalised fix for the channel_ready/stale-event
 * overwrite incidents.
 */

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
    // Operator hot-commands are ALWAYS honoured regardless of origin — the
    // switch below must never swallow them.  (Bug fixed 11-20: the per-origin
    // switch made this trailing check unreachable, so RESUME after a STOP
    // stayed Disabled forever — observed live in the night session.)
    if (reason.rfind("hotcmd_", 0) == 0) return true;
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
