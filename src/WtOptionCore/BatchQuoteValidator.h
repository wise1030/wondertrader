/*!
 * \file BatchQuoteValidator.h
 * \brief Whole-batch pre-send validation (P1 batch-4.2).
 *
 * The existing risk gates are PER QUOTE: the pricer caps each quote's size by
 * per-expiry position limits, OQM's filter chain checks one order at a time.
 * What none of them sees is the BATCH: in one refresh cycle we can stage NEW
 * quotes on dozens of strikes, and if a shock move fills them all at once the
 * per-expiry delta / short count jumps far beyond what any single-order check
 * would allow — before PositionGuard's next evaluation even runs.
 *
 * This validator aggregates the worst-case simultaneous-fill impact of every
 * pure-NEW open leg in the pending batch, per expiry, and vetoes the
 * lowest-priority legs until the batch fits:
 *   - delta add per expiry   <= max_exp_delta_add
 *   - short add per expiry   <= max_exp_short_add   (sell legs, worst-case
 *     assumed opening; conservative by design)
 *   - portfolio post-batch delta vs RiskLimitsEx::maxDelta (optional)
 *
 * Veto semantics: only legs that would OPEN a new order (no live order on
 * that side) are candidates — updates to existing orders and cancels are
 * never touched, so the guard can only remove risk, never add or churn it.
 *
 * Pure logic, no WT dependencies — unit-testable.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wt_option {

struct BatchLeg {
    std::string code;
    uint32_t expiry = 0;
    double  deltaPerLot = 1.0;   // option: |delta|; future: 1.0
    bool    isBuy = true;        // true = bid leg (long on fill)
    uint32_t qty = 0;
    int32_t rank = 0;            // higher = more urgent (kept longest)
};

struct BatchValidateConfig {
    bool   enable = false;           // master switch (gray release)
    double maxExpDeltaAdd = 0.0;     // per-expiry worst-case |delta| add cap (0 = off)
    uint32_t maxExpShortAdd = 0;     // per-expiry worst-case short-count add cap (0 = off)
    double maxPortfolioDelta = 0.0;  // post-batch |portfolio delta| cap (0 = off)
};

struct BatchValidateResult {
    // Legs to drop (same index space as the input vector).
    std::vector<size_t> dropped;
    // Human-readable reason per dropped leg (parallel to dropped).
    std::vector<std::string> reasons;
    bool anyDropped() const { return !dropped.empty(); }
};

class BatchQuoteValidator {
public:
    /// currentPortfolioDelta: portfolio delta BEFORE this batch fills.
    using DeltaProvider = std::function<double()>;

    BatchQuoteValidator(BatchValidateConfig cfg, DeltaProvider deltaProvider = nullptr)
        : m_cfg(cfg), m_deltaProvider(std::move(deltaProvider)) {}

    /// Greedy fit: aggregate all legs per expiry; while any cap is breached,
    /// drop the lowest-rank leg of the breaching expiry. Deterministic.
    BatchValidateResult validate(const std::vector<BatchLeg>& legs) const {
        BatchValidateResult res;
        if (!m_cfg.enable) return res;

        struct Acc { double deltaAdd = 0; uint32_t shortAdd = 0; };
        // expiry -> accumulator; index -1 reserved for the portfolio total.
        std::vector<Acc> acc;                 // parallel to legs (per-leg expiry acc refs)
        // We aggregate per expiry: map expiry -> running totals.
        // Simple O(n^2) is fine (n = pending quotes, a few hundred at most).
        std::vector<char> dropped(legs.size(), 0);

        auto expiryTotals = [&](uint32_t exp, double& dAdd, uint32_t& sAdd) {
            dAdd = 0; sAdd = 0;
            for (size_t i = 0; i < legs.size(); ++i) {
                if (dropped[i] || legs[i].expiry != exp) continue;
                const auto& L = legs[i];
                dAdd += (L.isBuy ? 1.0 : -1.0) * L.deltaPerLot * (double)L.qty;
                if (!L.isBuy) sAdd += L.qty;
            }
        };

        auto fits = [&](uint32_t exp, double dAdd, uint32_t sAdd) {
            if (m_cfg.maxExpDeltaAdd > 0 && std::abs(dAdd) > m_cfg.maxExpDeltaAdd)
                return false;
            if (m_cfg.maxExpShortAdd > 0 && sAdd > m_cfg.maxExpShortAdd)
                return false;
            return true;
        };

        // Iterate until every expiry fits (each drop only reduces totals).
        for (int guard = 0; guard < (int)legs.size() + 1; ++guard) {
            // Collect distinct expiries still contributing.
            std::vector<uint32_t> exps;
            for (size_t i = 0; i < legs.size(); ++i) {
                if (dropped[i]) continue;
                if (std::find(exps.begin(), exps.end(), legs[i].expiry) == exps.end())
                    exps.push_back(legs[i].expiry);
            }

            uint32_t breachExp = 0;
            double breachD = 0; uint32_t breachS = 0;
            bool found = false;
            for (uint32_t e : exps) {
                double d; uint32_t s;
                expiryTotals(e, d, s);
                if (!fits(e, d, s)) { breachExp = e; breachD = d; breachS = s; found = true; break; }
            }
            if (!found) break;   // everything fits

            // Drop the lowest-rank live leg of the breaching expiry.
            int worst = -1;
            for (size_t i = 0; i < legs.size(); ++i) {
                if (dropped[i] || legs[i].expiry != breachExp) continue;
                if (worst < 0 || legs[i].rank < legs[worst].rank) worst = (int)i;
            }
            if (worst < 0) break;   // nothing left to drop (shouldn't happen)
            dropped[worst] = 1;
            char buf[160];
            snprintf(buf, sizeof(buf),
                "exp=%u %s deltaAdd=%.2f shortAdd=%u",
                breachExp, legs[worst].isBuy ? "bid" : "ask", breachD, breachS);
            res.dropped.push_back((size_t)worst);
            res.reasons.emplace_back(buf);
        }

        // Optional portfolio-level check vs RiskLimitsEx::maxDelta semantics.
        if (m_cfg.maxPortfolioDelta > 0 && m_deltaProvider) {
            double dAdd = 0;
            for (size_t i = 0; i < legs.size(); ++i) {
                if (dropped[i]) continue;
                const auto& L = legs[i];
                dAdd += (L.isBuy ? 1.0 : -1.0) * L.deltaPerLot * (double)L.qty;
            }
            const double after = m_deltaProvider() + dAdd;
            if (std::abs(after) > m_cfg.maxPortfolioDelta) {
                // Drop lowest-rank legs globally until the portfolio fits.
                for (int guard = 0; guard < (int)legs.size() + 1; ++guard) {
                    double add = 0;
                    for (size_t i = 0; i < legs.size(); ++i) {
                        if (dropped[i]) continue;
                        const auto& L = legs[i];
                        add += (L.isBuy ? 1.0 : -1.0) * L.deltaPerLot * (double)L.qty;
                    }
                    if (std::abs(m_deltaProvider() + add) <= m_cfg.maxPortfolioDelta) break;
                    int worst = -1;
                    for (size_t i = 0; i < legs.size(); ++i) {
                        if (dropped[i]) continue;
                        if (worst < 0 || legs[i].rank < legs[worst].rank) worst = (int)i;
                    }
                    if (worst < 0) break;
                    dropped[worst] = 1;
                    char buf[160];
                    snprintf(buf, sizeof(buf),
                        "portfolio deltaAdd=%.2f after=%.2f cap=%.2f",
                        add, m_deltaProvider() + add, m_cfg.maxPortfolioDelta);
                    res.dropped.push_back((size_t)worst);
                    res.reasons.emplace_back(buf);
                }
            }
        }

        // Deterministic order for callers that replay drops sequentially
        // (reasons stay paired with their leg).
        std::vector<std::pair<size_t, std::string>> paired;
        paired.reserve(res.dropped.size());
        for (size_t i = 0; i < res.dropped.size(); ++i)
            paired.emplace_back(res.dropped[i], res.reasons[i]);
        std::sort(paired.begin(), paired.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        res.dropped.clear(); res.reasons.clear();
        for (auto& p : paired) { res.dropped.push_back(p.first); res.reasons.push_back(p.second); }
        return res;
    }

private:
    BatchValidateConfig m_cfg;
    DeltaProvider m_deltaProvider;
};

} // namespace wt_option
