#include "PositionGuard.h"
#include "../WTSTools/WTSLogger.h"
#include <cmath>
#include <algorithm>

namespace wt_option {

// C2: roll back a pending optimistic adjustment once its undo window expires
void PositionGuard::applyUndoIfDue(double now) {
    if (m_pendingAdjust == 0) return;
    double elapsed = now - m_pendingAdjustTime;
    if (m_cfg.undoWindowSec > 0 && elapsed < m_cfg.undoWindowSec)
        return;   // still inside the window, keep waiting for a fill

    // Window expired without an explaining fill -> commit the adjustment
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "PositionGuard: optimistic adjustment committed ({:+d}), internal={}",
        m_pendingAdjust, m_internalPos);
    m_pendingAdjust = 0;
}

bool PositionGuard::isBrokerStale() const {
    if (m_cfg.staleFreezeSec <= 0 || !m_initialized || !m_getTime)
        return false;
    return (m_getTime() - m_lastBrokerUpdate) > m_cfg.staleFreezeSec;
}

void PositionGuard::onFill(bool isBuy, uint32_t qty) {
    int32_t signedQty = isBuy ? static_cast<int32_t>(qty) : -static_cast<int32_t>(qty);
    m_internalPos += signedQty;

    // C2: a fill inside the undo window explains the discrepancy — roll back
    // the optimistic adjustment instead of letting it double-count.
    if (m_pendingAdjust != 0 && m_getTime) {
        double now = m_getTime();
        if ((now - m_pendingAdjustTime) < m_cfg.undoWindowSec) {
            m_internalPos -= m_pendingAdjust;
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "PositionGuard: UNDO optimistic adjust ({:+d}) on fill, internal={}",
                m_pendingAdjust, m_internalPos);
            m_pendingAdjust = 0;
        }
    }

    int32_t diff = m_internalPos - m_brokerPos;
    if (std::abs(diff) > m_cfg.tolerance) {
        double now = m_getTime ? m_getTime() : 0;
        if (now - m_lastAlertTime >= m_cfg.alertCooldownSec || m_lastAlertTime == 0) {
            m_lastAlertTime = now;
            WTSLogger::log_by_cat("strategy", LL_ERROR,
                "PositionGuard DISCREPANCY: internal={} broker={} diff={}",
                m_internalPos, m_brokerPos, diff);

            if (m_cfg.disableOnBreach && !m_disabled) {
                m_disabled = true;
                WTSLogger::log_by_cat("strategy", LL_ERROR,
                    "PositionGuard: TRADING DISABLED due to position discrepancy");
            }

            if (m_callback) m_callback("position", diff);
        }
    }
}

void PositionGuard::onBrokerPosition(bool isLong, double vol) {
    int32_t signedVol = isLong ? static_cast<int32_t>(vol) : -static_cast<int32_t>(vol);

    if (!m_initialized) {
        m_internalPos = signedVol;
        m_brokerPos = signedVol;
        m_initialized = true;
        m_disabled = false;
        m_lastBrokerUpdate = m_getTime ? m_getTime() : 0;
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "PositionGuard: initialized from broker pos={}", signedVol);
        return;
    }

    double now = m_getTime ? m_getTime() : 0;
    applyUndoIfDue(now);
    m_lastBrokerUpdate = now;

    // C2 (quantbox optimistic reconciliation): when the broker reports a
    // position that differs from internal by exactly the pending adjustment,
    // accept it. Otherwise, if we previously made no adjustment and the diff
    // appears, stage it optimistically (clamped below via clampLimit).
    if (m_pendingAdjust != 0) {
        m_internalPos -= m_pendingAdjust;   // settle previous adjustment first
        m_pendingAdjust = 0;
    }

    m_brokerPos = signedVol;

    int32_t diff = m_internalPos - m_brokerPos;
    if (std::abs(diff) > std::max(m_cfg.tolerance, 0)) {
        // C1: clamp — broker wins when the divergence blows past clampLimit
        if (m_cfg.clampLimit != 0 && std::abs(diff) > m_cfg.clampLimit) {
            WTSLogger::log_by_cat("strategy", LL_WARN,
                "PositionGuard CLAMP: internal={} -> broker={} (diff={} > limit={})",
                m_internalPos, m_brokerPos, diff, m_cfg.clampLimit);
            m_internalPos = m_brokerPos;
            diff = 0;
        } else if (m_cfg.undoWindowSec > 0 && !m_disabled) {
            // Stage optimistic adjustment with rollback window
            m_pendingAdjust = -diff;                 // adjustment that would align
            m_internalPos = m_brokerPos;             // apply optimistically
            m_pendingAdjustTime = now;
            WTSLogger::log_by_cat("strategy", LL_WARN,
                "PositionGuard: optimistic adjust {:+d} (undo window {:.1f}s)",
                m_pendingAdjust, m_cfg.undoWindowSec);
            diff = 0;
        }
    }

    // Re-evaluate breach state after any clamping/adjustment
    diff = m_internalPos - m_brokerPos;
    if (std::abs(diff) > m_cfg.tolerance) {
        if (now - m_lastAlertTime >= m_cfg.alertCooldownSec || m_lastAlertTime == 0) {
            m_lastAlertTime = now;
            WTSLogger::log_by_cat("strategy", LL_ERROR,
                "PositionGuard DISCREPANCY (broker update): internal={} broker={} diff={}",
                m_internalPos, m_brokerPos, diff);

            if (m_cfg.disableOnBreach && !m_disabled) {
                m_disabled = true;
                WTSLogger::log_by_cat("strategy", LL_ERROR,
                    "PositionGuard: TRADING DISABLED due to position discrepancy");
            }

            if (m_callback) m_callback("position", diff);
        }
    } else if (m_disabled) {
        m_disabled = false;
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "PositionGuard: discrepancy resolved (diff={}), trading re-enabled", diff);
    }
}

void PositionGuard::reconcile() {
    m_internalPos = m_brokerPos;
    m_pendingAdjust = 0;
    m_disabled = false;
    m_initialized = true;
    m_lastBrokerUpdate = m_getTime ? m_getTime() : 0;
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "PositionGuard: reconciled to broker pos={}", m_brokerPos);
}

} // namespace wt_option
