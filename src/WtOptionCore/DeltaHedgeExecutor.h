/*!
 * \file DeltaHedgeExecutor.h
 * \brief Portfolio delta-hedge executor — BorealisHedger-inspired (quantbox
 *        mm/pricer), compact form for the composite MM path.
 *
 * R10: OptionRisk computes portfolio delta but nobody executed it. This
 * executor fires an offsetting hedge order on the primary hedge instrument
 * when |portfolioDelta| breaches a threshold, with cooldown and size cap.
 * Default OFF (`delta_hedge.enable=false`); orders flow through the strategy
 * executor bridge so price-limit guard + anomaly registration apply for free.
 *
 * The decision logic is pure and unit-testable; the strategy supplies prices.
 */
#pragma once

#include <cstdint>
#include <string>
#include <cmath>

namespace wt_option {

struct HedgeDecision {
    bool   fire = false;
    bool   isBuy = false;      // direction of the HEDGE order
    uint32_t qty = 0;
    std::string reason;
};

class DeltaHedgeExecutor {
public:
    struct Config {
        double thresholdDelta = 0.0;    // fire when |portfolio delta| > this (0=off)
        int32_t  targetDelta = 0;       // recenter to this (usually 0)
        uint32_t maxOrderSize = 5;
        double cooldownSec = 3.0;
        // Delta of one hedge lot (futures: contract multiplier × |Δ_fut|, usually the multiplier).
        // Keep this explicit: `portfolioDelta` is already multiplied by contract size, so a
        // default of 1.0 would over-hedge by the multiplier (e.g. IO ≈ 300x).
        double deltaPerLot = 1.0;
        Config() = default;
    };

    explicit DeltaHedgeExecutor(const Config& cfg) : m_cfg(cfg) {}

    /// Pure decision. `now` from the injected clock; `lastFireTime` updated
    /// by the caller when an order is actually sent.
    static HedgeDecision decide(const Config& cfg, double portfolioDelta,
                                double nowSec, double lastFireTime) {
        HedgeDecision d;
        if (cfg.thresholdDelta <= 0) return d;

        const double excess = portfolioDelta - cfg.targetDelta;
        if (std::fabs(excess) < cfg.thresholdDelta) return d;
        if ((nowSec - lastFireTime) < cfg.cooldownSec) {
            d.reason = "cooldown";
            d.fire = false;
            return d;
        }

        // Hedge direction opposes excess long delta → sell
        d.isBuy = excess < 0;
        // Lot count: |excess| / delta-per-lot (futures ≈ multiplier), rounded UP
        // so we never stay under-hedged.
        const double lotsRaw = std::fabs(excess) / std::max(cfg.deltaPerLot, 1e-9);
        uint32_t lots = static_cast<uint32_t>(std::ceil(lotsRaw - 1e-9));
        if (lots == 0) return d;
        if (lots > cfg.maxOrderSize) lots = cfg.maxOrderSize;
        d.qty = lots;
        d.fire = true;
        d.reason = "delta_excess=" + std::to_string(excess);
        return d;
    }

    const Config& config() const { return m_cfg; }

private:
    Config m_cfg;
};

} // namespace wt_option
