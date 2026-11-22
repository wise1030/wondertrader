/*!
 * \file UiSnapshotView.h
 * \brief UI bridge POD view types (worker -> IO thread handoff).
 *
 * Design: analysis/ui-migration-design-2026-11-16.md (v2.1). The worker
 * thread fills these plain structures (no IO, no locks held across calls);
 * the UiBridge IO thread serializes them to JSON Lines. All "has*" flags
 * carry the null/unknown semantics: a field the engine cannot source is
 * encoded as JSON null, never as an invented default.
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wt_option {

// ---------------------------------------------------------------------------
// Per-leg (one side of a strike) view
// ---------------------------------------------------------------------------
struct UiLegView
{
    std::string code;        // stdCode (OptionData::getCode) — identity for tickets/orders

    // Market book (0/0 means "no book" -> JSON null)
    double bid = 0, bidSz = 0;
    double ask = 0, askSz = 0;
    double last = 0;

    bool hasTheo = false;   double theo = 0;
    bool hasIv = false;     double iv = 0;
    bool hasDelta = false;  double delta = 0;
    bool hasVshift = false; double vshift = 0;   // theoVol - atmVol

    // P1 follow-up (五档深度): 5-level book from OptionMarket's B12 store.
    // Only encoded when the feed actually carries depth beyond level 1
    // (CTP options usually carry level 1 only — see snapshot size note).
    static constexpr int32_t DEPTH = 5;
    int32_t numBid = 0, numAsk = 0;
    double bidPx[DEPTH] = {}, askPx[DEPTH] = {};
    double bidSz5[DEPTH] = {}, askSz5[DEPTH] = {};

    bool hasPnl = false;    double pnl = 0;

    double pos = 0;                          // net position (offsets applied)
    bool hasQmode = false; int32_t qmode = 0;

    // P1 (2026-11-19) batch-2: our resting quote + queue position, ported
    // from quantbox optiontrader bqpos/aqpos semantics:
    //   qp == 0   our price is at the market best (or better)
    //   qp > 0    ticks behind the market best (price-gap approximation)
    //   qp < 0    crossing the spread (our bid >= market ask / vice versa)
    //   9999      no resting quote on this side
    bool hasOb = false;   double obid = 0, oask = 0;
    int32_t bqp = 9999, aqp = 9999;
};

struct UiStrikeView
{
    double k = 0;
    UiLegView C, P;
};

// ---------------------------------------------------------------------------
// Per-expiry view
// ---------------------------------------------------------------------------
struct UiExpiryView
{
    uint32_t expiry = 0;

    // P1 (2026-11-18): quoting enable from the expiry config — lets the UI
    // grey out disabled series instead of showing them green.
    bool quoting = false;

    double fwd = 0;
    bool fwdReady = false;
    bool fitReady = false;
    bool underlyingValid = false;

    bool hasAtmVol = false;     double atmVol = 0;
    bool hasAtmVolSprd = false; double atmVolSprd = 0;
    int32_t daysToExpiry = 0;

    // Fit-gate diagnostics — same metric as the FITGATE log line
    // (HftOptionStrategy.cpp): legs with getBid() > 0.
    int32_t booksWithBid = 0, booksTotal = 0;

    bool hasGreeks = false; double greeksDelta = 0, greeksVega = 0;
    // P1 batch-5.2: full expiry greeks for the hedge panel + greek targets.
    // Gamma/theta ride the same hasGreeks flag (expGreeks is all-or-nothing).
    double greeksGamma = 0, greeksTheta = 0;
    bool hasTargDelta = false; double targDelta = 0;
    bool hasTargVega  = false; double targVega  = 0;

    bool hasUnd = false;
    std::string undCode;
    double undBid = 0, undAsk = 0;
    double undBidSz = 0, undAskSz = 0;   // P1 UI: market depth
    double undPos = 0;
    // Audit 11-22 (round 2): hedge-leg position delta in UNDERLYING units
    // (pos lots x contract size) — the same quantity the per-expiry hedge
    // gate (OptionHedgeCoordinator::checkMulti via OptionExpiryGreeks::
    // totalDelta) compares against hedgeDeltaThreshold.  Lets the BOARD
    // red rule match the engine exactly instead of option-delta-only.
    double undDelta = 0;
    // P1 UI: futures-leg market AND our market-making orders — the option
    // legs show both in the Grid (ordBid/ordAsk); the futures leg had
    // neither in the UI after UnderlierView was folded into the BOARD.
    bool hasUndOur = false;
    double undOurBid = 0, undOurAsk = 0, undOurBidSz = 0, undOurAskSz = 0;

    std::vector<UiStrikeView> strikes;
};

// ---------------------------------------------------------------------------
// Orders / fills
// ---------------------------------------------------------------------------
struct UiOrderView
{
    uint32_t id = 0;
    std::string code;
    bool buy = false;
    double px = 0;
    uint32_t qty = 0, filled = 0;
    double t = 0;            // strategy-clock issue seconds
    uint8_t tag = 0;         // P1 batch-4.3: OrderTag (0=Unknown,1=MM,2=Hedge,3=Scanner)
};

struct UiFillView
{
    std::string code;
    bool buy = false;
    double px = 0;
    uint32_t qty = 0;
    double t = 0;            // strategy-clock (sim) seconds — replay-safe
    uint8_t tag = 0;         // P1 batch-4.3: OrderTag of the parent order

    // P1 batch-6.3: issue-time pricing context for the attribution review.
    // Only present when fillAttribution capture is on (rec.ctx.valid).
    bool hasCtx = false;
    double theo = 0, midAtIssue = 0, iv = 0;
    double delta = 0, gamma = 0, vega = 0, theta = 0;
};

// ---------------------------------------------------------------------------
// Full snapshot (latest-wins single slot)
// ---------------------------------------------------------------------------
struct UiSnapshotView
{
    uint64_t seq = 0;            // worker _batchCount at build time
    uint64_t tsWallMs = 0;       // wall clock at build (age computed downstream)

    // TradingState
    std::string stateType;       // toString(TradingStateType)
    std::string stateReason;
    bool channelReady = false;
    bool underlyingReady = false;
    bool enabled = false;
    bool panicked = false;
    uint32_t tdate = 0;

    // Portfolio greeks (OptionRisk::getPositionGreeks — OPTION positions
    // only; the futures-hedge delta lives in pfDelta below)
    bool hasGreeks = false;
    double gDelta = 0, gGamma = 0, gVega = 0, gVegaTW = 0, gTheta = 0;
    // Audit 11-22 (round 2): OptionRisk::getPortfolioDelta() = option delta
    // + m_pfuDelta (futures-hedge delta).  This is the quantity the delta
    // hedger actually compares with hedgeDeltaThreshold — the BOARD TOTAL
    // row must use it, not gDelta, or a hedged-flat book still shows red.
    double pfDelta = 0;

    // PnL (sum of per-contract PnlTrackers, mirrors the compute loop)
    bool hasPnl = false;
    double pnlCur = 0, pnlRealized = 0, pnlTurnover = 0;

    // Risk / limits
    bool limitsBreached = false;
    int32_t tps = 0, tpsMax = 0;
    // P1 batch-5.1: delta-hedge fire threshold — the hedge panel colours a
    // row red when |$Δ| exceeds it while Γ≠0 (a gamma-active expiry that the
    // hedger cannot neutralise passively).
    double hedgeDeltaThreshold = 0;
    uint32_t ordersSent = 0, fills = 0, cancels = 0, rejects = 0;
    double cancelRatio = 0, fillRatio = 0;   // global: totals-derived
    // P1 batch-4.3: per-source (OrderTag) buckets — 0=Unknown 1=MM 2=Hedge 3=Scanner
    uint32_t liveByTag[4] = {0, 0, 0, 0};
    uint32_t fillsByTag[4] = {0, 0, 0, 0};
    bool hasSessionStop = false;
    int32_t sessionStopInSec = 0;

    // P1 batch-6.4: prediction accuracy (forward 30s + scanner convergence)
    uint32_t predResolved = 0, scanResolved = 0;
    double predHitRate = 0, predMae = 0, scanHitRate = 0, scanIc = 0;

    // Health
    double batchP50 = 0, batchP95 = 0, batchP99 = 0;   // microseconds
    uint64_t aepEvents = 0, queueDrops = 0;
    uint64_t mdOverflow = 0, traderDepth = 0;
    uint64_t eventDrops = 0, snapshotCoalesced = 0;

    std::vector<UiExpiryView> expiries;
    std::vector<UiOrderView> orders;                      // live orders only
    std::vector<std::pair<std::string, std::pair<double, double>>> contracts; // code -> {mult, tick}

    // Hot-param echo (P2 SETPARAM whitelist, hotVal semantics: sync_param ptr
    // when share_domain is active, member otherwise) — the UI shows these as
    // the current values before an edit.
    bool hasParams = false;
    double pWgtVegaflow = 0, pWgtFrontfutSkew = 0, pWgtDeltaflow = 0;
    double pWgtAtmsig = 0, pWgtRollema = 0;
    // Audit 11-22 (round 3): GVV blend weight echo (SETPARAM whitelist)
    double pVolcurveWeight = 0;
    int32_t pMaxTps = 0;
};

// ---------------------------------------------------------------------------
// Event frames (bounded FIFO; order/trade/reject hints + command acks)
// ---------------------------------------------------------------------------
enum class UiEventType : uint8_t
{
    Order = 0,
    Trade = 1,
    Reject = 2,
    Ack = 3,
    Alert = 4,   // P1 batch-5.5: graded alert (order anomaly / panic / risk)
};

struct UiEventFrame
{
    UiEventType type = UiEventType::Order;
    uint64_t seq = 0;         // worker _batchCount at push time
    uint64_t tsWallMs = 0;

    // order/trade/reject payload
    uint32_t id = 0;
    uint8_t tag = 0;          // P1 batch-4.3: OrderTag of the parent order
    std::string code;
    bool buy = false;
    double qty = 0, px = 0;
    double left = 0;          // order: remaining qty
    bool canceled = false;

    // ack payload
    uint64_t cmdSeq = 0;
    bool ok = false;
    std::string err;

    // P1 batch-5.5: alert payload.  level 0=info 1=warn 2=error; msg rides in
    // the dedicated field (code stays empty for alerts).
    uint8_t level = 0;
    std::string msg;
};

} // namespace wt_option
