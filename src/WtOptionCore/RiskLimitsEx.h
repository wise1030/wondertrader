#pragma once

#include <string>
#include <cstdint>
#include <cmath>

namespace wt_option {

struct RiskLimitsEx {
    // Greeks limits
    double maxDelta = 1000;
    double maxGamma = 100;
    double maxVega = 10000;
    double maxLossPerDay = 100000;
    double panicThreshold = 0.05;

    // Pre-trade limits
    uint32_t maxOrderSize = 100;
    double maxOrderValue = 1000000;
    double clearlyErroneousPercent = 0.05;
    uint32_t maxBurstOrdersPerSec = 50;
    double minSellPrice = 0;

    // Position limits
    int32_t maxPositionPerOption = 100;
    int32_t maxTotalPosition = 1000;
    int32_t maxShortCallPerSymbol = 0;
    int32_t maxShortPutPerSymbol = 0;

    enum class CheckResult { PASS, REJECT, WARN };

    struct CheckReport {
        CheckResult result = CheckResult::PASS;
        std::string reason;
        double value = 0;
        double limit = 0;
        operator bool() const { return result == CheckResult::PASS; }
    };

    CheckReport checkPreTrade(const std::string& code, bool isBuy,
                               double price, uint32_t qty,
                               int32_t currentPosition,
                               double refPrice) const;

    CheckReport checkPostTrade(int32_t totalPosition, double totalExposure) const;

    CheckReport checkGreeks(double delta, double gamma, double vega, double pnl) const;

    CheckReport checkOrderSize(uint32_t qty) const;
    CheckReport checkOrderValue(double price, uint32_t qty) const;
    CheckReport checkClearlyErroneous(double price, double refPrice) const;
    CheckReport checkMinSellPrice(bool isBuy, double price) const;
    CheckReport checkPosition(int32_t position) const;

    /// R1: exchange price-limit guard (quantbox ShfePriceLimits absorption —
    /// limits come straight off the WT tick feed's upper_limit/lower_limit).
    /// upper/lower <= 0 means "unknown", which passes.
    CheckReport checkPriceLimit(double price, double upper, double lower) const;
};

} // namespace wt_option
