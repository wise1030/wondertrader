#include "OptionHedgeCoordinator.h"

#include <cmath>

namespace wt_option {

void OptionHedgeCoordinator::configure(const DeltaHedgeExecutor::Config& cfg,
                                       double deltaPerLot, std::string hedgeCode,
                                       SendFn send, PriceFn price, LogFn log)
{
    m_cfg = cfg;
    m_cfg.deltaPerLot = deltaPerLot;
    m_code = std::move(hedgeCode);
    m_send = std::move(send);
    m_price = std::move(price);
    m_log = std::move(log);
}

void OptionHedgeCoordinator::setHedgeCode(std::string code)
{
    m_code = std::move(code);
}

void OptionHedgeCoordinator::setDeltaPerLot(double v)
{
    m_cfg.deltaPerLot = v;
}

bool OptionHedgeCoordinator::check(double nowSec, double portfolioDelta)
{
    return check(m_cfg, nowSec, portfolioDelta, 0);
}

bool OptionHedgeCoordinator::fireLeg(int legIdx, const std::string& code,
                                     double netDelta, double nowSec,
                                     double priceOverride)
{
    if (!m_send || !m_price || code.empty()) return false;
    if (!(m_cfg.deltaPerLot > 0)) return false;

    HedgeDecision d = DeltaHedgeExecutor::decide(m_cfg, netDelta, nowSec,
                                                 legIdx < 0 ? m_sharedLastFire
                                                            : m_legLastFire[legIdx]);
    if (!d.fire) return false;

    const double px = (priceOverride > 0) ? priceOverride : m_price(code);
    if (!(px > 0)) return false;

    const std::vector<uint32_t> ids = m_send(code, d.isBuy, px, d.qty);
    if (ids.empty()) return false;

    bool insertedAny = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (uint32_t id : ids) {
            if (id != 0) {
                // delta this order will add once filled (sign included)
                const double tgt = (d.isBuy ? 1.0 : -1.0) * d.qty * m_cfg.deltaPerLot;
                m_inflight.emplace(id, InflightRec{legIdx, tgt});
                insertedAny = true;
            }
        }
    }
    if (!insertedAny) return false;

    if (legIdx < 0) m_sharedLastFire = nowSec;
    else            m_legLastFire[legIdx] = nowSec;
    m_lastFire = nowSec;
    if (m_log) {
        m_log("DeltaHedger fired: " + code + (d.isBuy ? " BUY " : " SELL ")
              + std::to_string(d.qty) + "@" + std::to_string(px)
              + " (" + d.reason + ") ids=" + std::to_string(ids.size()));
    }
    return true;
}

size_t OptionHedgeCoordinator::checkMulti(double nowSec,
                                          const ExpDeltaFn& expDeltaFn,
                                          const ExpDeltaFn& portfolioNetFn)
{
    if (m_legs.empty())
        return check(nowSec, portfolioNetFn(0)) ? 1 : 0;

    if (m_legLastFire.size() != m_legs.size())
        m_legLastFire.assign(m_legs.size(), -1e9);

    size_t fired = 0;
    for (size_t i = 0; i < m_legs.size(); ++i) {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            bool legInflight = false;
            for (const auto& kv : m_inflight) {
                if (kv.second.legIdx == static_cast<int>(i)) { legInflight = true; break; }
            }
            if (legInflight) continue;
        }
        const double d = expDeltaFn(m_legs[i].exp);
        if (fireLeg(static_cast<int>(i), m_legs[i].code, d, nowSec))
            ++fired;
    }

    // Shared residual leg: portfolio net minus the target delta of every
    // exclusive leg's in-flight order — the two tiers never double-hedge.
    // Compute under the lock, fire OUTSIDE it (fireLeg re-locks).
    if (!m_sharedCode.empty()) {
        bool skip = false;
        double residual = 0.0;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            for (const auto& kv : m_inflight) {
                if (kv.second.legIdx < 0) { skip = true; break; }
            }
            if (!skip) {
                residual = portfolioNetFn(0);
                for (const auto& kv : m_inflight)
                    residual -= kv.second.targetDelta;
            }
        }
        if (!skip && fireLeg(-1, m_sharedCode, residual, nowSec))
            ++fired;
    }
    return fired;
}

bool OptionHedgeCoordinator::check(const DeltaHedgeExecutor::Config& cfg,
                                   double nowSec, double portfolioDelta,
                                   double priceOverride)
{
    if (!m_send || !m_price || m_code.empty()) return false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_inflight.empty()) return false;   // one batch at a time
    }
    if (!(cfg.deltaPerLot > 0)) return false;

    HedgeDecision d = DeltaHedgeExecutor::decide(cfg, portfolioDelta,
                                                 nowSec, m_lastFire);
    if (!d.fire) return false;

    const double px = (priceOverride > 0) ? priceOverride : m_price(m_code);
    if (!(px > 0)) return false;

    const std::vector<uint32_t> ids = m_send(m_code, d.isBuy, px, d.qty);
    if (ids.empty()) return false;

    bool insertedAny = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (uint32_t id : ids) {
            if (id != 0) {
                m_inflight.emplace(id, InflightRec{-1, 0.0});
                insertedAny = true;
            }
        }
    }
    if (!insertedAny) return false;

    m_lastFire = nowSec;
    if (m_log) {
        m_log("DeltaHedger fired: " + m_code + (d.isBuy ? " BUY " : " SELL ")
              + std::to_string(d.qty) + "@" + std::to_string(px)
              + " (" + d.reason + ") ids=" + std::to_string(ids.size()));
    }
    return true;
}

void OptionHedgeCoordinator::onTerminal(uint32_t localid)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_inflight.erase(localid);
}

bool OptionHedgeCoordinator::hasInflight() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return !m_inflight.empty();
}

std::vector<uint32_t> OptionHedgeCoordinator::inflightIds() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    std::vector<uint32_t> ids;
    ids.reserve(m_inflight.size());
    for (const auto& kv : m_inflight) ids.push_back(kv.first);
    return ids;
}

} // namespace wt_option
