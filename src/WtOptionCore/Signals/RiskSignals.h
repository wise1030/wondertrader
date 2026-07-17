#pragma once

#include "ISignal.h"
#include "WTSVariant.hpp"
#include <string>
#include <map>
#include <deque>
#include <cstdint>

namespace wt_option {

class ToxicitySignal : public IRiskSignal {
public:
    ToxicitySignal() : m_name("Toxicity") {}
    const std::string& getName() const override { return m_name; }
    bool init(wtp::WTSVariant* cfg) override;
    void onFill(const std::string& code, bool isBuy, double qty, double price) override;
    void onBatchEnd() override;
    RiskAction getAction() const override;
    double getWidenFactor() const override;
    RiskAction getActionByCode(const std::string& code) const override;
    double getWidenFactorByCode(const std::string& code) const override;

private:
    struct FillRecord {
        double time = 0;
        bool isBuy = false;
        double price = 0;
        bool resolved = false;
        bool adverse = false;
    };

    void checkExpiry(double now);

    std::string m_name;
    int32_t m_maxAdverseFills = 3;
    double m_windowSec = 60.0;
    double m_widenFactor = 2.0;
    double m_recoverySec = 300.0;
    int32_t m_maxFillRatePerMin = 10;
    int32_t m_panicAdverseFills = 6;   // escalate to Panic at this adverse count

    std::map<std::string, std::deque<FillRecord>> m_fillHistory;
    std::map<std::string, double> m_lastAdverseTime;
    std::map<std::string, int32_t> m_consecutiveAdverse;
    double m_curTime = 0;
    double m_globalWidenFactor = 1.0;
    double m_globalActionEndTime = 0;
};

class PnlLimitSignal : public IRiskSignal {
public:
    PnlLimitSignal() : m_name("PnlLimit") {}
    const std::string& getName() const override { return m_name; }
    bool init(wtp::WTSVariant* cfg) override;
    void onBatchEnd() override;
    RiskAction getAction() const override { return m_action; }

    void setPortfolioPnl(double pnl) { m_portfolioPnl = pnl; }

private:
    std::string m_name;
    double m_maxDailyLoss = 100000.0;
    RiskAction m_action = RiskAction::None;
    double m_portfolioPnl = 0;
};

} // namespace wt_option
