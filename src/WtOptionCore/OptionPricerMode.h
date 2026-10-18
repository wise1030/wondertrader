/*!
 * \file OptionPricerMode.h
 * \brief Strategy/pricer boundary.
 *
 * CompositeOptionPricer = market-making strategy pricer.
 * GammaScalpOptionPricer = independent gamma-scalping strategy pricer.
 * They share the Black/vol engine (OptionPricer2) but NOT strategy logic and
 * must not be merged into one implementation.
 */
#pragma once

#include <string>

namespace wt_option {

enum class OptionPricerMode {
    CompositeMM = 0,
    GammaScalp  = 1,
};

inline bool parseOptionPricerMode(const std::string& s, OptionPricerMode& out)
{
    if (s.empty() || s == "composite" || s == "composite_mm" || s == "mm") {
        out = OptionPricerMode::CompositeMM;
        return true;
    }
    if (s == "gammascalp" || s == "gamma_scalp" || s == "gamma") {
        out = OptionPricerMode::GammaScalp;
        return true;
    }
    return false;
}

inline const char* to_string(OptionPricerMode m)
{
    return m == OptionPricerMode::GammaScalp ? "gammascalp" : "composite_mm";
}

} // namespace wt_option
