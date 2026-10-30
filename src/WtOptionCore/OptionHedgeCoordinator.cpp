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
    if (!m_send || !m_price || m_code.empty()) return false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_inflight.empty()) return false;   // one batch at a time
    }
    if (!(m_cfg.deltaPerLot > 0)) return false;

    HedgeDecision d = DeltaHedgeExecutor::decide(m_cfg, portfolioDelta,
                                                 nowSec, m_lastFire);
    if (!d.fire) return false;

    const double px = m_price(m_code);
    if (!(px > 0)) return false;

    const std::vector<uint32_t> ids = m_send(m_code, d.isBuy, px, d.qty);
    if (ids.empty()) return false;

    bool insertedAny = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (uint32_t id : ids) {
            if (id != 0) {
                m_inflight.insert(id);
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

} // namespace wt_option
