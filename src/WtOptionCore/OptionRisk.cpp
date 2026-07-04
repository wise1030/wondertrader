/*!
 * \file OptionRisk.cpp
 * \brief Portfolio risk / Greeks aggregator (migrated from quantbox)
 *
 * Business logic preserved. Migration deltas documented in OptionRisk.h.
 *
 * NOTE on OptionGrid API:
 * The OptionGrid is not yet migrated into the main WtOptionCore directory
 * (only _trash/simplified_v1/OptionGrid.h exists, with a different surface).
 * Calls to grid->subscribe(...), grid->expired(...), grid->getExpiryData(...),
 * grid->getSpotTradingData(), and ExpiryData::getPrimaryHedge()/
 * getSecondaryUnderliers() are guarded with TODO[OptionGrid migration] markers.
 * When OptionGrid lands, replace the stub bodies with the real calls.
 */
#include "OptionRisk.h"
#include "OptionData.h"
#include "ExpiryData.h"
#include "OptionExpiryGreeks.h"
#include "OptionRiskData.h"
#include "OptionGrid.h"

#include <iostream>
#include <memory>

namespace wt_option {

/////////////////////////////////////////////////////////////////
// OptionRisk

OptionRisk::OptionRisk(const OptionGridPtr& grid)
    : m_spOptionGrid(grid)
    , m_spPositionGreeks(std::make_shared<OptionGreeks>())
    , m_bAutoUpdateGreeks(true)
    , m_pfuDelta(0)
{
    if (!grid) return;

    // Iterate all current options and create risk data entries.
    // TODO[OptionGrid migration]: confirm getAllOptions() returns
    // const std::vector<OptionDataPtr>&. The simplified_v1 grid exposes it.
    for (const OptionDataPtr& od : grid->getAllOptions())
    {
        createOptionRiskData(od);
    }

    // Register self as grid listener (replaces grid->subscribe(Subscription, this)).
    // TODO[OptionGrid migration]: grid->addListener(this) once OptionGrid lands.
    // For now we rely on the engine calling onComputeValuesCompleted() externally
    // or wiring the listener post-construction.
    // grid->addListener(this);
}

OptionRiskDataPtr OptionRisk::createOptionRiskData(const OptionDataPtr& od)
{
    if (!od) return nullptr;

    // TODO[OptionGrid migration]: original called getOptionGrid()->expired(instr)
    // to skip expired contracts. No expired() on the simplified grid; rely on
    // ExpiryData::daysToExpiry() <= 0 once that is the convention. For now,
    // create unconditionally (matches simplified_v1 behavior).
    // if (getOptionGrid()->expired(od->getCode())) return nullptr;

    auto rd = std::make_shared<OptionRiskData>(od);
    m_optionList.insert(rd);

    const OptionExpiryGreeksPtr& egreeks = getExpiryGreeks(od->getExpiry());
    egreeks->registerOptionRiskData(rd);

    return rd;
}

OptionRiskDataPtr OptionRisk::get(const std::string& instr)
{
    const by_instr& index = m_optionList.get<0>();
    by_instr::const_iterator it = index.find(instr);
    if (it == index.end())
    {
        // TODO[OptionGrid migration]: original called m_spOptionGrid->get(instr)
        // which may trigger a create. The simplified grid exposes getOption(code).
        // For now, if not in our list, we cannot lazily create without the grid
        // returning an OptionDataPtr by code — wire this once OptionGrid lands.
        return OptionRiskDataPtr();
    }
    return *it;
}

const OptionExpiryGreeksPtr& OptionRisk::getExpiryGreeks(uint32_t exp)
{
    OptionExpiryGreeksPtr& egreeks = m_expiryTable[exp];
    if (!egreeks)
    {
        // TODO[OptionGrid migration]: original pulled ExpiryData via
        // m_spOptionGrid->getExpiryData(exp). The simplified grid exposes
        // getExpiry(exp). Wire once OptionGrid lands.
        ExpiryDataPtr ed;
        // if (m_spOptionGrid) ed = m_spOptionGrid->getExpiry(exp);

        egreeks = std::make_shared<OptionExpiryGreeks>(ed);

        // TODO[OptionGrid migration]: original registered primary + secondary
        // hedge instruments from ExpiryData::getPrimaryHedge() and
        // getSecondaryUnderliers() (longbeach MarketDataContext types that no
        // longer exist). Once ExpiryData exposes hedge codes directly, do:
        //   if (ed && !ed->getHedgeCode().empty())
        //       egreeks->setPrimaryHedge(registerHedgeInstrument(ed->getHedgeCode(), exp));
        // For now, hedge wiring is deferred to the engine.

        // Subscribe to per-expiry Greeks / underlier-delta change callbacks
        // (replaces Subscription + boost::bind).
        OptionRisk* self = this;
        egreeks->addGreeksChangedCallback(
            [self](const OptionExpiryGreeks& g, const OptionGreeks& prev)
            { self->__onExpiryGreeksChanged(g, prev); });
        egreeks->addUDeltaChangedCallback(
            [self](const OptionExpiryGreeks& g, double prev)
            { self->__onUndDeltaChanged(g, prev); });
    }
    return egreeks;
}

OptionRisk::HedgeDataPtr OptionRisk::registerHedgeInstrument(const std::string& code,
                                                              uint32_t exp)
{
    for (const HedgeDataPtr& hd : m_hedgeDataList)
    {
        if (hd->code == code && hd->expiry == exp)
            return hd;
    }
    std::cout << "OptionRisk::registerHedgeInstrument " << code << std::endl;
    auto hd = std::make_shared<HedgeData>();
    hd->code          = code;
    hd->expiry        = exp;
    hd->position      = 0;
    hd->deltaPosition = 0.0;
    hd->multiplier    = 1.0;
    m_hedgeDataList.push_back(hd);
    return hd;
}

void OptionRisk::setHedgePosition(const std::string& code, int32_t position)
{
    for (HedgeDataPtr& hd : m_hedgeDataList)
    {
        if (hd->code == code)
        {
            hd->position = position;
            // deltaPosition = multiplier * settlementFraction * position
            // The original HedgeData computed this as
            //   m_multiplier * m_spExpiryData->getSettlementFraction() * position
            // We don't carry an ExpiryData pointer here; engine is expected to
            // set deltaPosition directly via hd->deltaPosition if it needs the
            // fraction applied. We update the simple product as a fallback.
            hd->deltaPosition = hd->multiplier * position;
            return;
        }
    }
}

void OptionRisk::update()
{
    m_spPositionGreeks->reset();
    double udelta = 0;

    for (const ExpiryTable::value_type& v : m_expiryTable)
    {
        const OptionExpiryGreeksPtr& eg = v.second;
        eg->update();
        m_spPositionGreeks->accum(eg->frac(), *eg);
        m_spPositionGreeks->delta() -= eg->frac()      * eg->delta();
        m_spPositionGreeks->delta() += eg->frac_delta() * eg->delta();

        udelta += (eg->frac_delta() * eg->getUnderlierDelta());
    }
    m_pfuDelta = udelta;

    // If SpotTradingData exists, override underlier delta.
    // TODO[OptionGrid migration]: original:
    //   if (m_spOptionGrid->getSpotTradingData()) {
    //       const SpotTradingDataPtr& std_ = m_spOptionGrid->getSpotTradingData();
    //       m_pfuDelta = std_->getDelta();
    //   }
    // SpotTradingData is not yet migrated; the engine can set m_pfuDelta
    // externally if needed. Leaving the override path as a no-op for now.
}

const OptionRisk::by_instr& OptionRisk::all()
{
    return m_optionList.get<0>();
}

const OptionRisk::by_instr& OptionRisk::all() const
{
    return m_optionList.get<0>();
}

double OptionRisk::getDelta() const
{
    return m_spPositionGreeks->delta();
}

double OptionRisk::getOptionDelta() const
{
    return m_spPositionGreeks->delta();
}

double OptionRisk::getPortfolioDelta() const
{
    return getOptionDelta() + m_pfuDelta;
}

double OptionRisk::getUnderlierDelta() const
{
    double delta = 0;
    for (const ExpiryTable::value_type& v : m_expiryTable)
    {
        const OptionExpiryGreeksPtr& eg = v.second;
        delta += eg->getUnderlierDelta();
    }

    // If SpotTradingData exists, override underlier delta.
    // TODO[OptionGrid migration]: original overrode with SpotTradingData::getDelta().
    // Left as no-op until SpotTradingData lands.
    return delta;
}

const OptionGreeksPtr& OptionRisk::getPositionGreeks() const
{
    return m_spPositionGreeks;
}

const OptionGreeks& OptionRisk::option_pfgreeks() const
{
    return *m_spPositionGreeks;
}

double OptionRisk::totalDelta() const
{
    double raw_delta = 0;
    for (const ExpiryTable::value_type& v : m_expiryTable)
    {
        const OptionExpiryGreeksPtr& eg = v.second;
        raw_delta += eg->totalDelta();
    }
    // TODO[OptionGrid migration]: original added SpotTradingData::getDelta().
    // Left as no-op until SpotTradingData lands.
    return raw_delta;
}

std::vector<OptionRiskDataPtr> OptionRisk::getNonZeroPositions()
{
    std::vector<OptionRiskDataPtr> v;
    if (!m_spOptionGrid) return v;
    // TODO[OptionGrid migration]: confirm getAllOptions() availability.
    for (const OptionDataPtr& od : m_spOptionGrid->getAllOptions())
    {
        OptionRiskDataPtr rd = get(od->getCode());
        if (rd && rd->getPosition() != 0)
            v.push_back(rd);
    }
    return v;
}

std::vector<OptionRiskDataPtr> OptionRisk::getNonZeroPositions(uint32_t exp)
{
    std::vector<OptionRiskDataPtr> v;
    if (!m_spOptionGrid) return v;
    for (const OptionDataPtr& od : m_spOptionGrid->getAllOptions())
    {
        if (od->getExpiry() != exp) continue;
        OptionRiskDataPtr rd = get(od->getCode());
        if (rd && rd->getPosition() != 0)
            v.push_back(rd);
    }
    return v;
}

void OptionRisk::onAddOption(const OptionDataPtr& od)
{
    if (!od) return;
    // TODO[OptionGrid migration]: expired() not available on simplified grid.
    // if (getOptionGrid()->expired(od->getCode())) return;
    if (!m_optionList.get<0>().count(od->getCode()))
        createOptionRiskData(od);
}

void OptionRisk::__onUndDeltaChanged(const OptionExpiryGreeks& g, double prev)
{
    // Incremental m_pfuDelta update on hedge position change.
    m_pfuDelta += g.frac_delta() * (g.getUnderlierDelta() - prev);
}

void OptionRisk::__onExpiryGreeksChanged(const OptionExpiryGreeks& g,
                                          const OptionGreeks& prev)
{
    m_spPositionGreeks->reduce(g.frac(), prev);
    m_spPositionGreeks->accum (g.frac(), g.option_greeks());
    m_spPositionGreeks->delta() -= g.frac()       * g.option_greeks().delta();
    m_spPositionGreeks->delta() += g.frac_delta() * g.option_greeks().delta();
}

void OptionRisk::onComputeValuesCompleted(const IOptionGrid* /*grid*/)
{
    if (m_bAutoUpdateGreeks)
        update();
}

} // namespace wt_option
