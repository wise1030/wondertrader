#pragma once
#include "IScanModule.h"
#include "../OptionData.h"
#include "../optioncoretypes.h"
#include <string>
#include <memory>

namespace wt_option {

class LowBidsScanner : public IScanModule {
public:
    struct Config {
        std::string ident = "LowBidsScanner";
        bool enable = true;
        double min_edge = 0.5;
        double delta_min = 0.1;
        double delta_max = 0.9;
    };

    LowBidsScanner(const Config& cfg, OptionTraderContextPtr ctx) : m_config(cfg), m_ctx(ctx) {}
    // Factory bridge (R6): unified creation path from ScannerConfig.
    // Per-scanner numeric params stay at code defaults until the
    // ScannerConfig schema grows per-type fields.
    explicit LowBidsScanner(const ScannerConfig& sc) : LowBidsScanner(Config{}, nullptr)
    {
        if (!sc.name.empty()) m_config.ident = sc.name;
        m_config.enable = sc.enabled;
        m_config.min_edge = sc.min_edge;
        m_config.delta_min = sc.delta_min;
        m_config.delta_max = sc.delta_max;
    }
    virtual ~LowBidsScanner() {}

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
};

} // namespace wt_option
