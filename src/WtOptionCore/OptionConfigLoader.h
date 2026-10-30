/*!
 * \file OptionConfigLoader.h
 * \brief Typed option-strategy config loader with effective-value reporting.
 *
 * This loader does not replace every legacy direct read on day one.  It owns
 * the highest-risk typed fields that previously drifted from schema/docs and
 * provides an effective-value dump so a config key cannot be "valid but
 * silently unconsumed" without evidence.
 */
#pragma once

#include "../Includes/WTSVariant.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wt_option {

class OptionConfigLoader {
public:
    struct Values {
        double riskFreeRate = 0.03;
        bool riskFreeRateConfigured = false;
        int32_t maxTPS = 50;
        double minComputeInterval = 0.02;
        uint64_t marketFreshTimeoutMs = 3000;
        double slowComputeInterval = 15.0;
    };

    struct Result {
        bool ok = true;
        Values values;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        std::map<std::string, std::string> effective;
    };

    static Result load(wtp::WTSVariant* cfg);
    static std::string dumpEffectiveValues(const Result& result);
};

} // namespace wt_option
