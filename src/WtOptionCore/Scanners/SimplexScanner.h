#pragma once
#include "IScanModule.h"
#include "../OptionData.h"
#include "../optioncoretypes.h"
#include <string>
#include <memory>

namespace wt_option {

class SimplexScanner : public IScanModule {
public:
    struct Config {
        std::string ident = "SimplexScanner";
        bool enable = true;
        double min_edge = 0.5;
        double delta_min = 0.1;
        double delta_max = 0.9;
    };

    SimplexScanner(const Config& cfg, OptionTraderContextPtr ctx) : m_config(cfg), m_ctx(ctx) {}
    virtual ~SimplexScanner() {}

    std::string getName() const override { return m_config.ident; }
    void onStart() override { m_active = true; }
    void onStop() override { m_active = false; }
    void onPanic() override { m_active = false; }
    void onOptionHit(OptionData* od, int32_t index) override {}

    double scanOption(OptionData* opt);

private:
    Config m_config;
    OptionTraderContextPtr m_ctx;
    bool m_active = false;
};

} // namespace wt_option
