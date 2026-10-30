/*!
 * \file OptionTimeService.h
 * \brief Per-strategy continuous event-time service.
 *
 * WonderTrader exposes seconds-of-day, which wraps at midnight.  Several
 * option-core modules use that value for durations (TPS window, TTL, EMA
 * decay, panic blackout, hedge cooldown), so a night session crossing 00:00
 * can freeze timers or make their deltas negative.
 *
 * This service keeps a per-instance day offset and detects only the normal
 * late-night → early-morning transition.  It deliberately does not try to be
 * a global clock: HftOptionStrategy owns one instance on the worker path and
 * resets it at session boundaries.
 */
#pragma once

#include "optioncoretypes.h"

namespace wt_option {

class OptionTimeService {
public:
    OptionTimeService() = default;

    /// Session-boundary hook.  The monotonic timeline intentionally survives:
    /// EMA filters and other stateful consumers may persist across sessions,
    /// and an idle period around midnight must not hide the day rollover.
    void reset() {
        // Deliberately no state change.  Keep both m_lastRawSecOfDay and
        // m_dayOffsetSec so the next sample can still be classified correctly.
    }

    /// Continuous strategy event time in seconds.  The context only needs the
    /// same interface as ctxTimeSeconds().
    template<typename Ctx>
    double nowSeconds(Ctx* ctx) {
        if (!ctx) return m_dayOffsetSec;
        const double raw = ctxTimeSeconds(ctx);

        // Detect the normal midnight rollover only.  Arbitrary backwards jumps
        // inside a session must not advance the synthetic day.
        if (m_haveRaw && m_lastRawSecOfDay > 23.0 * 3600.0 && raw < 1.0 * 3600.0)
            m_dayOffsetSec += 24.0 * 3600.0;

        m_lastRawSecOfDay = raw;
        m_haveRaw = true;
        return m_dayOffsetSec + raw;
    }

private:
    bool   m_haveRaw = false;
    double m_lastRawSecOfDay = 0.0;
    double m_dayOffsetSec = 0.0;
};

} // namespace wt_option
