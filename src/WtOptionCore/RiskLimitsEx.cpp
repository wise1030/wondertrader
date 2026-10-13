#include "RiskLimitsEx.h"
#include <algorithm>

namespace wt_option {

RiskLimitsEx::CheckReport RiskLimitsEx::checkOrderSize(uint32_t qty) const {
    CheckReport r;
    if (qty > maxOrderSize) {
        r.result = CheckResult::REJECT;
        r.value = static_cast<double>(qty);
        r.limit = static_cast<double>(maxOrderSize);
        r.reason = "order_size";
    }
    return r;
}

RiskLimitsEx::CheckReport RiskLimitsEx::checkOrderValue(double price, uint32_t qty) const {
    CheckReport r;
    double value = price * qty;
    if (value > maxOrderValue) {
        r.result = CheckResult::REJECT;
        r.value = value;
        r.limit = maxOrderValue;
        r.reason = "order_value";
    }
    return r;
}

RiskLimitsEx::CheckReport RiskLimitsEx::checkClearlyErroneous(double price, double refPrice) const {
    CheckReport r;
    if (refPrice <= 0) return r;
    double pct = std::abs(price / refPrice - 1.0);
    if (pct > clearlyErroneousPercent) {
        r.result = CheckResult::REJECT;
        r.value = pct * 100;
        r.limit = clearlyErroneousPercent * 100;
        r.reason = "clearly_erroneous";
    }
    return r;
}

RiskLimitsEx::CheckReport RiskLimitsEx::checkMinSellPrice(bool isBuy, double price) const {
    CheckReport r;
    if (!isBuy && price < minSellPrice) {
        r.result = CheckResult::REJECT;
        r.value = price;
        r.limit = minSellPrice;
        r.reason = "min_sell_price";
    }
    return r;
}

RiskLimitsEx::CheckReport RiskLimitsEx::checkPosition(int32_t position) const {
    CheckReport r;
    if (std::abs(position) > maxPositionPerOption) {
        r.result = CheckResult::REJECT;
        r.value = static_cast<double>(position);
        r.limit = static_cast<double>(maxPositionPerOption);
        r.reason = "max_position_per_option";
    }
    return r;
}

RiskLimitsEx::CheckReport RiskLimitsEx::checkPriceLimit(double price,
                                                          double upper,
                                                          double lower) const {
    CheckReport r;
    if (price <= 0) return r;                       // nothing to judge
    if (upper > 0 && price > upper + 1e-9) {
        r.result = CheckResult::REJECT;
        r.value = price;
        r.limit = upper;
        r.reason = "price_upper_limit";
        return r;
    }
    if (lower > 0 && price < lower - 1e-9) {
        r.result = CheckResult::REJECT;
        r.value = price;
        r.limit = lower;
        r.reason = "price_lower_limit";
    }
    return r;
}

RiskLimitsEx::CheckReport RiskLimitsEx::checkPreTrade(
    const std::string& /*code*/, bool isBuy,
    double price, uint32_t qty,
    int32_t currentPosition,
    double refPrice) const {

    int32_t signedQty = isBuy ? static_cast<int32_t>(qty) : -static_cast<int32_t>(qty);
    int32_t finalPos = currentPosition + signedQty;

    CheckReport r = checkOrderSize(qty);
    if (r.result != CheckResult::PASS) return r;

    r = checkOrderValue(price, qty);
    if (r.result != CheckResult::PASS) return r;

    r = checkClearlyErroneous(price, refPrice);
    if (r.result != CheckResult::PASS) return r;

    r = checkMinSellPrice(isBuy, price);
    if (r.result != CheckResult::PASS) return r;

    r = checkPosition(finalPos);
    return r;
}

RiskLimitsEx::CheckReport RiskLimitsEx::checkPostTrade(
    int32_t totalPosition, double /*totalExposure*/) const {

    CheckReport r;
    if (std::abs(totalPosition) > maxTotalPosition) {
        r.result = CheckResult::WARN;
        r.value = static_cast<double>(totalPosition);
        r.limit = static_cast<double>(maxTotalPosition);
        r.reason = "max_total_position";
    }
    return r;
}

namespace {

// A2 fix helper: breach -> REJECT (latches panic downstream);
// >=90% of limit -> WARN (early warning without halting).
RiskLimitsEx::CheckReport thresholdCheck(const char* name, double value,
                                          double limit, double warnRatio = 0.9) {
    RiskLimitsEx::CheckReport r;
    if (limit <= 0) return r;                       // disabled
    const double av = std::abs(value);
    if (av > limit) {
        r.result = RiskLimitsEx::CheckResult::REJECT;
        r.value = value;
        r.limit = limit;
        r.reason = name;
    } else if (av >= limit * warnRatio) {
        r.result = RiskLimitsEx::CheckResult::WARN;
        r.value = value;
        r.limit = limit;
        r.reason = name;
    }
    return r;
}

} // anonymous namespace

RiskLimitsEx::CheckReport RiskLimitsEx::checkGreeks(
    double delta, double gamma, double vega, double pnl) const {

    // Loss limit is one-sided (only losses breach)
    if (maxLossPerDay > 0 && pnl < -maxLossPerDay) {
        CheckReport r;
        r.result = CheckResult::REJECT;
        r.value = pnl;
        r.limit = -maxLossPerDay;
        r.reason = "max_loss_per_day";
        return r;
    }
    if (maxLossPerDay > 0 && pnl < -maxLossPerDay * 0.9) {
        CheckReport r;
        r.result = CheckResult::WARN;
        r.value = pnl;
        r.limit = -maxLossPerDay;
        r.reason = "max_loss_per_day";
        return r;
    }

    CheckReport r = thresholdCheck("max_delta", delta, maxDelta);
    if (r.result != CheckResult::PASS) return r;

    r = thresholdCheck("max_gamma", gamma, maxGamma);
    if (r.result != CheckResult::PASS) return r;

    r = thresholdCheck("max_vega", vega, maxVega);
    return r;
}

} // namespace wt_option
