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

RiskLimitsEx::CheckReport RiskLimitsEx::checkGreeks(
    double delta, double gamma, double vega, double pnl) const {

    CheckReport r;
    if (std::abs(delta) > maxDelta) {
        r.result = CheckResult::WARN;
        r.value = delta;
        r.limit = maxDelta;
        r.reason = "max_delta";
        return r;
    }
    if (std::abs(gamma) > maxGamma) {
        r.result = CheckResult::WARN;
        r.value = gamma;
        r.limit = maxGamma;
        r.reason = "max_gamma";
        return r;
    }
    if (std::abs(vega) > maxVega) {
        r.result = CheckResult::WARN;
        r.value = vega;
        r.limit = maxVega;
        r.reason = "max_vega";
        return r;
    }
    if (pnl < -maxLossPerDay) {
        r.result = CheckResult::WARN;
        r.value = pnl;
        r.limit = -maxLossPerDay;
        r.reason = "max_loss_per_day";
        return r;
    }
    return r;
}

} // namespace wt_option
