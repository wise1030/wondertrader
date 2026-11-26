/*!
 * \file OptionHedgeCoordinator.h
 * \brief Phase 2 seam: portfolio delta-hedge decision + in-flight ownership.
 *
 * Extracted from HftOptionStrategy::checkDeltaHedge. The strategy keeps config
 * and price/order hooks; the coordinator owns the pure decision, cooldown,
 * in-flight suppression and terminal bookkeeping (unit-testable without WT).
 *
 * P1 (2026-11-18): multi-leg support. The coordinator can now hold one
 * EXCLUSIVE leg per expiry (each expiry hedged by its own month future —
 * strictly attributed, no double counting) plus one SHARED portfolio leg
 * (e.g. a single CZCE future for several series) that hedges the residual
 * after exclusive legs' targets. The legacy single-leg API (configure with
 * a hedge code + check(portfolioDelta)) stays as the degenerate one-leg
 * path and is used by the close-flatten mode.
 */
#pragma once

#include "DeltaHedgeExecutor.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wt_option {

class OptionHedgeCoordinator {
public:
    /// Must return ALL framework localids created for the hedge batch.  A
    /// boolean success value (e.g. 1) is not a valid order id and would leave
    /// the in-flight latch stuck forever.
    using SendFn = std::function<std::vector<uint32_t>(const std::string& code,
                                                       bool isBuy, double price,
                                                       uint32_t qty)>;
    using PriceFn = std::function<double(const std::string& code)>;
    using LogFn = std::function<void(const std::string& message)>;
    /// P1: per-expiry net-delta source (option + exclusive legs), and the
    /// portfolio net delta for the shared residual leg.
    using ExpDeltaFn = std::function<double(uint32_t exp)>;

    void configure(const DeltaHedgeExecutor::Config& cfg, double deltaPerLot,
                   std::string hedgeCode, SendFn send, PriceFn price,
                   LogFn log = {});
    void setHedgeCode(std::string code);
    void setDeltaPerLot(double v);
    const DeltaHedgeExecutor::Config& config() const { return m_cfg; }

    // ---- P1 multi-leg API ----
    /// One exclusive leg per expiry; each leg hedges totalDelta(exp) with its
    /// own month future. Empty vector = fall back to the legacy single leg.
    struct Leg { uint32_t exp = 0; std::string code; };
    void setLegs(std::vector<Leg> legs) { m_legs = std::move(legs); }
    void setSharedLeg(std::string code) { m_sharedCode = std::move(code); }

    // ---- v3 risk-tier API (quote-shift-design-proposal-v3.md §4) ----
    // Futures hedge is the risk-control FALLBACK: the strategy computes the
    // normalized delta exposure each cycle and arms the bail-out threshold
    // only past bail_pgn. effThreshold <= 0 = use m_cfg.thresholdDelta
    // (legacy). targetDelta aligns the hedge with the operator's targ_delta
    // (same source as the L3 quote-shift anchor).
    void setEffectiveThreshold(double th) { m_effThreshold = th; }
    void setPortfolioTargetDelta(double t) { m_targetDelta = t; }
    double effectiveThreshold() const {
        return m_effThreshold > 0 ? m_effThreshold : m_cfg.thresholdDelta;
    }
    /// Multi-leg check: every leg whose |net delta| breaches the threshold is
    /// rebalanced independently; the shared leg (if configured) hedges
    /// portfolioNetDelta MINUS the target delta of exclusive legs' in-flight
    /// orders, so the two tiers never double-hedge the same gap.
    /// Returns the number of legs that fired.
    size_t checkMulti(double nowSec, const ExpDeltaFn& expDeltaFn,
                      const ExpDeltaFn& portfolioNetFn);

    /// Returns true iff a hedge order was sent. `portfolioDelta` is the
    /// multiplier-scaled portfolio delta.
    bool check(double nowSec, double portfolioDelta);
    /// check with an explicit config override (close-flatten mode passes a
    /// tight target-0 config while the coordinator keeps the backstop one).
    /// priceOverride > 0 replaces the quote-fn price for this round (the
    /// close-flatten path crosses the spread to fill in the close window).
    bool check(const DeltaHedgeExecutor::Config& cfg, double nowSec,
               double portfolioDelta, double priceOverride = 0);
    /// Snapshot of currently in-flight hedge order ids (the strategy cancels
    /// them when disarming close-flatten so nothing rests through the break).
    std::vector<uint32_t> inflightIds() const;

    /// Terminal callback for any order id (cancel/reject/full fill).
    /// P1 follow-up: backoff-aware terminal report.
    ///  Reject — entrust rejection of a hedge order: streak++ (backoff
    ///         cooldown x 2^streak, cap 32x).
    ///  Reset  — hedge order terminal WITHOUT a reject (filled/canceled
    ///         normally): streak = 0.
    ///  Ignore — terminal of an unrelated order (option OQM terminals ride
    ///         the same strategy callback): leaves the streak alone.
    /// The Ignore tier exists because option OQM terminals (every reprice
    /// cancel/fill) arrive on the same callback and used to reset the
    /// streak dozens of times per second, neutralising the backoff.
    enum class TermAction { Ignore, Reset, Reject };
    void onTerminal(uint32_t localid, TermAction action = TermAction::Ignore);

    bool hasInflight() const;
    double lastFireTime() const { return m_lastFire; }

private:
    struct InflightRec { int legIdx; double targetDelta; };  // legIdx -1 = shared
    bool fireLeg(int legIdx, const std::string& code, double netDelta,
                 double nowSec, double priceOverride = 0);

    DeltaHedgeExecutor::Config m_cfg;
    // v3 risk-tier: armed bail-out threshold + operator target (0 = legacy).
    double m_effThreshold = 0.0;
    double m_targetDelta = 0.0;
    std::string m_code;
    SendFn m_send;
    PriceFn m_price;
    LogFn m_log;

    mutable std::mutex m_mtx;
    std::unordered_map<uint32_t, InflightRec> m_inflight;
    double m_lastFire = -1e9;
    int m_rejectStreak = 0;   // consecutive entrust rejects (backoff input)

    // P1 multi-leg state
    std::vector<Leg> m_legs;
    std::string m_sharedCode;
    std::vector<double> m_legLastFire;
    double m_sharedLastFire = -1e9;
};

} // namespace wt_option
