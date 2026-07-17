/*!
 * \file OptionRisk.cpp
 * \brief Portfolio risk / Greeks aggregator (migrated from quantbox)
 *
 * Business logic preserved. Migration deltas documented in OptionRisk.h.
 *
 * NOTE on OptionGrid wiring:
 * OptionGrid is fully migrated. Grid-event delivery (onComputeValuesCompleted)
 * is wired externally by the strategy via grid->addListener(risk.get()) so the
 * constructor does NOT self-register (that would double-fire). Expired-contract
 * filtering uses ExpiryData::daysToExpiry() <= 0. The only genuinely
 * unavailable dependency is SpotTradingData (spot leg override of underlier
 * delta), which is not part of the migrated grid; that override path is left as
 * a documented no-op and can be set externally via the engine if needed.
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

namespace {
// A contract is considered expired once its expiry has no calendar days left.
// ExpiryData::daysToExpiry() defaults to 0 before the calendar is computed, so
// we only treat it as expired when strictly negative to avoid dropping
// freshly-created contracts whose calendar has not yet been populated.
bool isExpired(const OptionGridPtr& grid, const OptionDataPtr& od)
{
    if (!grid || !od) return false;
    ExpiryDataPtr ed = grid->getExpiryData(od->getExpiry());
    if (!ed) return false;
    return ed->daysToExpiry() < 0;
}
} // anonymous namespace

/////////////////////////////////////////////////////////////////
// OptionRisk

OptionRisk::OptionRisk(const OptionGridPtr& grid)
    : m_spOptionGrid(grid)
    , m_spPositionGreeks(std::make_shared<OptionGreeks>())
    , m_bAutoUpdateGreeks(true)
    , m_pfuDelta(0)
{
    if (!grid) return;

    // Seed risk-data entries for all currently-known options.
    for (const OptionDataPtr& od : grid->getAllOptions())
    {
        createOptionRiskData(od);
    }

    // Grid-listener registration is performed externally by the strategy
    // (grid->addListener(this)); we deliberately do NOT self-register here to
    // avoid a duplicate onComputeValuesCompleted callback.
}

OptionRiskDataPtr OptionRisk::createOptionRiskData(const OptionDataPtr& od)
{
    if (!od) return nullptr;

    // Skip expired contracts (original: getOptionGrid()->expired(instr)).
    if (isExpired(m_spOptionGrid, od)) return nullptr;

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
        // Not yet tracked — try to lazily create from the grid (original:
        // m_spOptionGrid->get(instr) which may trigger a create).
        if (m_spOptionGrid) {
            OptionDataPtr od = m_spOptionGrid->get(instr);
            if (od)
                return createOptionRiskData(od);
        }
        return OptionRiskDataPtr();
    }
    return *it;
}

const OptionExpiryGreeksPtr& OptionRisk::getExpiryGreeks(uint32_t exp)
{
    OptionExpiryGreeksPtr& egreeks = m_expiryTable[exp];
    if (!egreeks)
    {
        ExpiryDataPtr ed;
        if (m_spOptionGrid) ed = m_spOptionGrid->getExpiryData(exp);

        egreeks = std::make_shared<OptionExpiryGreeks>(ed);

        // Auto-register hedge instrument from ExpiryData
        if (ed && !ed->getHedgeCode().empty()) {
            auto hd = registerHedgeInstrument(ed->getHedgeCode(), exp);
            egreeks->setPrimaryHedge(hd);
        }

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
            hd->deltaPosition = hd->multiplier * position;
            // Notify expiry greeks of position change
            auto it = m_expiryTable.find(hd->expiry);
            if (it != m_expiryTable.end() && it->second)
                it->second->__onHedgePositionChange();
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

    // NOTE: The original overrode the underlier delta from a spot leg when a
    // SpotTradingData was present on the grid. SpotTradingData is not part of
    // the migrated OptionGrid (options + futures only). The engine may set
    // m_pfuDelta externally if a spot leg is ever introduced; until then this
    // override is intentionally absent.
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

    // NOTE: spot-leg (SpotTradingData) override not applicable — see update().
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
    // NOTE: spot-leg (SpotTradingData) delta not applicable — see update().
    return raw_delta;
}

std::vector<OptionRiskDataPtr> OptionRisk::getNonZeroPositions()
{
    std::vector<OptionRiskDataPtr> v;
    if (!m_spOptionGrid) return v;
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
    if (isExpired(m_spOptionGrid, od)) return;
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
    if (m_bAutoUpdateGreeks) {
        // P5: Mark all risk data as dirty since the pricer has recomputed
        // option Greeks. This ensures the incremental update() will pick up
        // the new values.
        for (const auto& rd : all())
            rd->markDirty();
        update();
    }
}

} // namespace wt_option
