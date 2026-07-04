/*!
 * \file MMScanner.h
 * \brief Market Making Scanner — scans for MM opportunities
 */
#pragma once
#include "../IScanModule.h"
#include "../OptionData.h"
#include "../OptionValues.h"
#include "../optioncoretypes.h"
#include <string>
#include <memory>

namespace wt_option {

class MMScanner : public IScanModule {
public:
    struct Config {
        std::string ident = "MMScanner";
        bool enable = true;
        double min_edge = 0.5;
        double max_spread = 10.0;
        int32_t min_size = 1;
        double delta_min = 0.1;
        double delta_max = 0.9;
    };

    MMScanner(const Config& cfg, OptionTraderContextPtr ctx) : m_config(cfg), m_ctx(ctx) {}
    virtual ~MMScanner() {}

    std::string getName() const override { return m_config.ident; }
    void onStart() override { m_active = true; }
    void onStop() override { m_active = false; }
    void onPanic() override { m_active = false; }
    void onOptionHit(OptionData* od, int32_t index) override {}

    enum ScanMode { SCANNER = 0, MAKER = 1 };

    // Check if option should be scanned
    int shouldScanOption(OptionData* opt, ScanMode smode);
    // Scan option for opportunities, returns score (0 = no opportunity)
    double scanOption(OptionData* opt, ScanMode smode);
    // Fire on option hit
    bool fireOption(OptionData* opt, int dir, double px, double score, ScanMode smode);

private:
    Config m_config;
    OptionTraderContextPtr m_ctx;
    bool m_active = false;
};

} // namespace wt_option
