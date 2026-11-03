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

#include <fstream>

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
    m_tblOptionTradingData[od->getCode()] = otd;
    od->setTradingData(otd);

    // 2. Bind RiskData (if OptionRisk is set)
    if (m_spPositionRisk) {
        auto rd = m_spPositionRisk->get(od->getCode());
        // OTD uses position provider; RiskData binding happens via setPositionProvider
    }

    // B03 fix: inject static contract facts (tick size / multiplier) from the
    // option product's comm info. setupGrid has no IBaseDataMgr in live mode,
    // so OptionData starts with tickSize=1.0.  A live contract may also have a
    // legitimate 1.0 tick, so always prefer exchange metadata when available.
    // NOTE: must run BEFORE the OQM is created (it copies tick_size).
    WTSCommodityInfo* optionCommInfo = nullptr;
    if (m_hftCtx) {
        // Derive the commodity from the concrete option code.  This handles
        // CZCE call/put dual products correctly (SRC/SRP) and avoids relying on
        // the grid-level product string, which is only a coarse default.
        auto ci = CodeHelper::extractStdCode(od->getCode().c_str(), nullptr);
        std::string commKey = fmt::format("{}.{}", ci._exchg, ci._product);
        optionCommInfo = m_hftCtx->stra_get_comminfo(commKey.c_str());
        if (optionCommInfo) {
            if (optionCommInfo->getPriceTick() > 0)
                od->setTickSize(optionCommInfo->getPriceTick());
            if (optionCommInfo->getVolScale() > 0)
                od->setMultiplier(optionCommInfo->getVolScale());
        }
    }

    // Propagate late-arriving contract facts to components that cache them.
    if (m_spPositionRisk) {
        auto rd = m_spPositionRisk->get(od->getCode());
        if (rd)
            rd->setContractSize(od->getMultiplier());
    }
    otd->getPnlTracker()->setMultiplier(od->getMultiplier());

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
    omCfg.tick_size = od->getTickSize();  // now carries the real tick (B03)
    omCfg.right_flag = (od->getRight() == OR_Call) ? 1 : 0;  // B3
    auto om = std::make_shared<OptionQuoteManager>(code, omCfg);
    if (m_quoteStats) om->setQuoteStatistics(m_quoteStats);
    if (m_preTradeFn) om->setPreTradeCheckFn(m_preTradeFn);   // A2
    if (m_optionFilterChain) om->setRiskFilterChain(m_optionFilterChain);
    if (m_positionGuardLookup) {
        auto guard = m_positionGuardLookup(code);
        if (guard) om->setPositionGuard(guard);
    }
    if (m_positionOffsetLookup) {
        auto offset = m_positionOffsetLookup(code);
        if (offset) om->setPositionOffsetMgr(offset);
    }
    // Route all exchange operations through the formal OrderGateway.  OQM has
    // no raw IHftStraCtx fallback; when no gateway is injected it is inert.
    if (m_orderGateway) {
        om->setSendQuoteFn([this, code](double bidP, uint32_t bidQ,
                                         double askP, uint32_t askQ) {
            return m_orderGateway->submitQuote(code, bidP, bidQ, askP, askQ);
        });
        if (omCfg.quote_style == OptionQuoteManager::Config::QS_BUYSELL) {
            om->setSendSingleFn([this, code](bool isBuy, double px,
                                              uint32_t qty) -> uint32_t {
                auto ids = m_orderGateway->submitOrder(code, isBuy, px, qty);
                if (m_orderIssuedFn) {
                    for (uint32_t id : ids) {
                        if (id != 0)
                            m_orderIssuedFn(code, isBuy, px, qty, id);
                    }
                }
                return ids.empty() ? 0 : ids[0];
            });
        } else if (m_orderIssuedFn) {
            // Paired-quote path: OQM trackOrder sees the returned side id.
            om->setOrderIssuedFn([this, code](bool isBuy, double px,
                                               uint32_t qty, uint32_t localid) {
                m_orderIssuedFn(code, isBuy, px, qty, localid);
            });
        }
        om->setCancelOrderFn([this](uint32_t localid) {
            return m_orderGateway->cancelOrder(localid);
        });
        om->setCancelAllFn([this, code]() {
            m_orderGateway->cancelAll(code);
        });
    }
    // B23 fix: wire the clock — TTL / min_intra_update / reject-retry timing
    // were all dead because m_getTime was never set
    om->setGetTimeFn([this, ctx = m_hftCtx]() -> double {
        if (m_timeFn) return m_timeFn();
        return ctx ? wt_option::ctxTimeSeconds(ctx) : 0.0;
    });
    // B11 fix: give OTD a live position source so QM_CLOSE/auto-close work
    if (m_positionSource)
        otd->setPositionProvider([src = m_positionSource, code]() -> int32_t {
            return src(code);
        });
    // P1-f: bind risk-side per-option data when it already exists (rd created
    // before this OTD); the reverse direction is bound in
    // OptionRisk::createOptionRiskData.  get() lazily creates the rd.
    if (m_spPositionRisk)
        otd->setOptionRiskData(m_spPositionRisk->get(code));
    otd->setQuoteManager(om);
    // B5: FIFO PnL mode for this contract's tracker
    if (m_fifoPnl)
        otd->getPnlTracker()->setFifoMode(true);

    // 5. Enable + setActive(false) — wait for channel_ready
    otd->enable();
    otd->setActive(false);

    // 6. Inject fee from the concrete option product's commodity info.
    if (m_hftCtx && od->getFee() == 0) {
        auto ci = CodeHelper::extractStdCode(od->getCode().c_str(), nullptr);
        std::string commKey = fmt::format("{}.{}", ci._exchg, ci._product);
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
            // PnlTracker charges multiplier * qty * price * feePct.
            // Convert the per-lot exchange fee into that proportional form.
            double feeBase = od->getMultiplier() * nominalPx;
            if (fee > 0 && feeBase > 0)
                otd->getPnlTracker()->setFeePct(fee / feeBase);
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
    auto existing = m_tblExpiryTradingData.find(ed->getExpiry());
    if (existing == m_tblExpiryTradingData.end())
        __createExpiryTradingData(ed);
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
    auto it = m_tblOptionTradingData.find(code);
    if (it != m_tblOptionTradingData.end())
        return it->second;
    auto od = m_spGrid->get(code);
    return od ? od->getTradingData() : nullptr;
}

OptionTradingDataPtr OptionTradingGrid::getTradingData(
    uint32_t exp, strike_t stk, OptionRight right) const
{
    auto od = m_spGrid->get(exp, stk, right);
    if (!od) return nullptr;
    auto it = m_tblOptionTradingData.find(od->getCode());
    if (it != m_tblOptionTradingData.end())
        return it->second;
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

    // Create primary UTD for this expiry's hedge code.
    // P0-2: when several expiries share the same hedgeCode, reuse ONE UTD.
    // The previous unconditional overwrite orphaned the earlier expiry's UTD
    // (its mid/quote-manager stopped receiving ticks) and would have built a
    // second quote manager for the same contract.
    const std::string& hedgeCode = ed->getHedgeCode();
    bool freshUtd = false;
    UnderlyingTradingDataPtr utd;
    auto existing = m_tblUnderlyingTradingData.find(hedgeCode);
    if (existing != m_tblUnderlyingTradingData.end()) {
        utd = existing->second;
    } else {
        utd = std::make_shared<UnderlyingTradingData>(hedgeCode, ed, shared_from_this());
        m_tblUnderlyingTradingData[hedgeCode] = utd;
        freshUtd = true;
    }
    etd->setPrimaryUnderlier(utd);

    // P0-1: give the UTD a live position source.  Without it fut_pos was
    // permanently 0 (CompositeOptionPricer sized future quotes as if flat).
    // Same provider contract as the OTD one (strategy _positions projection).
    if (freshUtd && m_positionSource)
        utd->setPositionProvider([src = m_positionSource, hedgeCode]() -> int32_t {
            return src(hedgeCode);
        });

    // Set tick size + contract size + fee from contract info
    if (freshUtd && m_hftCtx) {
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

    // Create per-future OQM (same pattern as option OTD).  The OQM is also
    // present without an IHftStraCtx for deterministic unit tests; send calls
    // remain inert until a sender/ctx is supplied.
    OptionQuoteManager::Config futOmCfg = m_futureOqmCfg;
    futOmCfg.exchange = m_exchange;
    futOmCfg.tick_size = utd->getTickSize();
    auto futOm = std::make_shared<OptionQuoteManager>(hedgeCode, futOmCfg);
    if (m_quoteStats) futOm->setQuoteStatistics(m_quoteStats);
    if (m_preTradeFn) futOm->setPreTradeCheckFn(m_preTradeFn);   // A2
    if (m_futureFilterChain) futOm->setRiskFilterChain(m_futureFilterChain);
    if (m_positionGuardLookup) {
        auto guard = m_positionGuardLookup(hedgeCode);
        if (guard) futOm->setPositionGuard(guard);
    }
    if (m_positionOffsetLookup) {
        auto offset = m_positionOffsetLookup(hedgeCode);
        if (offset) futOm->setPositionOffsetMgr(offset);
    }
    if (m_orderGateway) {
        futOm->setSendQuoteFn([this, hedgeCode](double bidP, uint32_t bidQ,
                                                 double askP, uint32_t askQ) {
            return m_orderGateway->submitQuote(hedgeCode, bidP, bidQ, askP, askQ);
        });
        if (futOmCfg.quote_style == OptionQuoteManager::Config::QS_BUYSELL) {
            futOm->setSendSingleFn([this, hedgeCode](bool isBuy, double px,
                                                      uint32_t qty) -> uint32_t {
                auto ids = m_orderGateway->submitOrder(hedgeCode, isBuy, px, qty);
                if (m_orderIssuedFn) {
                    for (uint32_t id : ids) {
                        if (id != 0)
                            m_orderIssuedFn(hedgeCode, isBuy, px, qty, id);
                    }
                }
                return ids.empty() ? 0 : ids[0];
            });
        } else if (m_orderIssuedFn) {
            futOm->setOrderIssuedFn([this, hedgeCode](bool isBuy, double px,
                                                       uint32_t qty, uint32_t localid) {
                m_orderIssuedFn(hedgeCode, isBuy, px, qty, localid);
            });
        }
        futOm->setCancelOrderFn([this](uint32_t localid) {
            return m_orderGateway->cancelOrder(localid);
        });
        futOm->setCancelAllFn([this, hedgeCode]() {
            m_orderGateway->cancelAll(hedgeCode);
        });
    }
    // B23 fix: wire the clock for the future OQM as well
    futOm->setGetTimeFn([this, ctx = m_hftCtx]() -> double {
        if (m_timeFn) return m_timeFn();
        return ctx ? wt_option::ctxTimeSeconds(ctx) : 0.0;
    });
    if (freshUtd) {
        // P0-2: a shared UTD keeps its original OQM — replacing it here would
        // silently swap the quote manager the other expiry is already using.
        utd->setQuoteManager(futOm);
        // B5: FIFO PnL mode for the hedge tracker too
        if (m_fifoPnl)
            utd->getPnlTracker()->setFifoMode(true);
        utd->enable();
        utd->setActive(false);
    }

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

// ============================================================================
// onSessionBegin - B07: reset lifetime counters of every OQM
// ============================================================================
void OptionTradingGrid::onSessionBegin()
{
    for (auto& pair : m_tblUnderlyingTradingData) {
        auto utd = pair.second;
        if (utd && utd->getQuoteManager())
            utd->getQuoteManager()->resetCounters();
    }
    for (const auto& od : m_spGrid->getAllOptions()) {
        if (!od) continue;
        auto otd = od->getTradingData();
        if (otd && otd->getQuoteManager())
            otd->getQuoteManager()->resetCounters();
    }
}

// ============================================================================
// dumpCountersCsv - C5: append every OQM's lifetime counters to path
// ============================================================================
void OptionTradingGrid::dumpCountersCsv(const std::string& path) const
{
    std::ofstream ofs(path, std::ios::app);
    if (!ofs.is_open()) return;
    ofs << "#code,cancels,newOrders,fills,rejects,lateFills,position\n";
    ofs.close();
    for (auto& pair : m_tblUnderlyingTradingData) {
        auto utd = pair.second;
        if (utd && utd->getQuoteManager())
            utd->getQuoteManager()->dumpCountersCsv(path);
    }
    for (const auto& od : m_spGrid->getAllOptions()) {
        if (!od) continue;
        auto otd = od->getTradingData();
        if (otd && otd->getQuoteManager())
            otd->getQuoteManager()->dumpCountersCsv(path);
    }
}

} // namespace wt_option
