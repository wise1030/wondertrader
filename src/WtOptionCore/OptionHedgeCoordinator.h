/*!
 * \file OptionHedgeCoordinator.h
 * \brief Phase 2 seam: portfolio delta-hedge decision + in-flight ownership.
 *
 * Extracted from HftOptionStrategy::checkDeltaHedge. The strategy keeps config
 * and price/order hooks; the coordinator owns the pure decision, cooldown,
 * in-flight suppression and terminal bookkeeping (unit-testable without WT).
 */
#pragma once

#include "DeltaHedgeExecutor.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>

namespace wt_option {

class OptionHedgeCoordinator {
public:
    using SendFn = std::function<int32_t(const std::string& code, bool isBuy,
                                         double price, uint32_t qty)>;
    using PriceFn = std::function<double(const std::string& code)>;
    using LogFn = std::function<void(const std::string& message)>;

    void configure(const DeltaHedgeExecutor::Config& cfg, double deltaPerLot,
                   std::string hedgeCode, SendFn send, PriceFn price,
                   LogFn log = {});
    void setHedgeCode(std::string code);
    void setDeltaPerLot(double v);
    const DeltaHedgeExecutor::Config& config() const { return m_cfg; }

    /// Returns true iff a hedge order was sent. `portfolioDelta` is the
    /// multiplier-scaled portfolio delta.
    bool check(double nowSec, double portfolioDelta);

    /// Terminal callback for any order id (cancel/reject/full fill).
    void onTerminal(uint32_t localid);

    bool hasInflight() const;
    double lastFireTime() const { return m_lastFire; }

private:
    DeltaHedgeExecutor::Config m_cfg;
    std::string m_code;
    SendFn m_send;
    PriceFn m_price;
    LogFn m_log;

    mutable std::mutex m_mtx;
    std::unordered_set<uint32_t> m_inflight;
    double m_lastFire = -1e9;
};

} // namespace wt_option
