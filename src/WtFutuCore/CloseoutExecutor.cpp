/*!
 * \file CloseoutExecutor.cpp
 * \brief Urgency-driven progressive closeout hedging executor implementation
 *
 * Design doc: docs/CLOSEOUT_EXECUTOR_DESIGN.md
 *
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#include "CloseoutExecutor.h"
#include "UnifiedOrderTracker.h"
#include "FutuPortfolio.h"
#include "../Includes/IUftStraCtx.h"
#include "../WTSTools/WTSLogger.h"
#include <algorithm>
#include <cmath>

namespace futu {

CloseoutExecutor::CloseoutExecutor()
{
}

void CloseoutExecutor::start(wtp::IUftStraCtx* ctx,
                              const char* code,
                              uint64_t close_time_ms,
                              double hedge_ratio)
{
    _phase           = CloseoutSub::DRAINING;
    std::strncpy(_code, code, sizeof(_code) - 1);
    _code[sizeof(_code) - 1] = '\0';
    _close_time_ms   = close_time_ms;
    _hedge_ratio     = hedge_ratio;
    _remaining       = 0;
    _total_to_hedge  = 0;
    _total_filled    = 0;
    _sweep_done      = false;
    _prev_round_pos  = 0;
    _prev_round_ts   = 0;
    _prev_round_qty  = 0;
    _rounds.clear();

    // Drain start timestamp: captured on first handleDraining call from snap.timestamp_ms
    // (must be in same time domain as snap.timestamp_ms = ms-from-midnight)
    _drain_start_ts = 0;
    _start_ts = 0;

    WTSLogger::info("CloseoutExecutor[{}] START: code={}, close_time_ms={}, hedge_ratio={}",
                    (void*)this, _code, close_time_ms, hedge_ratio);
}

void CloseoutExecutor::reset()
{
    _phase          = CloseoutSub::IDLE;
    _remaining      = 0;
    _total_to_hedge = 0;
    _total_filled   = 0;
    _inflight_qty   = 0;
    _sweep_done     = false;
    _prev_round_pos = 0;
    _prev_round_ts  = 0;
    _prev_round_qty = 0;
    _code[0]        = '\0';
    _rounds.clear();
}

void CloseoutExecutor::run(wtp::IUftStraCtx* ctx, const MarketSnapshot& snap)
{
    if (_phase == CloseoutSub::IDLE || _phase == CloseoutSub::COMPLETED)
        return;

    switch (_phase)
    {
        case CloseoutSub::DRAINING:
            handleDraining(ctx, snap);
            // If drain completed, fall through to assessing this tick
            if (_phase == CloseoutSub::ASSESSING)
                handleAssessing(ctx, snap);
            break;

        case CloseoutSub::ASSESSING:
            handleAssessing(ctx, snap);
            break;

        case CloseoutSub::EXECUTING:
            handleExecuting(ctx, snap);
            break;

        default:
            break;
    }
}

//==========================================================================
// Phase 1: DRAIN
//==========================================================================
bool CloseoutExecutor::handleDraining(wtp::IUftStraCtx* ctx,
                                       const MarketSnapshot& snap)
{
    // Capture drain start timestamp on first call (same time domain as snap)
    uint64_t now_ms = snap.timestamp_ms;
    if (_drain_start_ts == 0)
    {
        _drain_start_ts = now_ms;
        _start_ts = now_ms;
    }
    uint32_t elapsed = static_cast<uint32_t>(now_ms - _drain_start_ts);

    // Check drain completion
    bool drain_ok = false;
    if (_tracker)
    {
        drain_ok = (_tracker->getOrderCount() == 0);
    }
    else
    {
        // No tracker — assume drain ok (best effort)
        drain_ok = true;
    }

    if (drain_ok)
    {
        WTSLogger::info("CloseoutExecutor[{}] DRAIN complete after {}ms (orders={})",
                        (void*)this, elapsed,
                        _tracker ? _tracker->getOrderCount() : -1);
        _phase = CloseoutSub::ASSESSING;
        return true;
    }

    // Timeout check
    if (elapsed >= _cfg.drain_timeout_ms)
    {
        WTSLogger::warn("CloseoutExecutor[{}] DRAIN timeout after {}ms, {} orders still active — proceeding anyway",
                        (void*)this, elapsed,
                        _tracker ? _tracker->getOrderCount() : -1);
        _phase = CloseoutSub::ASSESSING;
        return true;
    }

    return false; // still draining
}

//==========================================================================
// Phase 2: ASSESS
//==========================================================================
void CloseoutExecutor::handleAssessing(wtp::IUftStraCtx* ctx,
                                        const MarketSnapshot& snap)
{
    if (!_portfolio)
    {
        WTSLogger::error("CloseoutExecutor[{}] ASSESS: portfolio is null", (void*)this);
        _phase = CloseoutSub::FAILED;
        return;
    }

    double net_delta = _portfolio->getNetDelta();

    if (std::abs(_hedge_ratio) < 1e-9)
    {
        WTSLogger::error("CloseoutExecutor[{}] ASSESS: invalid hedge_ratio={}",
                         (void*)this, _hedge_ratio);
        _phase = CloseoutSub::FAILED;
        return;
    }

    _remaining      = -net_delta / _hedge_ratio;
    _total_to_hedge = std::abs(_remaining);

    if (std::abs(_remaining) < 0.5)
    {
        WTSLogger::info("CloseoutExecutor[{}] ASSESS: already flat (remaining={:.2f}), COMPLETED",
                        (void*)this, _remaining);
        complete();
        return;
    }

    WTSLogger::warn("CloseoutExecutor[{}] ASSESS: remaining={:.2f} lots (net_delta={:.4f}), entering EXECUTE",
                    (void*)this, _remaining, net_delta);

    _phase = CloseoutSub::EXECUTING;
    _prev_round_pos = _remaining;  // track for fill calculation
    _prev_round_ts  = snap.timestamp_ms;
}

//==========================================================================
// Phase 3: EXECUTE
//==========================================================================
void CloseoutExecutor::handleExecuting(wtp::IUftStraCtx* ctx,
                                        const MarketSnapshot& snap)
{
    if (!_portfolio)
    {
        _phase = CloseoutSub::FAILED;
        return;
    }

    uint64_t now_ms = snap.timestamp_ms;

    // --- Inflight guard: wait for previous batch to settle before submitting next ---
    // In match_this_tick backtest mode, orders fill instantly but the fill
    // confirmation (on_order/on_trade) may arrive async. Without this guard,
    // the executor submits every tick → overfills → remaining flips sign →
    // submits opposite direction → infinite loop (1696 rounds, 11927 lots).
    //
    // FIX (PB-2): use OrderRouter's per-source active count instead of the
    // shared UnifiedOrderTracker. UnifiedOrderTracker only tracks MM quotes
    // (via FutuQuoter::trackMMOrder); closeout orders go through OrderRouter
    // which maintains its own per-source vector. The old code queried tracker
    // and always saw count=0 (closeout orders never landed there) → guard
    // was a no-op → every tick re-submitted → maker limit orders stacked on
    // the book → all filled at once when price reached them → blowup.
    if (_inflight_qty > 0)
    {
        // Count only this executor's own active orders (CLOSEOUT source).
        int active = _router
            ? static_cast<int>(_router->getActiveCountBySource(Source::CLOSEOUT))
            : 0;
        if (active > 0)
        {
            // Previous batch still inflight — wait
            return;
        }
        // Previous batch settled
        _inflight_qty = 0;
    }

    // --- Recompute remaining from fresh net delta ---
    double net_delta = _portfolio->getNetDelta();
    _remaining = -net_delta / _hedge_ratio;

    // --- Check completion ---
    if (std::abs(_remaining) < 0.5)
    {
        WTSLogger::info("CloseoutExecutor[{}] EXECUTE: flat (remaining={:.2f}), COMPLETED. "
                        "Filled {:.1f}/{:.1f} in {} rounds",
                        (void*)this, _remaining,
                        _total_filled, _total_to_hedge,
                        static_cast<uint32_t>(_rounds.size()));
        complete();
        return;
    }

    // --- Compute time left ---
    uint32_t time_left_ms = 0;
    if (_close_time_ms > now_ms)
        time_left_ms = static_cast<uint32_t>(_close_time_ms - now_ms);
    else
        time_left_ms = 0;

    // --- Force SWEEP if time is critical ---
    if (time_left_ms <= _cfg.sweep_threshold_ms && !_sweep_done)
    {
        WTSLogger::warn("CloseoutExecutor[{}] EXECUTE: {}ms left <= sweep_threshold({}), forcing SWEEP",
                        (void*)this, time_left_ms, _cfg.sweep_threshold_ms);
    }

    // --- Compute urgency ---
    double urgency = computeUrgency(now_ms);

    // --- Select price tier ---
    PriceTier tier = selectTier(urgency, time_left_ms);

    // --- Calculate batch quantity ---
    double batch_qty = calcBatchQty(_remaining, tier, snap.bid1_qty, snap.ask1_qty);

    if (batch_qty < 1.0)
    {
        // Can't place meaningful order at this depth — escalate
        if (tier < PriceTier::SWEEP)
        {
            tier = static_cast<PriceTier>(static_cast<int>(tier) + 1);
            batch_qty = calcBatchQty(_remaining, tier, snap.bid1_qty, snap.ask1_qty);
        }
        if (batch_qty < 1.0)
        {
            batch_qty = std::abs(_remaining); // last resort: send full remaining
        }
    }

    batch_qty = std::floor(batch_qty);  // round down to integer lots
    if (batch_qty < 1.0)
        batch_qty = 1.0;  // at least 1 lot

    // Don't exceed remaining
    batch_qty = std::min(batch_qty, std::abs(_remaining));

    // Absolute safety cap: prevent runaway batch in extreme conditions
    // (e.g. closeout starts with very large netDelta from upstream bugs)
    batch_qty = std::min(batch_qty, static_cast<double>(_cfg.max_batch_size));

    // --- Compute price ---
    bool is_buy = (_remaining > 0);  // positive remaining = need to buy
    double price = computePrice(tier, is_buy, snap.bid1, snap.ask1, snap.price_tick);

    if (price <= 0)
    {
        WTSLogger::error("CloseoutExecutor[{}] EXECUTE: invalid price {:.2f} (bid={:.2f} ask={:.2f})",
                         (void*)this, price, snap.bid1, snap.ask1);
        _phase = CloseoutSub::FAILED;
        return;
    }

    // --- Submit order ---
    WTSLogger::warn("CloseoutExecutor[{}] EXECUTE round {}: {} {:.0f} @ {:.2f} "
                    "(tier={}, urgency={:.3f}, remaining={:.1f}, time_left={}ms)",
                    (void*)this, static_cast<uint32_t>(_rounds.size()),
                    is_buy ? "BUY" : "SELL", batch_qty, price,
                    static_cast<int>(tier), urgency, _remaining, time_left_ms);

    submitHedgeOrder(ctx, is_buy, price, batch_qty);

    // --- Set inflight guard (wait for next batch until this settles) ---
    _inflight_qty = batch_qty;

    // --- Record round ---
    _prev_round_pos  = _remaining;
    _prev_round_ts   = now_ms;
    _prev_round_tier = tier;
    _prev_round_qty  = batch_qty;

    // --- Mark sweep done ---
    if (tier == PriceTier::SWEEP)
        _sweep_done = true;
}

//==========================================================================
// Core logic
//==========================================================================

double CloseoutExecutor::computeUrgency(uint64_t now_ms) const
{
    uint32_t time_left_ms = 0;
    if (_close_time_ms > now_ms)
        time_left_ms = static_cast<uint32_t>(_close_time_ms - now_ms);

    if (time_left_ms == 0)
        return 999.0; // extreme urgency

    double fill_rate = estimateFillRate();
    // Floor fill_rate to avoid urgency explosion when no fills yet
    // Assume at least 1 lot per 5 ticks (2500ms for 500ms tick)
    double fill_rate_floor = 1.0 / 2500.0;  // 0.0004 lots/ms
    if (fill_rate < fill_rate_floor)
        fill_rate = fill_rate_floor;

    double urgency = std::abs(_remaining) / (time_left_ms * fill_rate + 1e-6);

    // Clamp to sane range [0, 10]
    return std::min(urgency, 10.0);
}

PriceTier CloseoutExecutor::selectTier(double urgency, uint32_t time_left_ms) const
{
    // Force SWEEP if time is critical
    if (time_left_ms <= _cfg.sweep_threshold_ms)
        return PriceTier::SWEEP;

    if (urgency < 0.5)
        return PriceTier::PASSIVE;
    else if (urgency < 1.0)
        return PriceTier::MID_PASSIVE;
    else if (urgency < 1.5)
        return PriceTier::AGGRESSIVE;
    else if (urgency < 2.0)
        return PriceTier::VERY_AGGRESSIVE;
    else
        return PriceTier::SWEEP;
}

double CloseoutExecutor::calcBatchQty(double remaining, PriceTier tier,
                                       double bid1_qty, double ask1_qty) const
{
    double abs_remaining = std::abs(remaining);
    double depth = _cfg.depthRatio(tier);

    // SWEEP = unlimited depth
    if (tier == PriceTier::SWEEP)
    {
        return abs_remaining;
    }

    // Depth-aware limit: take fraction of best-side depth
    // If we need to buy, we face the ask side depth
    // If we need to sell, we face the bid side depth
    bool need_buy = (remaining > 0);
    double face_depth = need_buy ? ask1_qty : bid1_qty;
    double depth_limit = face_depth * depth;

    double batch = std::min(abs_remaining, depth_limit);
    return batch;
}

double CloseoutExecutor::computePrice(PriceTier tier, bool is_buy,
                                       double bid, double ask, double tick) const
{
    if (bid <= 0 || ask <= 0 || tick <= 0)
        return 0;

    double mid = (bid + ask) / 2.0;

    switch (tier)
    {
        case PriceTier::PASSIVE:
            return is_buy ? ask - tick : bid + tick;

        case PriceTier::MID_PASSIVE:
            return mid;

        case PriceTier::AGGRESSIVE:
            return is_buy ? ask : bid;

        case PriceTier::VERY_AGGRESSIVE:
            return is_buy ? ask + tick : bid - tick;

        case PriceTier::SWEEP:
            return is_buy ? ask + tick * _cfg.sweep_ticks
                          : bid - tick * _cfg.sweep_ticks;
    }
    return is_buy ? ask : bid;
}

double CloseoutExecutor::estimateFillRate() const
{
    if (_rounds.empty())
    {
        // Initial estimate: assume we can finish in 70% of available time
        uint64_t now_approx = _prev_round_ts;
        uint64_t planned_ms = 1;
        if (_close_time_ms > now_approx)
            planned_ms = (_close_time_ms - now_approx) * 7 / 10;

        return _total_to_hedge / (static_cast<double>(planned_ms) + 1e-6);
    }

    // Average of last 3 rounds
    double total_filled = 0;
    uint64_t total_time = 0;
    int n = std::min(3, static_cast<int>(_rounds.size()));
    for (int i = static_cast<int>(_rounds.size()) - n;
         i < static_cast<int>(_rounds.size()); i++)
    {
        total_filled += _rounds[i].filled_qty;
        total_time  += _rounds[i].elapsed_ms;
    }

    if (total_time == 0)
        return _total_to_hedge; // avoid div-by-zero: assume 1ms

    return total_filled / (static_cast<double>(total_time) + 1e-6);
}

void CloseoutExecutor::submitHedgeOrder(wtp::IUftStraCtx* ctx,
                                         bool is_buy, double price, double qty)
{
    int flag = _cfg.use_fak ? 1 : 0;

    // Closeout uses net-position API (stra_buy/stra_sell) instead of explicit
    // exit_long/exit_short. Reason: for CoverToday exchanges (INE/CFFEX),
    // exit_long(isToday=false) can only close yesterday's position. Since all
    // MM positions are opened intraday (today), the closeout would fail with
    // "no enough available old position" and loop 55000+ rounds.
    //
    // Net-position API auto-splits close-today/close-yesterday via ActionPolicy.
    // This is safe because CloseoutExecutor runs in CLOSEOUT phase where MM
    // quoting is halted — no concurrent fills, no risk of net-position API
    // opening new positions.
    if (!_router)
    {
        WTSLogger::error("CloseoutExecutor::submitHedgeOrder called with _router==nullptr; "
                         "setOrderRouter() must be invoked at strategy init. Skipping order.");
        return;
    }

    Source src = Source::CLOSEOUT;
    if (is_buy)
    {
        // BUY to close short → stra_buy (net position API)
        auto res = _router->submitBuy(ctx, _code, price, qty, src, flag);
        if (res.rate_limited)
            WTSLogger::warn("CloseoutExecutor: BUY rate limited");
        else if (res.self_trade_blocked)
            WTSLogger::warn("CloseoutExecutor: BUY self-trade blocked");
    }
    else
    {
        // SELL to close long → stra_sell (net position API)
        auto res = _router->submitSell(ctx, _code, price, qty, src, flag);
        if (res.rate_limited)
            WTSLogger::warn("CloseoutExecutor: SELL rate limited");
        else if (res.self_trade_blocked)
            WTSLogger::warn("CloseoutExecutor: SELL self-trade blocked");
    }
}

void CloseoutExecutor::updateRoundFill(const MarketSnapshot& snap)
{
    if (_prev_round_qty <= 0 || _prev_round_ts == 0)
        return;

    if (!_portfolio)
        return;

    double current_net_delta = _portfolio->getNetDelta();
    double current_remaining = -current_net_delta / _hedge_ratio;

    // Fill = how much remaining decreased (in absolute terms)
    double prev_abs = std::abs(_prev_round_pos);
    double curr_abs = std::abs(current_remaining);
    double filled = prev_abs - curr_abs;

    // Only record positive fills
    if (filled < 0)
        filled = 0;

    _total_filled += filled;

    uint64_t elapsed = 0;
    if (snap.timestamp_ms > _prev_round_ts)
        elapsed = snap.timestamp_ms - _prev_round_ts;

    ExecutionRound round;
    round.submit_ts     = _prev_round_ts;
    round.submitted_qty = _prev_round_qty;
    round.filled_qty    = filled;
    round.fill_ts       = snap.timestamp_ms;
    round.elapsed_ms    = elapsed;
    round.tier          = _prev_round_tier;
    _rounds.push_back(round);
}

void CloseoutExecutor::complete()
{
    _phase = CloseoutSub::COMPLETED;

    // Log summary
    uint64_t total_elapsed = 0;
    if (!_rounds.empty())
    {
        total_elapsed = _rounds.back().fill_ts - _start_ts;
    }

    WTSLogger::info("CloseoutExecutor[{}] COMPLETED: filled {:.1f}/{:.1f} lots "
                    "in {} rounds over {}ms",
                    (void*)this, _total_filled, _total_to_hedge,
                    static_cast<uint32_t>(_rounds.size()), total_elapsed);
}

} // namespace futu
