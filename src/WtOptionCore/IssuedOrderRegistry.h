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
 *  - records stay alive until reset()/TTL purge so LATE fills can be
 *    classified (OrderAnomalyGuard behavior);
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
    };

    struct Rec {
        std::string code;
        uint32_t totalQty = 0;
        uint32_t filledQty = 0;
        bool done = false;
        bool rejected = false;      // entrust failure (no terminal on_order)
        bool isBuy = false;
        double issuePx = 0;
        double issuedTime = 0;      // seconds (strategy clock)
        double doneTime = 0;
        IssueContext ctx;
        std::vector<Fill> fills;
    };

    enum class FillClass { Normal, LateAfterDone, Unknown, Overfill };

    void setGetTimeFn(std::function<double()> fn) { m_getTime = std::move(fn); }
    void setGetTimeUsFn(std::function<uint64_t()> fn) { m_getTimeUs = std::move(fn); }
    void setAlertCallback(AlertFn cb) { m_alert = std::move(cb); }
    /// Current microsecond clock (attribution snapshot timestamps).
    uint64_t nowUs() const { return m_getTimeUs ? m_getTimeUs() : 0; }

    /// Registration (called by executors right after a successful send).
    /// Returns the record so the caller can attach the IssueContext snapshot.
    Rec& onIssued(uint32_t localid, const std::string& code, uint32_t qty) {
        if (localid == 0) {
            static Rec s_dummy;
            s_dummy = Rec{};
            return s_dummy;
        }
        auto& rec = m_orders[localid];
        rec.code = code;
        rec.totalQty = qty;
        rec.filledQty = 0;
        rec.done = false;
        rec.rejected = false;
        rec.issuedTime = nowSec();
        rec.ctx = IssueContext{};
        rec.fills.clear();
        return rec;
    }

    /// Price/side supplement — quote APIs report two ids for one send, so the
    /// executor passes each id's own price right after onIssued.
    void onOrderSent(uint32_t localid, double price, bool isBuy) {
        auto it = m_orders.find(localid);
        if (it != m_orders.end()) {
            it->second.issuePx = price;
            it->second.isBuy = isBuy;
        }
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
            raise("order-not-found fill: localid=" + std::to_string(localid)
                  + " count=" + std::to_string(m_unknownFills));
            if (m_unknownFills == 3 || m_unknownFills % 100 == 0)
                escalate();
            return FillClass::Unknown;
        }
        Rec& rec = it->second;
        rec.fills.push_back({px, vol, nowUs(), spot});
        if (rec.done && !rec.rejected) {
            // Late fill after a cancel/done report.  Rejected orders never
            // traded, so a fill there is genuinely anomalous — count it as
            // unknown rather than silently absorbing it.
            m_lateAfterDone++;
            return FillClass::LateAfterDone;
        }
        if (rec.rejected) {
            m_unknownFills++;
            raise("fill for REJECTED order: " + rec.code + " localid=" +
                  std::to_string(localid));
            return FillClass::Unknown;
        }
        rec.filledQty += vol;
        if (rec.totalQty > 0 && rec.filledQty > rec.totalQty) {
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

    size_t unknownFillCount() const { return m_unknownFills; }
    size_t lateAfterDoneCount() const { return m_lateAfterDone; }
    size_t overfillCount() const { return m_overfills; }

    /// Bound memory by dropping the OLDEST done records once the map exceeds
    /// maxRecords (0 = keep until reset).  Size-bound rather than time-bound:
    /// a simulated-clock TTL erratically starves backtest attribution exports
    /// during fast replay, while a size cap behaves identically in live and
    /// backtest.  Live records are never dropped.
    void purgeDone(size_t maxRecords) {
        if (maxRecords == 0 || m_orders.size() <= maxRecords) return;
        std::vector<std::pair<double, uint32_t>> doneRecs;
        doneRecs.reserve(m_orders.size());
        for (const auto& [lid, r] : m_orders)
            if (r.done) doneRecs.emplace_back(r.doneTime, lid);
        std::sort(doneRecs.begin(), doneRecs.end());
        size_t need = m_orders.size() - maxRecords;
        for (size_t i = 0; i < doneRecs.size() && need > 0; ++i, --need)
            m_orders.erase(doneRecs[i].second);
    }

    void reset() {
        m_orders.clear();
        m_unknownFills = m_lateAfterDone = m_overfills = 0;
    }

private:
    double nowSec() const { return m_getTime ? m_getTime() : 0.0; }
    void raise(const std::string& msg) { if (m_alert) m_alert(msg); }
    void escalate() {
        raise("ORDER ANOMALY ESCALATION: " + std::to_string(m_unknownFills) +
              " unknown fills — recommend StopQuoting review");
    }

    std::unordered_map<uint32_t, Rec> m_orders;
    size_t m_unknownFills = 0;
    size_t m_lateAfterDone = 0;
    size_t m_overfills = 0;
    std::function<double()> m_getTime;
    std::function<uint64_t()> m_getTimeUs;
    AlertFn m_alert;
};

} // namespace wt_option
