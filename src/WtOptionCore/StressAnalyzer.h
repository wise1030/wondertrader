/*!
 * \file StressAnalyzer.h
 * \brief Delta-gamma-vega-theta scenario stress for the option book.
 *
 * Phase 2: the strategy previously only aggregated Greeks; there was no
 * forward-looking loss estimate under spot/vol shocks. This is a compact
 * scenario engine (not a full SPAN replica): each scenario applies a relative
 * spot shock, an absolute vol shock and a one-day theta decay to the current
 * portfolio Greeks.
 */
#pragma once

#include <string>
#include <vector>

namespace wt_option {

struct StressScenario {
    std::string name;
    double spot_shock_pct = 0.0;   // -0.10 = spot -10%
    double vol_shock = 0.0;        // absolute vol points, 0.05 = +5 vol
    double days_forward = 0.0;     // theta days applied
};

struct StressResult {
    StressScenario scenario;
    double pnl = 0.0;
};

class StressAnalyzer {
public:
    StressAnalyzer();

    void addDefaultScenarios();
    void addScenario(const StressScenario& s);
    void clearScenarios();
    const std::vector<StressScenario>& scenarios() const { return m_scenarios; }

    /// Evaluate one scenario. Greeks are portfolio-level and already include
    /// contract multipliers. `vega` is PnL per 1.00 vol; `theta` is PnL/day.
    static double evaluate(const StressScenario& s, double spot,
                           double delta, double gamma, double vega, double theta);

    /// Evaluate all scenarios in insertion order.
    std::vector<StressResult> evaluateAll(double spot, double delta,
                                          double gamma, double vega, double theta) const;

    /// Worst (most negative) scenario; empty/invalid book returns a zero result
    /// with name "<none>".
    StressResult worst(double spot, double delta, double gamma,
                       double vega, double theta) const;

private:
    std::vector<StressScenario> m_scenarios;
};

} // namespace wt_option
