#include "StressAnalyzer.h"

#include <cmath>
#include <limits>

namespace wt_option {

StressAnalyzer::StressAnalyzer() = default;

void StressAnalyzer::addDefaultScenarios()
{
    clearScenarios();
    // Spot-only tails + vol-only shocks + a combined crash scenario.
    addScenario({"spot_-20%", -0.20, 0.0, 0.0});
    addScenario({"spot_-10%", -0.10, 0.0, 0.0});
    addScenario({"spot_+10%",  0.10, 0.0, 0.0});
    addScenario({"spot_+20%",  0.20, 0.0, 0.0});
    addScenario({"vol_+5",     0.0,  0.05, 0.0});
    addScenario({"vol_-5",     0.0, -0.05, 0.0});
    addScenario({"crash_-10%_vol+10", -0.10, 0.10, 1.0});
}

void StressAnalyzer::addScenario(const StressScenario& s)
{
    if (!std::isfinite(s.spot_shock_pct) || !std::isfinite(s.vol_shock)
        || !std::isfinite(s.days_forward))
        return;
    m_scenarios.push_back(s);
}

void StressAnalyzer::clearScenarios()
{
    m_scenarios.clear();
}

double StressAnalyzer::evaluate(const StressScenario& s, double spot,
                                double delta, double gamma, double vega, double theta)
{
    if (!(spot > 0) || !std::isfinite(spot)) return 0.0;
    if (!std::isfinite(delta) || !std::isfinite(gamma)
        || !std::isfinite(vega) || !std::isfinite(theta))
        return 0.0;

    const double dS = spot * s.spot_shock_pct;
    const double pnl = delta * dS
                     + 0.5 * gamma * dS * dS
                     + vega * s.vol_shock
                     + theta * s.days_forward;
    return std::isfinite(pnl) ? pnl : 0.0;
}

std::vector<StressResult> StressAnalyzer::evaluateAll(double spot, double delta,
                                                      double gamma, double vega,
                                                      double theta) const
{
    std::vector<StressResult> out;
    out.reserve(m_scenarios.size());
    for (const auto& s : m_scenarios)
        out.push_back({s, evaluate(s, spot, delta, gamma, vega, theta)});
    return out;
}

StressResult StressAnalyzer::worst(double spot, double delta, double gamma,
                                   double vega, double theta) const
{
    StressResult w;
    w.scenario.name = "<none>";
    w.pnl = 0.0;
    bool have = false;
    for (const auto& s : m_scenarios) {
        const double pnl = evaluate(s, spot, delta, gamma, vega, theta);
        if (!have || pnl < w.pnl) {
            w = {s, pnl};
            have = true;
        }
    }
    return w;
}

} // namespace wt_option
