#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>

namespace wt_option {

class FillPriceChecker {
public:
    struct Config {
        double warningThreshold = 0.0025;
        double panicThreshold = 0.005;
        Config() = default;
    };

    using WarningCallback = std::function<void(const std::string& code, double fillPx,
                                                double issuePx, double pct)>;
    using PanicCallback = std::function<void(const std::string& code, double fillPx,
                                             double issuePx, double pct)>;

    FillPriceChecker() : m_cfg() {}
    explicit FillPriceChecker(const Config& cfg) : m_cfg(cfg) {}

    void onOrderSent(const std::string& code, uint32_t localid, double price);
    void onFill(const std::string& code, uint32_t localid, double fillPx);
    void onOrderCancelled(uint32_t localid);
    /// Clear tracking entry when an order is fully filled or otherwise done
    /// (prevents unbounded growth of m_issuePrices — B14)
    void onOrderCompleted(uint32_t localid) { m_issuePrices.erase(localid); }

    void setWarningCallback(WarningCallback cb) { m_warnCb = std::move(cb); }
    void setPanicCallback(PanicCallback cb) { m_panicCb = std::move(cb); }

    /// B13: latched panic state — survives until explicitly cleared, so the
    /// per-compute pricer-panic overwrite can no longer silently reset it.
    bool isPanicked() const { return m_panicked; }
    void clearPanic() { m_panicked = false; }

private:
    Config m_cfg;
    struct IssueInfo {
        std::string code;
        double price = 0;
    };
    std::unordered_map<uint32_t, IssueInfo> m_issuePrices;
    WarningCallback m_warnCb;
    PanicCallback m_panicCb;
    bool m_panicked = false;
};

using FillPriceCheckerPtr = std::shared_ptr<FillPriceChecker>;

} // namespace wt_option
