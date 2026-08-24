#include "PositionOffsetMgr.h"
#include "../WTSTools/WTSLogger.h"
#include <algorithm>
#include <cmath>

namespace wt_option {

void PositionOffsetMgr::onPositionUpdate(bool isLong, double prevol, double preavail,
                                           double newvol, double newavail) {
    // A1 fix: WT's on_position reports ABSOLUTE volumes (prev/new snapshots),
    // not deltas — the old `prevol + newvol` double-counted.
    // NOTE: the callback does not expose a today/prev split of availability,
    // so we conservatively book ALL available volume as close-today (matches
    // intraday MM reality on SHFE) and keep closePrev at 0 until a richer
    // position-detail source exists. getOrderBreakdown stays correct for the
    // combined view used by the offset guard.
    int32_t vol = static_cast<int32_t>(newvol);
    int32_t avail = static_cast<int32_t>(newavail);
    if (isLong) {
        m_brokerLongVol = vol;
        m_brokerLongAvail = avail;
        m_brokerLongTodayAvail = avail;
        m_brokerLongPrevAvail = 0;
    } else {
        m_brokerShortVol = vol;
        m_brokerShortAvail = avail;
        m_brokerShortTodayAvail = avail;
        m_brokerShortPrevAvail = 0;
    }
}

void PositionOffsetMgr::onOrderSent(bool isBuy, uint32_t qty, bool isCloseToday) {
    int32_t q = static_cast<int32_t>(qty);
    if (isBuy) {
        if (isCloseToday) m_frozenShortToday += q;
        else               m_frozenShortPrev += q;
    } else {
        if (isCloseToday) m_frozenLongToday += q;
        else               m_frozenLongPrev += q;
    }
}

void PositionOffsetMgr::onOrderCancelled(bool isBuy, uint32_t qty, bool isCloseToday) {
    int32_t q = static_cast<int32_t>(qty);
    if (isBuy) {
        if (isCloseToday) m_frozenShortToday = std::max(0, m_frozenShortToday - q);
        else               m_frozenShortPrev = std::max(0, m_frozenShortPrev - q);
    } else {
        if (isCloseToday) m_frozenLongToday = std::max(0, m_frozenLongToday - q);
        else               m_frozenLongPrev = std::max(0, m_frozenLongPrev - q);
    }
}

void PositionOffsetMgr::onFill(bool isBuy, uint32_t fillQty, bool isCloseToday) {
    int32_t q = static_cast<int32_t>(fillQty);
    // Unfreeze the frozen closeable bucket this fill consumed
    if (isBuy) {
        if (isCloseToday) m_frozenShortToday = std::max(0, m_frozenShortToday - q);
        else              m_frozenShortPrev = std::max(0, m_frozenShortPrev - q);
        m_localLongVol += q;
    } else {
        if (isCloseToday) m_frozenLongToday = std::max(0, m_frozenLongToday - q);
        else              m_frozenLongPrev = std::max(0, m_frozenLongPrev - q);
        m_localShortVol += q;
    }
}

int32_t PositionOffsetMgr::getCloseableToday(bool isBuy) const {
    if (isBuy) {
        return std::max(0, m_brokerShortTodayAvail - m_frozenShortToday);
    } else {
        return std::max(0, m_brokerLongTodayAvail - m_frozenLongToday);
    }
}

int32_t PositionOffsetMgr::getCloseablePrev(bool isBuy) const {
    if (isBuy) {
        return std::max(0, m_brokerShortPrevAvail - m_frozenShortPrev);
    } else {
        return std::max(0, m_brokerLongPrevAvail - m_frozenLongPrev);
    }
}

int32_t PositionOffsetMgr::getCloseableTotal(bool isBuy) const {
    return getCloseableToday(isBuy) + getCloseablePrev(isBuy);
}

PositionOffsetMgr::OrderBreakdown PositionOffsetMgr::getOrderBreakdown(bool isBuy, uint32_t qty) const {
    OrderBreakdown bd;
    uint32_t remaining = qty;

    uint32_t closeToday = static_cast<uint32_t>(getCloseableToday(isBuy));
    if (closeToday > 0) {
        bd.closeTodayQty = std::min(closeToday, remaining);
        remaining -= bd.closeTodayQty;
    }

    if (remaining > 0) {
        uint32_t closePrev = static_cast<uint32_t>(getCloseablePrev(isBuy));
        if (closePrev > 0) {
            bd.closePrevQty = std::min(closePrev, remaining);
            remaining -= bd.closePrevQty;
        }
    }

    bd.openQty = remaining;
    return bd;
}

PositionOffsetMgr::DiscrepancyInfo PositionOffsetMgr::checkDiscrepancy() const {
    DiscrepancyInfo info;
    info.brokerNet = getBrokerNet();
    info.localNet = getLocalNet();
    info.diff = info.localNet - info.brokerNet;
    info.hasDiscrepancy = (info.diff != 0);
    return info;
}

void PositionOffsetMgr::syncLocalToBroker() {
    m_localLongVol = m_brokerLongVol;
    m_localShortVol = m_brokerShortVol;
    m_frozenLongToday = 0;
    m_frozenLongPrev = 0;
    m_frozenShortToday = 0;
    m_frozenShortPrev = 0;
}

} // namespace wt_option
