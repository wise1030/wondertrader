#include "PositionGuard.h"
#include "../WTSTools/WTSLogger.h"
#include <cmath>

namespace wt_option {

void PositionGuard::onFill(bool isBuy, uint32_t qty) {
    int32_t signedQty = isBuy ? static_cast<int32_t>(qty) : -static_cast<int32_t>(qty);
    m_internalPos += signedQty;

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
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "PositionGuard: initialized from broker pos={}", signedVol);
        return;
    }

    m_brokerPos = signedVol;

    int32_t diff = m_internalPos - m_brokerPos;
    if (std::abs(diff) > m_cfg.tolerance) {
        double now = m_getTime ? m_getTime() : 0;
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
    m_disabled = false;
    m_initialized = true;
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "PositionGuard: reconciled to broker pos={}", m_brokerPos);
}

} // namespace wt_option
