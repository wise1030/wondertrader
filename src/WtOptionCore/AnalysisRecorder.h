/*!
 * \file AnalysisRecorder.h
 * \brief Analysis CSV sidecars (P1 batch-6.0) — the data source for the
 *        vol cone (6.2) and the PnL attribution review (6.3).
 *
 * Two append-only series, one row per expiry / per portfolio per minute:
 *
 *   generated/volseries_<tdate>.csv
 *     ts,expiry,atmvol,fwd,dte        — per expiry, minute cadence
 *
 *   generated/pfseries_<tdate>.csv
 *     ts,delta,gamma,vega,theta,pnl,spot,atmvol
 *                                     — portfolio greeks + pnl + front
 *                                       underlying/atmvol, minute cadence
 *
 * Design notes:
 *  - Pure logic + stdio, no WT dependencies — unit-testable.
 *  - The writer throttles itself (default 60s): callers feed it every batch
 *    and it no-ops until the interval elapses.  Day rollover renames the
 *    file (one file per trading day, same discipline as the quote-counter
 *    sidecar).
 *  - Headers are written on file creation, so a day file is self-describing.
 *  - g_pnl is carried through verbatim so the attribution math (6.3) can
 *    reconcile against the strategy's own PnL tracker.
 */
#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <string>

namespace wt_option {

class AnalysisRecorder {
public:
    explicit AnalysisRecorder(double intervalSec = 60.0) : m_interval(intervalSec) {}

    /// Change output directory ("generated" by default). Call before the
    /// first record of a session.
    void setDir(const std::string& dir) { m_dir = dir; }

    /// Per-expiry vol/forward sample.  Buffers the latest value per expiry;
    /// the flush writes ONE row per expiry.
    void recordVol(uint32_t expiry, double atmvol, double fwd, int32_t dte) {
        auto& s = m_vol[expiry];
        s.atmvol = atmvol; s.fwd = fwd; s.dte = dte;
        m_volDirty = true;
    }

    /// Portfolio greeks sample (overwrites the pending row — latest wins).
    void recordPf(double delta, double gamma, double vega, double theta,
                  double pnl, double spot, double atmvol) {
        m_pf = { delta, gamma, vega, theta, pnl, spot, atmvol };
        m_pfDirty = true;
    }

    /// Flush when the interval elapsed AND something changed.  `nowSec` is
    /// the strategy clock; `tdate` selects/creates the day file.
    bool maybeFlush(double nowSec, uint32_t tdate);

    /// Force-flush (session end).  Returns rows written.
    size_t flush(double nowSec, uint32_t tdate);

    // ---- test hooks ----
    double interval() const { return m_interval; }
    size_t pendingVolRows() const { return m_vol.size(); }
    bool pfPending() const { return m_pfDirty; }

private:
    struct VolRow { double atmvol = 0, fwd = 0; int32_t dte = 0; };
    struct PfRow   { double d = 0, g = 0, v = 0, t = 0, pnl = 0, spot = 0, atmvol = 0; };

    std::string volPath(uint32_t tdate) const {
        return m_dir + "/volseries_" + std::to_string(tdate) + ".csv";
    }
    std::string pfPath(uint32_t tdate) const {
        return m_dir + "/pfseries_" + std::to_string(tdate) + ".csv";
    }

    double m_interval;
    std::string m_dir = "generated";
    double m_lastFlush = 0;
    uint32_t m_tdate = 0;
    std::map<uint32_t, VolRow> m_vol;
    bool m_volDirty = false;
    PfRow m_pf;
    bool m_pfDirty = false;
};

// ============================================================================
// Implementation (header-inline: the class is header-only like
// BatchQuoteValidator — one TU in the strategy, one in the tests).
// ============================================================================
inline bool AnalysisRecorder::maybeFlush(double nowSec, uint32_t tdate) {
    if ((!m_volDirty && !m_pfDirty)) return false;
    if (m_lastFlush > 0 && nowSec - m_lastFlush < m_interval && tdate == m_tdate)
        return false;
    return flush(nowSec, tdate) > 0;
}

inline size_t AnalysisRecorder::flush(double nowSec, uint32_t tdate) {
    if (tdate == 0) return 0;
    size_t rows = 0;

    // Day rollover: nothing to do explicitly — the path embeds tdate.
    if (m_volDirty && !m_vol.empty()) {
        const std::string path = volPath(tdate);
        const bool exists = std::ifstream(path).good();
        std::ofstream out(path, std::ios::app);
        if (out.is_open()) {
            if (!exists) out << "ts,expiry,atmvol,fwd,dte\n";
            for (const auto& [exp, s] : m_vol)
                out << (uint64_t)nowSec << ',' << exp << ','
                    << s.atmvol << ',' << s.fwd << ',' << s.dte << '\n';
            ++rows;
        }
        m_volDirty = false;
    }

    if (m_pfDirty) {
        const std::string path = pfPath(tdate);
        const bool exists = std::ifstream(path).good();
        std::ofstream out(path, std::ios::app);
        if (out.is_open()) {
            if (!exists) out << "ts,delta,gamma,vega,theta,pnl,spot,atmvol\n";
            out << (uint64_t)nowSec << ',' << m_pf.d << ',' << m_pf.g << ','
                << m_pf.v << ',' << m_pf.t << ',' << m_pf.pnl << ','
                << m_pf.spot << ',' << m_pf.atmvol << '\n';
            ++rows;
        }
        m_pfDirty = false;
    }

    if (rows > 0) { m_lastFlush = nowSec; m_tdate = tdate; }
    return rows;
}

// ============================================================================
// P1 batch-6.5: quote stream recorder — the replayable tick record.
// One row per code per flush: ts,code,bid,ask,last,iv.  DEFAULT OFF (set
// qstream_sec > 0); at 1s cadence a 400-contract chain writes ~70MB/day,
// which is only acceptable when a replay session is being captured.
// ============================================================================
class QstreamRecorder {
public:
    explicit QstreamRecorder(double intervalSec = 1.0) : m_interval(intervalSec) {}

    void setDir(const std::string& dir) { m_dir = dir; }
    bool enabled() const { return m_interval > 0; }

    /// Latest quote per code (latest wins within a flush window).
    void record(const std::string& code, double bid, double ask, double last, double iv) {
        if (!enabled()) return;
        auto& r = m_q[code];
        r.bid = bid; r.ask = ask; r.last = last; r.iv = iv;
        m_dirty = true;
    }

    bool maybeFlush(double nowSec, uint32_t tdate) {
        if (!m_dirty || tdate == 0) return false;
        if (m_lastFlush > 0 && nowSec - m_lastFlush < m_interval) return false;
        const std::string path = m_dir + "/qstream_" + std::to_string(tdate) + ".csv";
        const bool exists = std::ifstream(path).good();
        std::ofstream out(path, std::ios::app);
        if (!out.is_open()) return false;
        if (!exists) out << "ts,code,bid,ask,last,iv\n";
        for (const auto& [code, r] : m_q)
            out << (uint64_t)nowSec << ',' << code << ',' << r.bid << ','
                << r.ask << ',' << r.last << ',' << r.iv << '\n';
        m_dirty = false;
        m_lastFlush = nowSec;
        return true;
    }

    void clearPending() { m_q.clear(); m_dirty = false; }

private:
    struct QRow { double bid = 0, ask = 0, last = 0, iv = 0; };
    double m_interval;
    std::string m_dir = "generated";
    double m_lastFlush = 0;
    std::map<std::string, QRow> m_q;
    bool m_dirty = false;
};

} // namespace wt_option
