/*!
 * \file CloseoutExecutor.h
 * \brief Urgency-driven progressive closeout hedging executor
 *
 * Replaces the single-shot executeCloseoutHedge with a multi-round
 * urgency-driven execution strategy:
 *   Phase 1: DRAIN    — wait for inflight orders to settle
 *   Phase 2: ASSESS   — read net delta, compute remaining
 *   Phase 3: EXECUTE  — iterative FAK batches with escalating price tiers
 *   Phase 4: VERIFY   — confirm flat or FAILED → retry
 *
 * Design doc: docs/CLOSEOUT_EXECUTOR_DESIGN.md
 *
 * Thread safety: NOT thread-safe. Main thread only (same as FutuQuoter/OrderRouter).
 *
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include "../Includes/FasterDefs.h"
#include "OrderRouter.h"

NS_WTP_BEGIN
class IUftStraCtx;
NS_WTP_END

namespace futu {

class UnifiedOrderTracker;
class FutuPortfolio;
struct ContractState;

/// Closeout execution phase
enum class CloseoutPhase : uint8_t
{
    IDLE        = 0,
    DRAINING    = 1,   ///< Waiting for inflight orders to settle
    ASSESSING   = 2,   ///< Reading net delta, computing remaining
    EXECUTING   = 3,   ///< Iterative FAK batches
    COMPLETED   = 4,
    FAILED      = 5,
};

/// Price escalation tier for urgency-driven execution
enum class PriceTier : uint8_t
{
    PASSIVE         = 0,  ///< Same side +1 tick
    MID_PASSIVE     = 1,  ///< Mid price
    AGGRESSIVE      = 2,  ///< Opposite price
    VERY_AGGRESSIVE = 3,  ///< Opposite +1 tick
    SWEEP           = 4,  ///< Opposite +N ticks
};

/// Single execution round record (for fill-rate estimation)
struct ExecutionRound
{
    uint64_t  submit_ts     = 0;
    double    submitted_qty = 0;
    double    filled_qty    = 0;
    uint64_t  fill_ts       = 0;
    uint64_t  elapsed_ms    = 0;
    PriceTier tier          = PriceTier::PASSIVE;
};

/// Market snapshot passed each tick (decoupled from WTS types)
struct MarketSnapshot
{
    double    bid1         = 0;
    double    ask1         = 0;
    double    bid1_qty     = 0;
    double    ask1_qty     = 0;
    double    price_tick   = 0;
    uint64_t  timestamp_ms = 0;
};

/// CloseoutExecutor configuration
struct CloseoutExecConfig
{
    uint32_t drain_timeout_ms     = 3000;  ///< Phase 1 drain timeout
    double   depth_ratio_passive  = 0.3;   ///< Passive tier: max depth fraction
    double   depth_ratio_mid      = 0.5;   ///< Mid tier: max depth fraction
    double   depth_ratio_aggr     = 0.8;   ///< Aggressive tier: max depth fraction
    uint32_t sweep_threshold_ms   = 5000;  ///< Time left (ms) to enter SWEEP
    uint32_t sweep_ticks          = 3;     ///< SWEEP: opponent price + N ticks
    bool     use_fak              = true;  ///< Use FAK for all orders

    /// Get depth ratio for a tier
    double depthRatio(PriceTier tier) const
    {
        switch (tier)
        {
            case PriceTier::PASSIVE:         return depth_ratio_passive;
            case PriceTier::MID_PASSIVE:     return depth_ratio_mid;
            case PriceTier::AGGRESSIVE:
            case PriceTier::VERY_AGGRESSIVE: return depth_ratio_aggr;
            case PriceTier::SWEEP:           return 1.0;
        }
        return depth_ratio_passive;
    }
};

/// Urgency-driven progressive closeout hedging executor
///
/// Usage:
///   1. start() — begin a closeout sequence
///   2. run()   — call every on_tick until getPhase()==COMPLETED or FAILED
///   3. reset() — call on session boundary
///
class CloseoutExecutor
{
public:
    CloseoutExecutor();
    ~CloseoutExecutor() = default;

    //==========================================================================
    // Dependency injection (setter style, consistent with StrategyCoordinator)
    //==========================================================================
    void setOrderRouter(OrderRouter* router) { _router = router; }
    void setOrderTracker(UnifiedOrderTracker* tracker) { _tracker = tracker; }
    void setPortfolio(FutuPortfolio* portfolio) { _portfolio = portfolio; }
    void setConfig(const CloseoutExecConfig& cfg) { _cfg = cfg; }

    //==========================================================================
    // Lifecycle
    //==========================================================================

    /// Begin a closeout sequence
    /// @param ctx           Strategy context
    /// @param code          Anchor contract code
    /// @param close_time_ms Session close timestamp (ms)
    /// @param hedge_ratio   Hedge ratio of anchor contract
    void start(wtp::IUftStraCtx* ctx,
               const char* code,
               uint64_t close_time_ms,
               double hedge_ratio);

    /// Advance the state machine. Call every on_tick.
    /// @param ctx  Strategy context
    /// @param snap Current market snapshot for the anchor contract
    void run(wtp::IUftStraCtx* ctx, const MarketSnapshot& snap);

    /// Reset to IDLE (call on session boundary)
    void reset();

    //==========================================================================
    // State queries
    //==========================================================================
    CloseoutPhase getPhase() const { return _phase; }
    bool isIdle() const      { return _phase == CloseoutPhase::IDLE; }
    bool isCompleted() const { return _phase == CloseoutPhase::COMPLETED; }
    bool isFailed() const    { return _phase == CloseoutPhase::FAILED; }
    bool isActive() const    { return _phase != CloseoutPhase::IDLE
                                    && _phase != CloseoutPhase::COMPLETED; }

    double getRemaining() const    { return _remaining; }
    double getTotalFilled() const  { return _total_filled; }
    uint32_t getRoundCount() const { return static_cast<uint32_t>(_rounds.size()); }

private:
    //==========================================================================
    // Phase handlers
    //==========================================================================

    /// Phase 1: Check if drain is complete
    /// @return true if drain complete (proceed to next phase this tick)
    bool handleDraining(wtp::IUftStraCtx* ctx, const MarketSnapshot& snap);

    /// Phase 2: Assess net delta and set remaining
    void handleAssessing(wtp::IUftStraCtx* ctx, const MarketSnapshot& snap);

    /// Phase 3: Execute one round of hedging
    void handleExecuting(wtp::IUftStraCtx* ctx, const MarketSnapshot& snap);

    //==========================================================================
    // Core logic
    //==========================================================================

    /// Compute urgency = |remaining| / (time_left_ms * fill_rate)
    double computeUrgency(uint64_t now_ms) const;

    /// Select price tier based on urgency and time remaining
    PriceTier selectTier(double urgency, uint32_t time_left_ms) const;

    /// Calculate batch quantity for this round (depth-aware)
    double calcBatchQty(double remaining, PriceTier tier,
                        double bid1_qty, double ask1_qty) const;

    /// Compute execution price for a tier
    double computePrice(PriceTier tier, bool is_buy,
                        double bid, double ask, double tick) const;

    /// Estimate fill rate (lots/ms) from recent rounds
    double estimateFillRate() const;

    /// Submit a hedge order (FAK)
    void submitHedgeOrder(wtp::IUftStraCtx* ctx,
                          bool is_buy, double price, double qty);

    /// Record the previous round's fill (called at start of each round)
    void updateRoundFill(const MarketSnapshot& snap);

    /// Mark completed
    void complete();

    //==========================================================================
    // Dependencies
    //==========================================================================
    OrderRouter*           _router    = nullptr;
    UnifiedOrderTracker*   _tracker   = nullptr;
    FutuPortfolio*         _portfolio = nullptr;
    CloseoutExecConfig     _cfg;

    //==========================================================================
    // State
    //==========================================================================
    CloseoutPhase  _phase          = CloseoutPhase::IDLE;
    char           _code[32]       = {};
    uint64_t       _close_time_ms  = 0;
    double         _hedge_ratio    = 1.0;

    double         _remaining      = 0;      ///< Lots left to hedge (+need buy, -need sell)
    double         _total_to_hedge = 0;
    double         _total_filled   = 0;

    uint64_t       _start_ts       = 0;
    uint64_t       _drain_start_ts = 0;
    bool           _sweep_done     = false;

    // Previous round info (for fill tracking)
    double         _prev_round_pos = 0;      ///< Portfolio position before last order
    uint64_t       _prev_round_ts  = 0;
    PriceTier      _prev_round_tier = PriceTier::PASSIVE;
    double         _prev_round_qty  = 0;

    // Execution history for fill-rate estimation
    std::vector<ExecutionRound> _rounds;
};

} // namespace futu
