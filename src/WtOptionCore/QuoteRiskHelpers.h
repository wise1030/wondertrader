/*!
 * \file QuoteRiskHelpers.h
 * \brief Pure, unit-testable helpers for the v3 risk-shift design
 *        (analysis/quote-shift-design-proposal-v3.md).
 *
 * quadraticShift  — L3 secondary risk shift (original quantbox form, revived):
 *                   s = −sign(pgn) · distort · pgn², clamped to ±maxShift.
 *                   Price units (元); 0 when distort off (0).
 * riskSizeScale   — soft-limit size shrink: the side that WORSENS a risk
 *                   dimension shrinks linearly once |pgn| passes shrinkStart,
 *                   reaching 0 at 1.0 (directional suppression). The reducing
 *                   side keeps full size.
 * teenieCapPrice  — original MMPricer teenie semantics: cap our quote at
 *                   market best ± 1 tick (queue priority, never crossing).
 *                   Requires a market price on that side; the caller must
 *                   skip the side when absent (mirrors the original's
 *                   `return false`).
 * stickyScaleFor  — sticky band downweight under risk pressure: 1.0 while
 *                   maxRisk ≤ 0.5, linearly down to 0.0 at maxRisk ≥ 1.0.
 */
#pragma once

#include <algorithm>
#include <cmath>

namespace wt_option {

namespace quote_risk {

enum class Side { BID, ASK };

inline double clampd(double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
}

// L3 secondary shift. pgn = normalized risk (0 at target). distort in price
// units per pgn²; maxShift in price units. Returns 0 when distort <= 0.
inline double quadraticShift(double pgn, double distort, double maxShift) {
    if (!(distort > 0)) return 0.0;
    if (pgn == 0) return 0.0;
    const double s = -((pgn > 0) ? 1.0 : -1.0) * distort * pgn * pgn;
    return clampd(s, -maxShift, maxShift);
}

// Per-expiry ddAdj: linear raw-month-net-delta quote translation. Unlike L3,
// it has no target/cross-expiry input and does not scale by |leg delta|.
inline double expiryDeltaSkew(double expiryNetDelta, double legDelta, double weight) {
    if (weight == 0.0) return 0.0;
    return -weight * expiryNetDelta * (legDelta >= 0.0 ? 1.0 : -1.0);
}

// Remove the current instrument's contribution from an aggregated vega
// bucket. The bucket optimization must be equivalent to the original
// per-position `instr1 != instr0` loop even when other codes share the bucket;
// the own exact-delta term is added separately by the caller.
inline double vegaBucketWithoutSelf(double bucketBaseVega, bool containsSelf,
                                    double selfPosition, double selfContractSize,
                                    double bucketRepDelta, double lambdaVegaWing) {
    if (!containsSelf) return bucketBaseVega;
    const double selfWing = std::exp(-lambdaVegaWing * std::abs(bucketRepDelta - 0.5));
    return bucketBaseVega - selfPosition * selfContractSize * selfWing;
}

// Size shrink for one quote side.
//   deltaSign: sign of the leg's own delta (+1 call / −1 put / 0 unknown).
//   side:      which side we are quoting.
//   pgnDelta:  normalized portfolio delta risk (target-relative).
//   pgnVega:   normalized portfolio vega risk.
//   shrinkStart: |pgn| where the worsening side starts shrinking (0.8).
// Buying a leg moves the portfolio by +deltaSign; selling by −deltaSign.
// Buying ANY option moves vega up; selling moves it down.
// A side only shrinks when it worsens the risk (move × pgn > 0).
inline double riskSizeScale(Side side, double deltaSign,
                            double pgnDelta, double pgnVega,
                            double shrinkStart) {
    const double moveD = (side == Side::BID ? 1.0 : -1.0) * deltaSign;
    const double moveV = (side == Side::BID ? 1.0 : -1.0);
    // shrinkStart >= 1.0 disables the shrink entirely (legacy behavior).
    if (shrinkStart >= 1.0) return 1.0;
    const double span = std::max(1e-9, 1.0 - shrinkStart);

    double scale = 1.0;
    if (pgnDelta != 0 && deltaSign != 0) {
        // SIGNED: move × pgn > 0 worsens the risk, < 0 reduces it (the
        // reducing side keeps full size — that is the whole point).
        const double worsen = moveD * pgnDelta;
        if (worsen > shrinkStart)
            scale -= clampd((worsen - shrinkStart) / span, 0.0, 1.0);
    }
    if (pgnVega != 0) {
        const double worsenV = moveV * pgnVega;
        if (worsenV > shrinkStart)
            scale -= clampd((worsenV - shrinkStart) / span, 0.0, 1.0);
    }
    return clampd(scale, 0.0, 1.0);
}

// Teenie cap (original MMPricer semantics): our bid may improve the market
// best by at most one tick; mirrored for the ask. Returns the capped price.
// mktPx must be the market best on THIS side (> 0); the caller skips the
// side entirely when absent (original returns false → erase that side).
inline double teenieCapPrice(Side dir, double ourPx, double mktPx,
                             double tickSize) {
    const double ts = tickSize > 0 ? tickSize : 1e-6;
    if (dir == Side::BID)
        return std::min(ourPx, mktPx + ts);
    return std::max(ourPx, mktPx - ts);
}

// Sticky band downweight: 1.0 at maxRisk ≤ 0.5, linear to 0.0 at ≥ 1.0.
inline double stickyScaleFor(double maxRisk) {
    return clampd(1.0 - (maxRisk - 0.5) / 0.5, 0.0, 1.0);
}

// ---- v4 panic redesign (analysis/panic-design-benchmark-plan.md) ----

// Does a BUY/SELL order REDUCE the portfolio delta exposure?
// pfDelta > 0 (long) → sells reduce; < 0 (short) → buys reduce; 0 → nothing.
inline bool reducesDelta(bool isBuy, double pfDelta) {
    if (pfDelta > 0) return !isBuy;
    if (pfDelta < 0) return isBuy;
    return false;
}

// Does a BUY/SELL order REDUCE the portfolio vega exposure?
// Same shape: long vega → sells reduce; short vega → buys reduce.
inline bool reducesVega(bool isBuy, double pfVega) {
    if (pfVega > 0) return !isBuy;
    if (pfVega < 0) return isBuy;
    return false;
}

// Lenient reducing test: the order reduces EITHER risk dimension.
inline bool isReducingOrder(bool isBuy, double pfDelta, double pfVega) {
    return reducesDelta(isBuy, pfDelta) || reducesVega(isBuy, pfVega);
}

// Panic blackout escalation: a re-arm DURING (or right after) the previous
// blackout means the condition never cleared — double the blackout (capped).
// A clean re-arm (previous blackout expired long ago) resets to base.
inline double escalateBlackout(double secSinceLastTrigger, double curBlackout,
                               double base, double maxBlackout) {
    if (secSinceLastTrigger <= curBlackout)
        return clampd(curBlackout * 2.0, base, maxBlackout);
    return base;
}

} // namespace quote_risk
} // namespace wt_option
