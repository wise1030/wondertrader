#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <memory>

namespace wt_option {

class PositionOffsetMgr {
public:
    PositionOffsetMgr() = default;

    void onPositionUpdate(bool isLong, double prevol, double preavail,
                           double newvol, double newavail);

    void onOrderSent(bool isBuy, uint32_t qty, bool isCloseToday);
    void onOrderCancelled(bool isBuy, uint32_t qty, bool isCloseToday);
    void onFill(bool isBuy, uint32_t fillQty, bool isCloseToday);

    int32_t getCloseableToday(bool isBuy) const;
    int32_t getCloseablePrev(bool isBuy) const;
    int32_t getCloseableTotal(bool isBuy) const;

    int32_t getLongToday() const { return m_brokerLongTodayAvail; }
    int32_t getLongPrev() const { return m_brokerLongPrevAvail; }
    int32_t getShortToday() const { return m_brokerShortTodayAvail; }
    int32_t getShortPrev() const { return m_brokerShortPrevAvail; }

    int32_t getLongTotal() const { return m_brokerLongVol; }
    int32_t getShortTotal() const { return m_brokerShortVol; }

    int32_t getLocalLong() const { return m_localLongVol; }
    int32_t getLocalShort() const { return m_localShortVol; }
    int32_t getLocalNet() const { return m_localLongVol - m_localShortVol; }
    int32_t getBrokerNet() const { return m_brokerLongVol - m_brokerShortVol; }

    struct OrderBreakdown {
        uint32_t closeTodayQty = 0;
        uint32_t closePrevQty = 0;
        uint32_t openQty = 0;
    };
    OrderBreakdown getOrderBreakdown(bool isBuy, uint32_t qty) const;

    struct DiscrepancyInfo {
        bool hasDiscrepancy = false;
        int32_t brokerNet = 0;
        int32_t localNet = 0;
        int32_t diff = 0;
    };
    DiscrepancyInfo checkDiscrepancy() const;

    void resetLocal() { m_localLongVol = m_localShortVol = 0; }
    void syncLocalToBroker();

private:
    int32_t m_brokerLongVol = 0;
    int32_t m_brokerLongAvail = 0;
    int32_t m_brokerShortVol = 0;
    int32_t m_brokerShortAvail = 0;
    int32_t m_brokerLongTodayAvail = 0;
    int32_t m_brokerLongPrevAvail = 0;
    int32_t m_brokerShortTodayAvail = 0;
    int32_t m_brokerShortPrevAvail = 0;

    int32_t m_frozenLongToday = 0;
    int32_t m_frozenLongPrev = 0;
    int32_t m_frozenShortToday = 0;
    int32_t m_frozenShortPrev = 0;

    int32_t m_localLongVol = 0;
    int32_t m_localShortVol = 0;
};

using PositionOffsetMgrPtr = std::shared_ptr<PositionOffsetMgr>;

} // namespace wt_option
