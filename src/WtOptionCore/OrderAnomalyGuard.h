/*!
 * \file OrderAnomalyGuard.h
 * \brief Exchange-report anomaly protection (absorbed from quantbox IssuedOrderTracker)
 *
 * Three protections ported from quantbox/quantbox/trading/IssuedOrderTracker,
 * downsized per D2 decision (no central registry — WT localid management stays
 * in the framework; we only add the race-condition defenses):
 *
 *  1. DONE-before-FILL: cancel/done report arrives before fills. Fills for such
 *     orders are cached here so the strategy can still route them when they
 *     arrive late (TTL-bounded).
 *  2. order-not-found fill: a fill references a localid this process never
 *     issued. Counted + ERROR-logged (escalates to alert after repeated hits).
 *     Does NOT auto-panic to avoid killing quoting on one bad report.
 *  3. overfill: cumulative fills exceed reported total quantity.
 *
 * Single-threaded: all calls happen on the async worker thread.
 */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <functional>

namespace wt_option {

class OrderAnomalyGuard {
public:
    using AlertFn = std::function<void(const std::string& msg)>;

    explicit OrderAnomalyGuard(double timeFnNowSec = 0.0) {}

    void setAlertCallback(AlertFn cb) { m_alert = std::move(cb); }
    void setGetTimeFn(std::function<double()> fn) { m_getTime = std::move(fn); }

    // ---- registration (called by executors right after a successful send) --
    void onIssued(uint32_t localid, const std::string& code, uint32_t qty) {
        if (localid == 0) return;
        auto& rec = m_orders[localid];
        rec.code = code;
        rec.totalQty = qty;
        rec.filledQty = 0;
        rec.done = false;
        rec.issuedTime = nowSec();
    }

    // ---- order status -------------------------------------------------------
    // Returns true when the done/cancel is for an UNKNOWN order (anomaly).
    void onOrderDone(uint32_t localid, bool isCanceled, double leftQty) {
        auto it = m_orders.find(localid);
        if (it == m_orders.end()) {
            // Unknown done: could be a manual/hot-param order — just note it.
            return;
        }
        if (leftQty <= 0 || isCanceled) {
            it->second.done = true;
            it->second.doneTime = nowSec();
        }
    }

    // ---- fill routing -------------------------------------------------------
    enum class FillClass { Normal, LateAfterDone, Unknown, Overfill };

    /// Classify and account a fill. `code`/`vol` returned via fields for reuse.
    FillClass onFill(uint32_t localid, uint32_t vol) {
        auto it = m_orders.find(localid);
        if (it == m_orders.end()) {
            m_unknownFills++;
            raise("order-not-found fill: localid=" + std::to_string(localid)
                  + " count=" + std::to_string(m_unknownFills));
            if (m_unknownFills == 3 || m_unknownFills % 100 == 0)
                escalate();
            return FillClass::Unknown;
        }
        if (it->second.done) {
            m_lateAfterDone++;
            return FillClass::LateAfterDone;
        }
        it->second.filledQty += vol;
        if (it->second.totalQty > 0 && it->second.filledQty > it->second.totalQty) {
            m_overfills++;
            raise("OVERFILL: " + it->second.code + " filled=" +
                  std::to_string(it->second.filledQty) + " > total=" +
                  std::to_string(it->second.totalQty));
            return FillClass::Overfill;
        }
        return FillClass::Normal;
    }

    // ---- queries / maintenance ---------------------------------------------
    size_t unknownFillCount() const { return m_unknownFills; }
    size_t lateAfterDoneCount() const { return m_lateAfterDone; }
    size_t overfillCount() const { return m_overfills; }

    void reset() {
        m_orders.clear();
        m_unknownFills = m_lateAfterDone = m_overfills = 0;
    }

private:
    struct Rec {
        std::string code;
        uint32_t totalQty = 0;
        uint32_t filledQty = 0;
        bool done = false;
        double issuedTime = 0;
        double doneTime = 0;
    };

    double nowSec() const { return m_getTime ? m_getTime() : 0.0; }
    void raise(const std::string& msg) {
        if (m_alert) m_alert(msg);
    }
    void escalate() {
        raise("ORDER ANOMALY ESCALATION: " + std::to_string(m_unknownFills) +
              " unknown fills — recommend StopQuoting review");
    }

    std::unordered_map<uint32_t, Rec> m_orders;
    size_t m_unknownFills = 0;
    size_t m_lateAfterDone = 0;
    size_t m_overfills = 0;
    std::function<double()> m_getTime;
    AlertFn m_alert;
};

} // namespace wt_option
