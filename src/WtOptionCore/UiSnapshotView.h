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

    bool hasUnd = false;
    std::string undCode;
    double undBid = 0, undAsk = 0;
    double undPos = 0;

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

    // Portfolio greeks (OptionRisk::getPositionGreeks)
    bool hasGreeks = false;
    double gDelta = 0, gGamma = 0, gVega = 0, gVegaTW = 0, gTheta = 0;

    // PnL (sum of per-contract PnlTrackers, mirrors the compute loop)
    bool hasPnl = false;
    double pnlCur = 0, pnlRealized = 0, pnlTurnover = 0;

    // Risk / limits
    bool limitsBreached = false;
    int32_t tps = 0, tpsMax = 0;
    uint32_t ordersSent = 0, fills = 0, cancels = 0, rejects = 0;
    double cancelRatio = 0, fillRatio = 0;   // global: totals-derived
    // P1 batch-4.3: per-source (OrderTag) buckets — 0=Unknown 1=MM 2=Hedge 3=Scanner
    uint32_t liveByTag[4] = {0, 0, 0, 0};
    uint32_t fillsByTag[4] = {0, 0, 0, 0};
    bool hasSessionStop = false;
    int32_t sessionStopInSec = 0;

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
};

} // namespace wt_option
