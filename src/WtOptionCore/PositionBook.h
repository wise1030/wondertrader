/*!
 * \file PositionBook.h
 * \brief Authoritative long/short position book for broker snapshots and fills.
 *
 * WT reports prev/new as yesterday/today buckets and reports long/short as
 * separate callbacks.  This book keeps both sides explicit, preserves
 * availability, and bridges fills only after the authoritative snapshot has
 * been applied.  All consumers that only need a net view can use net().
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace wt_option {

class PositionBook {
public:
    struct SidePosition {
        double prevVol = 0;
        double prevAvail = 0;
        double newVol = 0;      // today
        double newAvail = 0;    // today available

        double vol() const { return prevVol + newVol; }
        double avail() const { return prevAvail + newAvail; }
        bool empty() const { return vol() == 0.0 && avail() == 0.0; }
    };

    struct InstrumentPosition {
        SidePosition longPos;
        SidePosition shortPos;

        double net() const { return longPos.vol() - shortPos.vol(); }
        double netAvail() const { return longPos.avail() - shortPos.avail(); }
        double gross() const { return longPos.vol() + shortPos.vol(); }
        bool empty() const { return longPos.empty() && shortPos.empty(); }
    };

    using Snapshot = std::map<std::string, InstrumentPosition>;

    /// Clear stale state on channel recovery/session begin.  Broker snapshots
    /// are authoritative and will repopulate the book.
    void clearAll() {
        m_positions.clear();
        ++m_seq;
    }

    void onBrokerPosition(const std::string& code, bool isLong,
                          double prevol, double preavail,
                          double newvol, double newavail) {
        if (code.empty()) return;
        SidePosition side{prevol, preavail, newvol, newavail};
        if (isLong) m_positions[code].longPos = side;
        else        m_positions[code].shortPos = side;
        ++m_seq;
    }

    /// Fill events are incremental bridges between authoritative broker reports.
    void addFill(const std::string& code, bool isBuy, double qty) {
        if (code.empty() || qty <= 0) return;
        auto& pos = m_positions[code];
        SidePosition& side = isBuy ? pos.longPos : pos.shortPos;
        side.newVol += qty;
        side.newAvail += qty;
        ++m_seq;
    }

    const InstrumentPosition& get(const std::string& code) const {
        static const InstrumentPosition emptyPos;
        auto it = m_positions.find(code);
        return it == m_positions.end() ? emptyPos : it->second;
    }

    bool has(const std::string& code) const {
        return m_positions.find(code) != m_positions.end();
    }

    double net(const std::string& code) const { return get(code).net(); }
    double gross(const std::string& code) const { return get(code).gross(); }

    Snapshot snapshot() const { return m_positions; }
    uint64_t version() const { return m_seq; }

private:
    Snapshot m_positions;
    uint64_t m_seq = 0;
};

} // namespace wt_option
