#include "PositionOffsetMgr.h"
#include "../WTSTools/WTSLogger.h"
#include <algorithm>
#include <cmath>

namespace wt_option {

void PositionOffsetMgr::onPositionUpdate(bool isLong, double prevol, double preavail,
                                           double newvol, double newavail) {
    if (isLong) {
        m_brokerLongVol = static_cast<int32_t>(prevol + newvol);
        m_brokerLongAvail = static_cast<int32_t>(preavail + newavail);
        m_brokerLongPrevAvail = static_cast<int32_t>(preavail);
        m_brokerLongTodayAvail = static_cast<int32_t>(newavail);
    } else {
        m_brokerShortVol = static_cast<int32_t>(prevol + newvol);
        m_brokerShortAvail = static_cast<int32_t>(preavail + newavail);
        m_brokerShortPrevAvail = static_cast<int32_t>(preavail);
        m_brokerShortTodayAvail = static_cast<int32_t>(newavail);
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
    if (isBuy) {
        if (isCloseToday) {
            m_localLongVol += 0;
            m_frozenShortToday = std::max(0, m_frozenShortToday - q);
        } else {
            m_localLongVol += 0;
            m_frozenShortPrev = std::max(0, m_frozenShortPrev - q);
        }
    } else {
        if (isCloseToday) {
            m_localShortVol += 0;
            m_frozenLongToday = std::max(0, m_frozenLongToday - q);
        } else {
            m_localShortVol += 0;
            m_frozenLongPrev = std::max(0, m_frozenLongPrev - q);
        }
    }
    if (isBuy) m_localLongVol += q;
    else       m_localShortVol += q;
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
