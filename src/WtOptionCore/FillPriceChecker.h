#pragma once
/*!
 * \file FillPriceChecker.h
 * \brief Fill-vs-issue price deviation policy (plan 3.3).
 *
 * Formerly a standalone per-order table fed in parallel with
 * OrderAnomalyGuard (now absorbed into IssuedOrderRegistry) from the same
 * callbacks.  It is now a pure policy over the single IssuedOrderRegistry:
 * the strategy calls onFill() only for live-order fills, passing the issue
 * price read from the registry record.  The B13 panic latch (survives until
 * explicitly cleared) is unchanged.
 */

#include "IssuedOrderRegistry.h"

#include <cstdint>
#include <string>
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

    /// Deviation check for one fill of a LIVE order.  Callers obtain
    /// issuePx from IssuedOrderRegistry::get(localid) and must skip fills
    /// classified LateAfterDone/Unknown (the old erase-on-done semantics).
    void onFill(const std::string& code, double fillPx, double issuePx);

    void setWarningCallback(WarningCallback cb) { m_warnCb = std::move(cb); }
    void setPanicCallback(PanicCallback cb) { m_panicCb = std::move(cb); }

    /// B13: latched panic state — survives until explicitly cleared, so the
    /// per-compute pricer-panic overwrite can no longer silently reset it.
    bool isPanicked() const { return m_panicked; }
    void clearPanic() { m_panicked = false; }

private:
    Config m_cfg;
    WarningCallback m_warnCb;
    PanicCallback m_panicCb;
    bool m_panicked = false;
};

using FillPriceCheckerPtr = std::shared_ptr<FillPriceChecker>;

} // namespace wt_option
