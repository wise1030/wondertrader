#pragma once
/*!
 * \file IssuedOrderRegistry.h
 * \brief Single per-order registry — merger of OrderAnomalyGuard::Rec and
 *        FillPriceChecker::IssueInfo (plan 3.3), extended with the optional
 *        issue-context snapshot and per-fill records for fill-quality
 *        attribution (plan 3.6).
 *
 * Previously the strategy maintained THREE parallel per-order tables
 * (OQM::OrderState, OrderAnomalyGuard::Rec, FillPriceChecker::IssueInfo),
 * each fed from the same on_trade/on_order callbacks — a change-one-miss-one
 * hazard.  This registry is the single bookkeeping point; OrderAnomalyGuard
 * and FillPriceChecker become policy consumers of it.
 *
 * Lifecycle semantics (preserved from the merged classes):
 *  - records stay alive until reset() or the grace-window purge so LATE
 *    fills can be classified (OrderAnomalyGuard behavior); the purge is
 *    size-bound AND age-bound — a record younger than the grace window is
 *    never dropped, so late fills arriving after a session boundary keep
 *    their attribution instead of degrading to "unknown";
 *  - deviation checking only applies to fills arriving while the order is
 *    live (FillPriceChecker erased on done — reproduced via the `done` flag).
 *
 * Single-threaded: all calls happen on the async worker thread.
 */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <algorithm>
#include <utility>
#include <limits>
#include <cmath>

namespace wt_option {

class IssuedOrderRegistry {
public:
    using AlertFn = std::function<void(const std::string& msg)>;

    /// Optional snapshot of the pricing context at issue time (plan 3.6).
    /// Populated only when fillAttribution.enable is on; NaN = not captured.
    struct IssueContext {
        double theo    = std::numeric_limits<double>::quiet_NaN();
        double mid     = std::numeric_limits<double>::quiet_NaN();
        double iv      = std::numeric_limits<double>::quiet_NaN();
        double delta   = std::numeric_limits<double>::quiet_NaN();
        double gamma   = std::numeric_limits<double>::quiet_NaN();
        double vega    = std::numeric_limits<double>::quiet_NaN();
        double theta   = std::numeric_limits<double>::quiet_NaN();
        double spot    = std::numeric_limits<double>::quiet_NaN();
        double fwd     = std::numeric_limits<double>::quiet_NaN();
        uint64_t issueTimeUs = 0;
        bool valid = false;
    };

    struct Fill {
        double px = 0;
        uint32_t qty = 0;
        uint64_t timeUs = 0;
        double spot = std::numeric_limits<double>::quiet_NaN();  // per-expiry underlying at fill
        double timeSim = 0;     // fill time, strategy (simulated) clock seconds
        bool   late = false;    // arrived after a done/cancel report
    };

    struct Rec {
        std::string code;
        uint32_t totalQty = 0;
        uint32_t filledQty = 0;   // live-order fills only (excludes late/rejected)
        uint32_t fillQty = 0;     // ALL recorded fills (incl. late/rejected)
        bool done = false;
        bool rejected = false;      // entrust failure (no terminal on_order)
        bool isBuy = false;
        bool qtySplitUnknown = false;  // WT split one order into several ids;
                                       // totalQty is the WHOLE-order qty then
        double issuePx = 0;
        double issuedTime = 0;      // seconds (strategy clock)
        double issueTimeSim = 0;    // same clock, captured at registration
        double doneTime = 0;
        uint32_t sessionDate = 0;   // owning session (attribution grouping)
        size_t exportedFills = 0;   // fills already written to the CSV
        IssueContext ctx;
        std::vector<Fill> fills;
    };

    /// One row of fill_anomaly.csv — exchange reports we could not attribute
    /// to a live record (truly unknown localid, or fill on a rejected order).
    struct UnknownEvent {
        std::string code;       // empty when the localid was never issued
        uint32_t count = 0;
        double firstTime = 0;   // strategy clock seconds
        double lastTime = 0;
    };

    enum class FillClass { Normal, LateAfterDone, Unknown, Overfill };

    void setGetTimeFn(std::function<double()> fn) { m_getTime = std::move(fn); }
    void setGetTimeUsFn(std::function<uint64_t()> fn) { m_getTimeUs = std::move(fn); }
    void setAlertCallback(AlertFn cb) { m_alert = std::move(cb); }
    /// Current microsecond clock (attribution snapshot timestamps).
    uint64_t nowUs() const { return m_getTimeUs ? m_getTimeUs() : 0; }

    /// Registration (test/legacy path): (re)initialize the record wholesale.
    /// Production callers use ensureIssued() so a duplicate registration
    /// never wipes fills/ctx accumulated by an earlier one.
    Rec& onIssued(uint32_t localid, const std::string& code, uint32_t qty) {
        if (localid == 0) {
            static Rec s_dummy;
            s_dummy = Rec{};
            return s_dummy;
        }
        auto& rec = m_orders[localid];
        rec = Rec{};
        rec.code = code;
        rec.totalQty = qty;
        rec.issuedTime = nowSec();
        rec.issueTimeSim = rec.issuedTime;
        return rec;
    }

    /// Idempotent registration (production path, plan 3.3 convergence fix):
    /// initializes a MISSING record; for an existing one only backfills
    /// fields that are still empty — never clears fills/ctx/filledQty/done.
    Rec& ensureIssued(uint32_t localid, const std::string& code, uint32_t qty,
                      uint32_t sessionDate = 0) {
        if (localid == 0) {
            static Rec s_dummy;
            s_dummy = Rec{};
            return s_dummy;
        }
        auto it = m_orders.find(localid);
        if (it != m_orders.end()) {
            Rec& r = it->second;
            if (r.code.empty()) r.code = code;
            if (r.totalQty == 0 && !r.qtySplitUnknown) r.totalQty = qty;
            if (r.sessionDate == 0) r.sessionDate = sessionDate;
            return r;
        }
        auto& rec = m_orders[localid];
        rec.code = code;
        rec.totalQty = qty;
        rec.sessionDate = sessionDate;
        rec.issuedTime = nowSec();
        rec.issueTimeSim = rec.issuedTime;
        return rec;
    }

    /// Terminal order status. Returns true when the order became done.
    bool onOrderDone(uint32_t localid, bool isCanceled, double leftQty) {
        auto it = m_orders.find(localid);
        if (it == m_orders.end()) return false;
        if (leftQty <= 0 || isCanceled) {
            it->second.done = true;
            it->second.doneTime = nowSec();
            return true;
        }
        return false;
    }

    /// Entrust rejection — WT sends no terminal on_order afterwards.
    void onOrderRejected(uint32_t localid) {
        auto it = m_orders.find(localid);
        if (it != m_orders.end()) {
            it->second.done = true;
            it->second.rejected = true;
            it->second.doneTime = nowSec();
        }
    }

    /// Classify and account a fill; records it for attribution export.
    /// `spot` (may be NaN) is the per-expiry underlying price at fill time.
    FillClass onFill(uint32_t localid, uint32_t vol, double px,
                     double spot = std::numeric_limits<double>::quiet_NaN()) {
        auto it = m_orders.find(localid);
        if (it == m_orders.end()) {
            m_unknownFills++;
            recordUnknown(localid, "", nowSec());
            raise("order-not-found fill: localid=" + std::to_string(localid)
                  + " count=" + std::to_string(m_unknownFills));
            if (m_unknownFills == 3 || m_unknownFills % 100 == 0)
                escalate();
            return FillClass::Unknown;
        }
        Rec& rec = it->second;
        const bool late = rec.done && !rec.rejected;
        rec.fills.push_back({px, vol, nowUs(), spot, nowSec(), late});
        rec.fillQty += vol;
        if (late) {
            // Late fill after a cancel/done report.  Rejected orders never
            // traded, so a fill there is genuinely anomalous — count it as
            // unknown rather than silently absorbing it.
            m_lateAfterDone++;
            return FillClass::LateAfterDone;
        }
        if (rec.rejected) {
            m_unknownFills++;
            recordUnknown(localid, rec.code, nowSec());
            raise("fill for REJECTED order: " + rec.code + " localid=" +
                  std::to_string(localid));
            return FillClass::Unknown;
        }
        rec.filledQty += vol;
        // qtySplitUnknown: totalQty is the whole-order qty shared by every
        // split id — per-id overfill comparison would be meaningless.
        if (rec.totalQty > 0 && !rec.qtySplitUnknown && rec.filledQty > rec.totalQty) {
            m_overfills++;
            raise("OVERFILL: " + rec.code + " filled=" +
                  std::to_string(rec.filledQty) + " > total=" +
                  std::to_string(rec.totalQty));
            return FillClass::Overfill;
        }
        return FillClass::Normal;
    }

    // ---- queries ------------------------------------------------------------
    const Rec* get(uint32_t localid) const {
        auto it = m_orders.find(localid);
        return it == m_orders.end() ? nullptr : &it->second;
    }
    const std::unordered_map<uint32_t, Rec>& orders() const { return m_orders; }
    std::unordered_map<uint32_t, Rec>& orders() { return m_orders; }

    size_t unknownFillCount() const { return m_unknownFills; }
    size_t lateAfterDoneCount() const { return m_lateAfterDone; }
    size_t overfillCount() const { return m_overfills; }

    /// Unattributable exchange reports (fill_anomaly.csv source).
    const std::unordered_map<uint32_t, UnknownEvent>& unknownEvents() const { return m_unknownEvents; }
    void clearUnknownEvents() { m_unknownEvents.clear(); }

    /// Bound memory by dropping the OLDEST done records.  A record is
    /// eligible only when it is done AND older than minDoneAgeSec (strategy
    /// clock) — the age floor keeps the late-fill attribution window open
    /// across session boundaries.  maxRecords == 0 disables the size bound
    /// (time-based purge only).  Live records are never dropped; when live
    /// records alone exceed maxRecords the map may stay above the cap.
    void purgeDone(size_t maxRecords, double minDoneAgeSec = 0.0) {
        const double now = nowSec();
        std::vector<std::pair<double, uint32_t>> doneRecs;
        doneRecs.reserve(m_orders.size());
        for (const auto& [lid, r] : m_orders) {
            if (!r.done) continue;
            if (now - r.doneTime < minDoneAgeSec) continue;  // still in grace window
            doneRecs.emplace_back(r.doneTime, lid);
        }
        if (doneRecs.empty()) return;
        std::sort(doneRecs.begin(), doneRecs.end());
        size_t need = maxRecords == 0 ? doneRecs.size()
                                      : (m_orders.size() > maxRecords ? m_orders.size() - maxRecords : 0);
        for (size_t i = 0; i < doneRecs.size() && need > 0; ++i, --need)
            m_orders.erase(doneRecs[i].second);
    }

    /// Session-boundary bookkeeping: clear per-session counters (and the
    /// archived unknown events) while KEEPING order records for the
    /// grace-window purge above.
    void resetCounters() {
        m_unknownFills = m_lateAfterDone = m_overfills = 0;
        m_unknownEvents.clear();
    }

    void reset() {
        m_orders.clear();
        resetCounters();
    }

private:
    double nowSec() const { return m_getTime ? m_getTime() : 0.0; }
    void recordUnknown(uint32_t localid, const std::string& code, double t) {
        auto& ev = m_unknownEvents[localid];
        if (ev.count == 0) {
            ev.code = code;
            ev.firstTime = t;
        }
        ev.count++;
        ev.lastTime = t;
    }
    void raise(const std::string& msg) { if (m_alert) m_alert(msg); }
    void escalate() {
        raise("ORDER ANOMALY ESCALATION: " + std::to_string(m_unknownFills) +
              " unknown fills — recommend StopQuoting review");
    }

    std::unordered_map<uint32_t, Rec> m_orders;
    std::unordered_map<uint32_t, UnknownEvent> m_unknownEvents;
    size_t m_unknownFills = 0;
    size_t m_lateAfterDone = 0;
    size_t m_overfills = 0;
    std::function<double()> m_getTime;
    std::function<uint64_t()> m_getTimeUs;
    AlertFn m_alert;
};

} // namespace wt_option
