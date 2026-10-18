#pragma once
#include "IScanModule.h"
#include "../OptionData.h"
#include "../optioncoretypes.h"
#include <string>
#include <memory>
#include <deque>

namespace wt_option {

class GarchScanner : public IScanModule {
public:
    struct Config {
        std::string ident = "GarchScanner";
        bool enable = true;
        double min_edge = 0.5;
        double delta_min = 0.1;
        double delta_max = 0.9;
        // R7: GARCH(1,1) kernel params — variance recursion
        //   σ²_t = ω + α·r²_{t−1} + β·σ²_{t−1}
        // Defaults approximate a typical CN index-futures regime; stationarity
        // requires α+β < 1 (enforced at use).
        double garch_omega = 1e-10;
        double garch_alpha = 0.08;
        double garch_beta  = 0.90;
        int    warmup_returns = 50;     // min samples before signals fire
        // Annualization: returns are per-update; default assumes ~1 update/min
        // (252 trading days × 240 min). Calibrate to actual feed cadence.
        double annualization_factor = 60480.0;
    };

    GarchScanner(const Config& cfg, OptionTraderContextPtr ctx) : m_config(cfg), m_ctx(ctx) {}
    // Factory bridge (R6): unified creation path from ScannerConfig.
    // Per-scanner numeric params stay at code defaults until the
    // ScannerConfig schema grows per-type fields.
    explicit GarchScanner(const ScannerConfig& sc) : GarchScanner(Config{}, nullptr)
    {
        if (!sc.name.empty()) m_config.ident = sc.name;
        m_config.enable = sc.enabled;
        m_config.min_edge = sc.min_edge;
        m_config.delta_min = sc.delta_min;
        m_config.delta_max = sc.delta_max;
        m_config.garch_omega = sc.garch_omega;
        m_config.garch_alpha = sc.garch_alpha;
        m_config.garch_beta = sc.garch_beta;
        m_config.warmup_returns = sc.warmup_returns;
        m_config.annualization_factor = sc.annualization_factor;
    }
    virtual ~GarchScanner() {}

    std::string getName() const override { return m_config.ident; }
    void onStart() override;
    void onStop() override { m_active = false; }
    void onPanic() override { m_active = false; }
    void onOptionHit(OptionData* od, int32_t index) override { (void)index; onOptionUpdate(od); }
    void onOptionUpdate(OptionData* od) override;

    /// R7: feed underlying mid prices; maintains the GARCH(1,1) state on close-to-close returns
    void onUnderlyingUpdate(double price) override;

    double scanOption(OptionData* opt);

    // Test/diagnostic accessors
    bool  volReady() const { return m_samples >= m_config.warmup_returns; }
    double forecastVol() const { return m_sigma; }          // per-return σ (same unit as returns)

private:
    void updateGarch(double ret);

    Config m_config;
    OptionTraderContextPtr m_ctx;

    // R7: GARCH(1,1) state
    double m_lastPrice = 0.0;
    double m_sigma = 0.0;            // conditional std-dev of returns
    int    m_samples = 0;
};

} // namespace wt_option
