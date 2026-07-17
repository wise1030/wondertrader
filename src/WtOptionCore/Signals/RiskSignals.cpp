#include "RiskSignals.h"
#include "SignalFactory.h"
#include "../Includes/WTSVariant.hpp"
#include "../WTSTools/WTSLogger.h"
#include <algorithm>
#include <cmath>

USING_NS_WTP;

namespace wt_option {

// ============================================================================
// ToxicitySignal
// ============================================================================
bool ToxicitySignal::init(WTSVariant* cfg) {
    if (!cfg) return true;
    m_maxAdverseFills = cfg->has("max_adverse_fills") ? cfg->getInt32("max_adverse_fills") : 3;
    m_windowSec       = cfg->has("window_sec") ? cfg->getDouble("window_sec") : 60.0;
    m_widenFactor     = cfg->has("widen_factor") ? cfg->getDouble("widen_factor") : 2.0;
    m_recoverySec     = cfg->has("recovery_sec") ? cfg->getDouble("recovery_sec") : 300.0;
    m_maxFillRatePerMin = cfg->has("max_fill_rate_per_min") ? cfg->getInt32("max_fill_rate_per_min") : 10;
    m_panicAdverseFills = cfg->has("panic_adverse_fills") ? cfg->getInt32("panic_adverse_fills") : 6;
    return true;
}

void ToxicitySignal::onFill(const std::string& code, bool isBuy, double qty, double price) {
    if (!m_enabled) return;
    auto& hist = m_fillHistory[code];
    FillRecord rec;
    rec.time = m_curTime;
    rec.isBuy = isBuy;
    rec.price = price;
    hist.push_back(rec);

    // Check if previous fills were adverse: if we bought and price dropped, or sold and price rose
    for (auto& prev : hist) {
        if (prev.resolved) continue;
        if (prev.isBuy) {
            if (price < prev.price - 1e-9) {
                prev.adverse = true;
            }
            prev.resolved = true;
        } else {
            if (price > prev.price + 1e-9) {
                prev.adverse = true;
            }
            prev.resolved = true;
        }
    }

    // Count consecutive adverse fills
    int32_t adverseCount = 0;
    for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
        if (it->adverse) adverseCount++;
        else break;
    }
    m_consecutiveAdverse[code] = adverseCount;

    if (adverseCount >= m_maxAdverseFills && m_curTime > 0) {
        m_lastAdverseTime[code] = m_curTime;
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "Toxicity {} adverse_fills={}", code, adverseCount);
    }
}

void ToxicitySignal::checkExpiry(double now) {
    m_curTime = now;
    // Expire old fill records
    for (auto& [code, hist] : m_fillHistory) {
        while (!hist.empty() && (now - hist.front().time) > m_windowSec) {
            hist.pop_front();
        }
    }
    // Expire adverse state
    for (auto it = m_lastAdverseTime.begin(); it != m_lastAdverseTime.end();) {
        if (now - it->second > m_recoverySec) {
            m_consecutiveAdverse.erase(it->first);
            it = m_lastAdverseTime.erase(it);
        } else {
            ++it;
        }
    }
}

void ToxicitySignal::onBatchEnd() {
    checkExpiry(m_curTime);

    // Check global fill rate
    int32_t totalRecentFills = 0;
    for (const auto& [code, hist] : m_fillHistory) {
        totalRecentFills += static_cast<int32_t>(hist.size());
    }
    if (totalRecentFills > m_maxFillRatePerMin) {
        m_globalWidenFactor = m_widenFactor;
        m_globalActionEndTime = m_curTime + m_recoverySec;
    }
    if (m_curTime > m_globalActionEndTime) {
        m_globalWidenFactor = 1.0;
    }
}

RiskAction ToxicitySignal::getAction() const {
    // Escalate to Panic if any single contract has very high adverse fills
    for (const auto& [code, count] : m_consecutiveAdverse) {
        if (count >= m_panicAdverseFills)
            return RiskAction::Panic;
    }
    if (m_globalWidenFactor > 1.0) return RiskAction::Widen;
    return RiskAction::None;
}

double ToxicitySignal::getWidenFactor() const {
    return m_globalWidenFactor;
}

RiskAction ToxicitySignal::getActionByCode(const std::string& code) const {
    auto it = m_consecutiveAdverse.find(code);
    if (it != m_consecutiveAdverse.end() && it->second >= m_maxAdverseFills)
        return RiskAction::Widen;
    return RiskAction::None;
}

double ToxicitySignal::getWidenFactorByCode(const std::string& code) const {
    auto it = m_consecutiveAdverse.find(code);
    if (it != m_consecutiveAdverse.end() && it->second >= m_maxAdverseFills)
        return m_widenFactor;
    return 1.0;
}

// ============================================================================
// PnlLimitSignal
// ============================================================================
bool PnlLimitSignal::init(WTSVariant* cfg) {
    if (!cfg) return true;
    m_maxDailyLoss = cfg->has("max_daily_loss") ? cfg->getDouble("max_daily_loss") : 100000.0;
    return true;
}

void PnlLimitSignal::onBatchEnd() {
    if (!m_enabled) return;
    if (m_portfolioPnl < -m_maxDailyLoss) {
        if (m_action != RiskAction::Panic) {
            WTSLogger::log_by_cat("strategy", LL_ERROR,
                "PnlLimit BREACH pnl={:.2f} limit={:.2f} → PANIC", m_portfolioPnl, m_maxDailyLoss);
        }
        m_action = RiskAction::Panic;
    }
}

// ============================================================================
// Registration
// ============================================================================
REGISTER_RISK_SIGNAL("Toxicity", ToxicitySignal)
REGISTER_RISK_SIGNAL("PnlLimit", PnlLimitSignal)

} // namespace wt_option
