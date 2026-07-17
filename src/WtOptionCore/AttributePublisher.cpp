#include "AttributePublisher.h"
#include "OptionData.h"
#include "OptionTradingData.h"
#include "UnderlyingTradingData.h"
#include "OptionRisk.h"
#include "OptionGrid.h"
#include "../WTSTools/WTSLogger.h"
#include "../Share/fmtlib.h"

#include <cmath>
#include <algorithm>

namespace wt_option {

void AttributePublisher::collectOption(const std::string& code,
    OptionTradingData* otd, const OptionData* od)
{
    if (!od) return;
    OptionAttrs& a = m_optAttrs[code];
    a.priced = od->values(0).isPriced();
    a.delta = od->greeks().delta();
    a.theo = od->values(0).theo();
    a.impliedVol = od->values(0).impliedVol();
    a.fwd = od->values(0).forward();
    a.mbid = od->getBid();
    a.mask = od->getAsk();
    if (otd) {
        a.enabled = otd->isActive();
        a.position = otd->getPosition();
        const MultiMarket& our = otd->multiMarket();
        a.obid = our.getBestBid().px();
        a.oask = our.getBestAsk().px();
        a.obid_sz = our.getBestBid().sz();
        a.oask_sz = our.getBestAsk().sz();
    }
    m_dirty = true;
}

void AttributePublisher::collectUnderlying(const std::string& code,
    UnderlyingTradingData* utd)
{
    if (!utd) return;
    UnderlyingAttrs& a = m_undAttrs[code];
    a.enabled = utd->isActive();
    a.position = utd->getPosition();
    a.mbid = utd->getBid();
    a.mask = utd->getAsk();
    a.fwd = utd->getFwd();
    const MultiMarket& our = utd->ourMarket();
    a.obid = our.getBestBid().px();
    a.oask = our.getBestAsk().px();
    m_dirty = true;
}

bool AttributePublisher::publish(double now)
{
    if (!m_dirty) return false;
    if ((now - m_lastPublishTime) < m_publishInterval) return false;

    m_lastPublishTime = now;
    m_dirty = false;

    // B17: Count active instruments and sides
    m_activeOptions = 0;
    m_activeFutures = 0;
    m_activeSides = 0;

    for (const auto& [code, a] : m_optAttrs) {
        if (a.enabled) {
            m_activeOptions++;
            if (a.obid > 0) m_activeSides++;
            if (a.oask > 0) m_activeSides++;
        }
    }
    for (const auto& [code, a] : m_undAttrs) {
        if (a.enabled) {
            m_activeFutures++;
            if (a.obid > 0) m_activeSides++;
            if (a.oask > 0) m_activeSides++;
        }
    }

    // Portfolio risk summary
    double portDelta = 0, portVega = 0, portGamma = 0;
    if (m_risk) {
        const OptionGreeks& g = *m_risk->getPositionGreeks();
        portDelta = g.delta();
        portVega = g.vega();
        portGamma = g.gamma();
    }

    // Publish summary via structured log
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "ATTRS: opt={} fut={} sides={} delta={:.2f} vega={:.2f} gamma={:.6f}",
        m_activeOptions, m_activeFutures, m_activeSides,
        portDelta, portVega, portGamma);

    // Publish per-option attributes (only if priced and enabled)
    for (const auto& [code, a] : m_optAttrs) {
        if (!a.priced) continue;
        WTSLogger::log_by_cat("strategy", LL_DEBUG,
            "OPT {} d={:.3f} pos={} theo={:.4f} iv={:.4f} "
            "mb={:.4f}/{:.4f} ob={:.4f}/{:.4f}({}/{})",
            code, a.delta, static_cast<int>(a.position), a.theo, a.impliedVol,
            a.mbid, a.mask, a.obid, a.oask, a.obid_sz, a.oask_sz);
    }

    // Publish per-underlying attributes
    for (const auto& [code, a] : m_undAttrs) {
        if (!a.enabled) continue;
        WTSLogger::log_by_cat("strategy", LL_DEBUG,
            "UND {} pos={} mb={:.4f}/{:.4f} ob={:.4f}/{:.4f} fwd={:.4f}",
            code, static_cast<int>(a.position), a.mbid, a.mask,
            a.obid, a.oask, a.fwd);
    }

    return true;
}

void AttributePublisher::publishSingle(const std::string& code, bool isOption)
{
    if (isOption) {
        auto it = m_optAttrs.find(code);
        if (it == m_optAttrs.end()) return;
        const OptionAttrs& a = it->second;
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "OPT {} d={:.3f} pos={} theo={:.4f} iv={:.4f} "
            "mb={:.4f}/{:.4f} ob={:.4f}/{:.4f}({}/{})",
            code, a.delta, static_cast<int>(a.position), a.theo, a.impliedVol,
            a.mbid, a.mask, a.obid, a.oask, a.obid_sz, a.oask_sz);
    } else {
        auto it = m_undAttrs.find(code);
        if (it == m_undAttrs.end()) return;
        const UnderlyingAttrs& a = it->second;
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "UND {} pos={} mb={:.4f}/{:.4f} ob={:.4f}/{:.4f} fwd={:.4f}",
            code, static_cast<int>(a.position), a.mbid, a.mask,
            a.obid, a.oask, a.fwd);
    }
}

void AttributePublisher::refresh()
{
    m_dirty = true;
}

} // namespace wt_option
