#pragma once
/*!
 * \file EdgeScannerBase.h
 * \brief Shared implementation of the four "edge" scanners.
 *
 * Convergence (plan batch 2.1): Open/Simplex/StrikeSpread/LowBids scanners
 * were byte-identical except class name and the hit-reason string.  One base
 * implementation lives here; each concrete scanner is a thin name+reason
 * alias so existing config "type" strings keep working unchanged.
 */
#include "IScanModule.h"
#include "../OptionData.h"
#include "../optioncoretypes.h"
#include <string>
#include <memory>
#include <cmath>
#include <utility>

namespace wt_option {

class EdgeScannerBase : public IScanModule {
public:
    struct Config {
        std::string ident;
        bool enable = true;
        double min_edge = 0.5;
        double delta_min = 0.1;
        double delta_max = 0.9;
    };

    EdgeScannerBase(Config cfg, OptionTraderContextPtr ctx, const char* reason)
        : m_config(std::move(cfg)), m_ctx(std::move(ctx)), m_reason(reason) {}

    // Factory bridge (R6): unified creation path from ScannerConfig.
    // Per-scanner numeric params stay at code defaults until the
    // ScannerConfig schema grows per-type fields.
    explicit EdgeScannerBase(const ScannerConfig& sc, const char* defaultIdent,
                             const char* reason)
        : EdgeScannerBase(Config{defaultIdent, true, 0.5, 0.1, 0.9}, nullptr, reason)
    {
        if (!sc.name.empty()) m_config.ident = sc.name;
        m_config.enable = sc.enabled;
        m_config.min_edge = sc.min_edge;
        m_config.delta_min = sc.delta_min;
        m_config.delta_max = sc.delta_max;
    }

    std::string getName() const override { return m_config.ident; }
    void onStart() override { m_active = true; }
    void onStop() override { m_active = false; }
    void onPanic() override { m_active = false; }
    void onOptionHit(OptionData* od, int32_t index) override { (void)index; onOptionUpdate(od); }
    void onOptionUpdate(OptionData* od) override;

    double scanOption(OptionData* opt);

private:
    Config m_config;
    OptionTraderContextPtr m_ctx;
    const char* m_reason;
};

inline void EdgeScannerBase::onOptionUpdate(OptionData* od) {
    if (!od || !m_active || !isEnabled()) return;
    const double score = scanOption(od);
    if (score > 0)
        publishHit(od, score, m_reason);
}

inline double EdgeScannerBase::scanOption(OptionData* opt) {
    if (!m_active || !opt) return 0.0;
    if (opt->getBid() <= 0 || opt->getAsk() <= 0) return 0.0;
    double delta = std::abs(opt->greeks().delta());
    if (delta < m_config.delta_min || delta > m_config.delta_max) return 0.0;
    double theo = opt->getTheoPrice();
    if (theo <= 0) return 0.0;
    double mid = (opt->getBid() + opt->getAsk()) * 0.5;
    double edge = std::abs(theo - mid);
    if (edge < m_config.min_edge) return 0.0;
    return edge;
}

} // namespace wt_option
