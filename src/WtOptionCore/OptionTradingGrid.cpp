/*!
 * \file OptionTradingGrid.cpp
 * \brief Trading object lifecycle manager implementation
 *
 * Migrated from quantbox optioncore/OptionTradingGrid.cc (530 lines).
 * Key method: onAddOption creates OTD + binds executors + binds RiskData.
 */
#include "OptionTradingGrid.h"
#include "OptionQuoteManager.h"
#include "OptionRisk.h"
#include "OptionRiskData.h"
#include "OptionExpiryGreeks.h"
#include "../WTSTools/WTSLogger.h"

namespace wt_option {

OptionTradingGrid::OptionTradingGrid(OptionGridPtr grid)
    : m_spGrid(std::move(grid))
{
}

// ============================================================================
// IOptionGridListener — receives events from OptionGrid
// ============================================================================

void OptionTradingGrid::onAddOption(const OptionDataPtr& od)
{
    if (!od || od->getTradingData()) return;  // already created

    // 1. Create OTD
    auto otd = std::make_shared<OptionTradingData>();
    otd->init(od);
    od->setTradingData(otd);

    // 2. Bind RiskData (if OptionRisk is set)
    if (m_spPositionRisk) {
        auto rd = m_spPositionRisk->get(od->getCode());
        // OTD uses position provider; RiskData binding happens via setPositionProvider
    }

    // 3. Bind executors (wrap with code capture for per-contract dispatch)
    const std::string& code = od->getCode();
    if (m_quoteExec) {
        otd->setQuoteExecutor([this, code](double bidP, uint32_t bidQ,
                                            double askP, uint32_t askQ) -> uint32_t {
            int32_t ret = m_quoteExec(code, bidP, bidQ, askP, askQ);
            return static_cast<uint32_t>(ret);
        });
    }
    if (m_orderExec) {
        otd->setOrderExecutor([this, code](bool isBuy, double price, uint32_t qty) -> bool {
            int32_t ret = m_orderExec(code, isBuy, price, qty);
            return ret > 0;
        });
    }
    if (m_cancelExec) {
        // OTD cancel executor uses orderId; for now wrap with code-based cancel
        // Phase 2 will replace with per-contract OptionQuoteManager
        otd->setCancelExecutor([this, code](uint32_t /*orderId*/) -> bool {
            m_cancelExec(code);
            return true;
        });
    }

    // 4. Create/ensure ExpiryTradingData exists
    getExpiryTradingData(od->getExpiry());

    // 4b. Create per-contract OptionQuoteManager (Phase 2)
    OptionQuoteManager::Config omCfg = m_optionOqmCfg;
    omCfg.exchange = m_exchange;
    omCfg.tick_size = od->getTickSize();
    auto om = std::make_shared<OptionQuoteManager>(code, omCfg, m_uftCtx);
    otd->setQuoteManager(om);

    // 5. Enable + setActive(false) — wait for channel_ready
    otd->enable();
    otd->setActive(false);

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "OTG::onAddOption created OTD for {}", code);
}

void OptionTradingGrid::onAddExpiry(const ExpiryDataPtr& ed)
{
    if (!ed) return;
    getExpiryTradingData(ed->getExpiry());
}

void OptionTradingGrid::onComputeValuesCompleted(const IOptionGrid* /*grid*/)
{
    // Forward to OptionRisk (if connected as separate listener, it already receives this)
    // CTG also receives this independently via its own listener registration
    // Nothing to do here in the middle layer — events flow directly from Grid to listeners
}

// ============================================================================
// Accessors
// ============================================================================

OptionTradingDataPtr OptionTradingGrid::getTradingData(const std::string& code) const
{
    auto od = m_spGrid->get(code);
    if (!od) return nullptr;
    return od->getTradingData();
}

OptionTradingDataPtr OptionTradingGrid::getTradingData(
    uint32_t exp, strike_t stk, OptionRight right) const
{
    auto od = m_spGrid->get(exp, stk, right);
    if (!od) return nullptr;
    return od->getTradingData();
}

UnderlyingTradingDataPtr OptionTradingGrid::getUnderlyingTradingData(
    const std::string& code) const
{
    auto it = m_tblUnderlyingTradingData.find(code);
    if (it != m_tblUnderlyingTradingData.end())
        return it->second;
    return nullptr;
}

ExpiryTradingDataPtr OptionTradingGrid::getExpiryTradingData(uint32_t exp) const
{
    auto it = m_tblExpiryTradingData.find(exp);
    if (it != m_tblExpiryTradingData.end())
        return it->second;

    // Lazy creation (const_cast — mirrors quantbox pattern)
    auto* self = const_cast<OptionTradingGrid*>(this);
    auto ed = self->m_spGrid->getExpiryData(exp);
    if (!ed) return nullptr;
    return self->__createExpiryTradingData(ed);
}

// ============================================================================
// Compute delegation
// ============================================================================

void OptionTradingGrid::computeValues(IOptionPricer* pricer)
{
    if (m_spGrid && pricer)
        m_spGrid->computeValues(pricer);
}

// ============================================================================
// Enable/disable all
// ============================================================================

void OptionTradingGrid::enableAll()
{
    for (const auto& od : m_spGrid->getAllOptions()) {
        if (od && od->getTradingData())
            od->getTradingData()->setActive(true);
    }
}

void OptionTradingGrid::disableAll()
{
    for (const auto& od : m_spGrid->getAllOptions()) {
        if (od && od->getTradingData())
            od->getTradingData()->setActive(false);
    }
}

// ============================================================================
// Private — create ExpiryTradingData
// ============================================================================

ExpiryTradingDataPtr OptionTradingGrid::__createExpiryTradingData(const ExpiryDataPtr& ed)
{
    auto etd = std::make_shared<ExpiryTradingData>();

    // Create primary UTD for this expiry's hedge code
    const std::string& hedgeCode = ed->getHedgeCode();
    auto utd = std::make_shared<UnderlyingTradingData>(hedgeCode, ed, shared_from_this());
    etd->setPrimaryUnderlier(utd);
    m_tblUnderlyingTradingData[hedgeCode] = utd;

    // Bind expiry greeks from OptionRisk
    if (m_spPositionRisk) {
        auto egreeks = m_spPositionRisk->getExpiryGreeks(ed->getExpiry());
        etd->setExpiryGreeks(egreeks);
    }

    m_tblExpiryTradingData[ed->getExpiry()] = etd;

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "OTG::__createExpiryTradingData exp={} hedge={}", ed->getExpiry(), hedgeCode);

    return etd;
}

} // namespace wt_option
