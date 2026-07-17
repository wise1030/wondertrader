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
#include "../Includes/IHftStraCtx.h"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/fmtlib.h"
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
    auto om = std::make_shared<OptionQuoteManager>(code, omCfg, m_hftCtx);
    if (m_quoteStats) om->setQuoteStatistics(m_quoteStats);
    otd->setQuoteManager(om);

    // 5. Enable + setActive(false) — wait for channel_ready
    otd->enable();
    otd->setActive(false);

    // 6. Inject fee from commodity info (P0-3 fix)
    if (m_hftCtx && od->getFee() == 0) {
        auto ed = od->getExpiryData();
        if (ed) {
            std::string commKey = m_exchange + "." + ed->getOptionProduct();
            WTSCommodityInfo* commInfo = m_hftCtx->stra_get_comminfo(commKey.c_str());
            if (commInfo) {
                // calcFee(price, qty, offset): offset 0=open, 1=close, 2=closeToday
                // For volume-based fee (nFeeAlg==0): fee = rate * qty = rate * 1
                // For amount-based fee (nFeeAlg==1): fee = rate * amount = rate * price * volScale
                // We use a nominal price for amount-based; volume-based is exact.
                double mid = od->getMid();
                double nominalPx = (mid > 0) ? mid : 1.0;
                double fee = commInfo->calcFee(nominalPx, 1, 0);
                od->setFee(fee);
            }
        }
    }

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "OTG::onAddOption created OTD for {}", code);
}

void OptionTradingGrid::onAddExpiry(const ExpiryDataPtr& ed)
{
    if (!ed) return;
    auto it = m_hedgeOverrides.find(ed->getExpiry());
    if (it != m_hedgeOverrides.end() && !it->second.empty()) {
        ed->setHedgeCode(it->second);
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "OTG hedge override: exp={} hedge={}", ed->getExpiry(), it->second);
    }
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
    for (auto& pair : m_tblUnderlyingTradingData) {
        if (pair.second)
            pair.second->setActive(true);
    }
}

void OptionTradingGrid::disableAll()
{
    for (const auto& od : m_spGrid->getAllOptions()) {
        if (od && od->getTradingData())
            od->getTradingData()->setActive(false);
    }
    for (auto& pair : m_tblUnderlyingTradingData) {
        if (pair.second)
            pair.second->setActive(false);
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

    // Set tick size + contract size + fee from contract info
    if (m_hftCtx) {
        CodeHelper::CodeInfo ci = CodeHelper::extractStdCode(hedgeCode.c_str(), nullptr);
        std::string prodKey = fmt::format("{}.{}", ci._exchg, ci._product);
        WTSCommodityInfo* commInfo = m_hftCtx->stra_get_comminfo(prodKey.c_str());
        if (commInfo) {
            if (commInfo->getPriceTick() > 0)
                utd->setTickSize(commInfo->getPriceTick());
            if (commInfo->getVolScale() > 0)
                utd->setContractSize(commInfo->getVolScale());
            // Inject fee rate for cost calculation
            // getFees(price) returns price * m_feePct, so m_feePct = fee_per_lot / volScale
            double volScale = commInfo->getVolScale();
            double feePerLot = commInfo->calcFee(1.0, 1, 0);
            if (volScale > 0 && feePerLot > 0)
                utd->setFeePct(feePerLot / volScale);
        }
    }

    // Create per-future OQM (same pattern as option OTD)
    if (m_hftCtx) {
        OptionQuoteManager::Config futOmCfg = m_futureOqmCfg;
        futOmCfg.exchange = m_exchange;
        futOmCfg.tick_size = utd->getTickSize();
        auto futOm = std::make_shared<OptionQuoteManager>(hedgeCode, futOmCfg, m_hftCtx);
        if (m_quoteStats) futOm->setQuoteStatistics(m_quoteStats);
        utd->setQuoteManager(futOm);
    }
    utd->enable();
    utd->setActive(false);

    // Bind UTD → ExpiryData (so pricer can access hedge trading data)
    ed->setHedgeUTD(utd.get());

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
