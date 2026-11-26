/*!
 * \file OptionGrid.cpp
 * \brief 3-level option grid implementation (migrated from quantbox)
 */
#include "OptionGrid.h"
#include "IOptionPricer.h"
#include "IOptionGridListener.h"

// WT headers — include standard FIRST to avoid namespace pollution
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>

#include "WTSDataDef.hpp"
#include "IBaseDataMgr.h"
#include "WTSContractInfo.hpp"
#include "WTSStruct.h"
#include "CodeHelper.hpp"
#include "WTSLogger.h"
#include "fmtlib.h"

namespace wt_option {

// P0-② (audit 2026-11-18): tail month segment of a contract code, e.g.
// "SHFE.cu.2612" → "2612".  Empty on parse failure (index codes / hot codes).
static std::string contractMonthOf(const std::string& stdCode) {
    auto pos = stdCode.rfind('.');
    if (pos == std::string::npos || pos + 1 >= stdCode.size())
        return "";
    return stdCode.substr(pos + 1);
}

// Right mapping: OptionRight(OR_Call=0, OR_Put=1) → StrikeData Right(RT_CALL, RT_PUT)
// StrikeData uses get(Right) / call() / put() — we'll use those directly

OptionGrid::OptionGrid(const std::string& optionProduct,
                       const std::string& underlyingCode,
                       wtp::IBaseDataMgr* bdMgr,
                       wtp::WTSSessionInfo* sessInfo)
    : m_optionProduct(optionProduct)
    , m_underlyingCode(underlyingCode)
    , m_bdMgr(bdMgr)
    , m_sessInfo(sessInfo)
{
    auto pos = underlyingCode.find('.');
    if (pos != std::string::npos)
        m_exchange = underlyingCode.substr(0, pos);

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "OptionGrid created: product={} underlying={}", optionProduct, underlyingCode);
}

OptionGrid::~OptionGrid() {}

// ============================================================================
// Underlying price — DERIVED, scene-dispatched (2026-11-26 refactor, revised
// same day after review: one-underlying-many-expiries vs per-month futures)
// ============================================================================
//  - Multi-series commodity futures (any expiry registered an EXCLUSIVE own
//    underlying): there is no single shared underlying — the underlying
//    family is {front-month future, far-month futures}.  The grid-level
//    reference price is the FRONT month's price: frontExpiry()->price().
//  - Single-underlying scenes (index options like CFFEX IO/MO; exchange
//    series options configured on one product-level code): ONE underlying
//    feeds MULTIPLE expiries, all sharing the same price series.  The
//    reference price is the global underlying price itself — it must NOT
//    wait for any ExpiryData to exist (startup window regression) and does
//    not need a front-month selector.
double OptionGrid::getUnderlyingPrice() const {
    if (!m_expiryOwnUnderlying.empty()) {
        auto fe = frontExpiry();
        return fe ? fe->getUnderlyingPrice() : 0.0;
    }
    return m_globalUnderlyingPrice;
}

bool OptionGrid::isUnderlyingFresh(uint64_t nowMs) const {
    if (m_marketStaleTimeoutMs == 0) return true;
    uint64_t ts;
    if (!m_expiryOwnUnderlying.empty()) {
        auto fe = frontExpiry();
        if (!fe) return false;
        ts = fe->underlyingUpdateTime();
    } else {
        ts = m_globalUnderlyingUpdateTime;
    }
    if (ts == 0) return false;
    if (nowMs < ts) return true;  // clock skew: accept
    return nowMs - ts <= m_marketStaleTimeoutMs;
}

bool OptionGrid::hasFreshOptionMarket(uint64_t nowMs) const {
    if (m_marketStaleTimeoutMs == 0) return true;
    std::shared_lock<std::shared_mutex> lock(m_gridMutex);
    for (const auto& od : m_allOptions) {
        if (!od) continue;
        const uint64_t ts = od->getMarket().updateTime;
        if (ts != 0 && (nowMs < ts || nowMs - ts <= m_marketStaleTimeoutMs))
            return true;
    }
    return false;
}

void OptionGrid::setExpiryUnderlying(uint32_t expiry, const std::string& code) {
    if (code.empty()) return;
    auto& vec = m_expiryUnderlyingMap[code];
    // Avoid duplicate registration
    for (uint32_t e : vec) {
        if (e == expiry) return;
    }
    vec.push_back(expiry);
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "OptionGrid: expiry {} underlying registered as '{}'", expiry, code);
}

// ============================================================================
// onTick — tick-driven contract discovery + market update
// ============================================================================
void OptionGrid::onTick(const std::string& stdCode, const wtp::WTSTickData* tick) {
    if (!tick) return;

    // The global underlying's own tick always refreshes the global price
    // state, independent of routing (index codes are ALSO registered in the
    // per-expiry map for fallback expiries, so they never reach the global
    // branch below).  Single-underlying scenes read this state as the
    // grid-level reference price.
    if (stdCode == m_underlyingCode)
        m_globalUnderlyingPrice = tick->price();

    // Per-expiry underlying tick? (multi-series scenario: different futures
    // contracts for different expiries).  Each registered expiry's own price
    // is updated — this is the pricing source of truth.  The grid-level
    // price derives from the front expiry via getUnderlyingPrice().
    auto eit = m_expiryUnderlyingMap.find(stdCode);
    if (eit != m_expiryUnderlyingMap.end()) {
        for (uint32_t exp : eit->second) {
            auto ed = getExpiryData(exp);
            if (ed) ed->setUnderlyingPrice(tick->price());
        }
        return;
    }

    // Global underlying tick? (e.g. index price for CFFEX options, or
    // single-series commodity).  Route it into the expiries that fall back
    // to the global underlying — same P0-② month-match gate as the fallback
    // registration.  Index-type underlyings (no month segment) feed all
    // fallback expiries.  Also remember the price/timestamp as the
    // single-underlying scene's grid-level reference (independent of any
    // ExpiryData existing).
    if (stdCode == m_underlyingCode) {
        const std::string gm = contractMonthOf(stdCode);
        for (auto& [exp, ed] : m_expiries) {
            if (!ed) continue;
            if (m_expiryOwnUnderlying.find(exp) != m_expiryOwnUnderlying.end())
                continue;   // exclusive own underlying — never overwritten
            const std::string em = std::to_string(exp).substr(2);   // YYYYMM → YYMM
            if (gm.size() != 4 || gm == em)
                ed->setUnderlyingPrice(tick->price());
        }
        return;
    }

    // Option tick?
    if (!CodeHelper::isStdChnFutOptCode(stdCode.c_str()))
        return;

    // Build market snapshot from tick (B12: include multi-level depth)
    OptionMarket mkt;
    mkt.bid = tick->bidprice(0);
    mkt.ask = tick->askprice(0);
    mkt.last = tick->price();
    mkt.bidSize = tick->bidqty(0);
    mkt.askSize = tick->askqty(0);
    mkt.underlyingPrice = getUnderlyingPrice();
    mkt.updateTime = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());

    // B12: Capture multi-level depth (up to 10 levels)
    for (int32_t i = 0; i < MAX_MARKET_DEPTH; ++i) {
        double bp = tick->bidprice(i);
        double ap = tick->askprice(i);
        double bq = tick->bidqty(i);
        double aq = tick->askqty(i);
        if (bp > 0 && bq > 0) {
            mkt.bidPrices[i] = bp;
            mkt.bidQty[i] = bq;
            mkt.numBidLevels = i + 1;
        }
        if (ap > 0 && aq > 0) {
            mkt.askPrices[i] = ap;
            mkt.askQty[i] = aq;
            mkt.numAskLevels = i + 1;
        }
    }

    // Dynamic discovery: create if not exists
    {
        std::shared_lock<std::shared_mutex> rlock(m_gridMutex);
        auto it = m_optionsByCode.find(stdCode);
        if (it != m_optionsByCode.end()) {
            it->second->updateMarket(mkt);
            return;
        }
    }

    // Create new option
    OptionDataPtr od = createOption(stdCode);
    if (od) {
        od->updateMarket(mkt);

        // Update ExpiryData with exact expiredate from contract info
        auto* ci = tick->getContractInfo();
        if (ci && ci->getExpireDate() > 0) {
            auto ed = od->getExpiryData();
            if (ed) {
                uint32_t curDate = m_currentDate;
                if (curDate == 0) return;  // not yet initialized
                ed->setCurSecOfDay(m_curSecOfDay);
                ed->updateDaysToExpiration(curDate, ci->getExpireDate());
            }
        }
    }
}

// ============================================================================
// onTick (TickDataRef) — async path, no WTSTickData* dependency
// ============================================================================
void OptionGrid::onTick(const std::string& stdCode, const TickDataRef& tick) {
    // The global underlying's own tick always refreshes the global price
    // state, independent of routing (index codes are ALSO registered in the
    // per-expiry map for fallback expiries, so they never reach the global
    // branch below).  Single-underlying scenes read this state as the
    // grid-level reference price.
    if (stdCode == m_underlyingCode) {
        m_globalUnderlyingPrice = tick.price;
        m_globalUnderlyingUpdateTime = tick.updateTime;
    }

    // 1. Per-expiry underlying tick? (multi-series scenario: different futures contracts for different expiries)
    auto eit = m_expiryUnderlyingMap.find(stdCode);
    if (eit != m_expiryUnderlyingMap.end()) {
        for (uint32_t exp : eit->second) {
            auto ed = getExpiryData(exp);
            if (ed) {
                ed->setUnderlyingPrice(tick.price, tick.updateTime);
                // Update future market data for synthetic forward (quantbox design)
                double futSpread = (tick.bid > 0 && tick.ask > 0) ? (tick.ask - tick.bid) : 0;
                ed->setFutureMid(tick.price, futSpread);
            }
        }
        return;
    }

    // 2. Global underlying tick? (e.g. index price for CFFEX options, or single-series commodity)
    //    Route it into the expiries that FALL BACK to the global underlying,
    //    with the same P0-② month-match gate as the fallback registration —
    //    the global MAIN-month tick must not overwrite a far month's pricing
    //    leg.  Index-type underlyings (no month segment) feed all fallback
    //    expiries.  Also remember the price/timestamp as the
    //    single-underlying scene's grid-level reference (independent of any
    //    ExpiryData existing).
    if (stdCode == m_underlyingCode) {
        const std::string gm = contractMonthOf(stdCode);
        for (auto& [exp, ed] : m_expiries) {
            if (!ed) continue;
            if (m_expiryOwnUnderlying.find(exp) != m_expiryOwnUnderlying.end())
                continue;   // exclusive own underlying — never overwritten
            const std::string em = std::to_string(exp).substr(2);   // YYYYMM → YYMM
            if (gm.size() != 4 || gm == em) {
                ed->setUnderlyingPrice(tick.price, tick.updateTime);
                if (ed->getIncludeFuture()) {
                    double futSpread = (tick.bid > 0 && tick.ask > 0) ? (tick.ask - tick.bid) : 0;
                    ed->setFutureMid(tick.price, futSpread);
                }
            }
        }
        return;
    }

    // 3. Option tick?
    if (!CodeHelper::isStdChnFutOptCode(stdCode.c_str()))
        return;

    // Build market snapshot from tick
    OptionMarket mkt;
    mkt.bid = tick.bid;
    mkt.ask = tick.ask;
    mkt.last = tick.price;
    mkt.bidSize = tick.bidQty;
    mkt.askSize = tick.askQty;
    mkt.underlyingPrice = getUnderlyingPrice();
    mkt.updateTime = tick.updateTime;

    // P1 follow-up: five-level book into the B12 depth store (level 0 is
    // level 1 of the arrays — bid/bidSize above stay the authoritative best).
    for (int32_t i = 0; i < TickDataRef::DEPTH; ++i) {
        if (i < tick.numBidLevels) {
            mkt.bidPrices[i] = tick.bidPrices[i];
            mkt.bidQty[i] = tick.bidQtys[i];
        }
        if (i < tick.numAskLevels) {
            mkt.askPrices[i] = tick.askPrices[i];
            mkt.askQty[i] = tick.askQtys[i];
        }
    }
    mkt.numBidLevels = tick.numBidLevels;
    mkt.numAskLevels = tick.numAskLevels;

    // Dynamic discovery: create if not exists
    {
        std::shared_lock<std::shared_mutex> rlock(m_gridMutex);
        auto it = m_optionsByCode.find(stdCode);
        if (it != m_optionsByCode.end()) {
            it->second->updateMarket(mkt);
            return;
        }
    }

    // Create new option
    OptionDataPtr od = createOption(stdCode);
    if (od) {
        od->updateMarket(mkt);

        // B28 fix: backfill exact expiry date from tick contract info.
        // The async path used to lose this, freezing expiry at the YYYYMM15
        // approximation (±14 days error in maturity/theta/discount).
        if (tick.expireDate > 0) {
            auto ed = od->getExpiryData();
            if (ed && m_currentDate != 0) {
                ed->setCurSecOfDay(m_curSecOfDay);
                ed->updateDaysToExpiration(m_currentDate, tick.expireDate);
            }
        }
    }
}

// ============================================================================
// createOption — parse stdCode → OptionData + StrikeData + ExpiryData
// ============================================================================
OptionDataPtr OptionGrid::createOption(const std::string& stdCode) {
    return __createOption(stdCode);
}

OptionDataPtr OptionGrid::__createOption(const std::string& stdCode) {
    std::unique_lock<std::shared_mutex> lock(m_gridMutex);

    auto it = m_optionsByCode.find(stdCode);
    if (it != m_optionsByCode.end())
        return it->second;

    if (!CodeHelper::isStdChnFutOptCode(stdCode.c_str())) {
        WTSLogger::log_by_cat("strategy", LL_WARN, "OptionGrid: not option: {}", stdCode);
        return nullptr;
    }

    // Parse: EXCHG.productMMMM.C.strike (4 parts split by '.')
    std::vector<std::string> parts;
    {
        std::string s = stdCode;
        size_t start = 0, end;
        while ((end = s.find('.', start)) != std::string::npos) {
            parts.push_back(s.substr(start, end - start));
            start = end + 1;
        }
        parts.push_back(s.substr(start));
    }
    if (parts.size() < 4) return nullptr;

    const std::string& exchg = parts[0];
    const std::string& prodMonth = parts[1];
    const std::string& rightStr = parts[2];
    const std::string& strikeStr = parts[3];

    // Extract product (alpha prefix) and month (digit suffix) from prodMonth
    std::string product;
    std::string monthStr;
    for (size_t i = 0; i < prodMonth.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(prodMonth[i]))) {
            product = prodMonth.substr(0, i);
            monthStr = prodMonth.substr(i);
            break;
        }
    }
    if (product.empty()) product = prodMonth;

    // Parse expiry YYYYMM (6 digits, e.g. 202610)
    uint32_t expiry = 0;
    if (monthStr.size() == 4) {
        // YYMM → YYYYMM
        uint32_t yy = std::stoul(monthStr.substr(0, 2));
        uint32_t mm = std::stoul(monthStr.substr(2));
        expiry = (2000 + yy) * 100 + mm;
    } else if (monthStr.size() == 6) {
        expiry = std::stoul(monthStr);
    }

    strike_t strike = std::stod(strikeStr);
    OptionRight right = (rightStr == "C" || rightStr == "c") ? OR_Call : OR_Put;

    // Get multiplier/tickSize from IBaseDataMgr
    double multiplier = 1.0;
    double tickSize = 1.0;
    if (m_bdMgr) {
        // raw code for option: e.g. "cu2502C50000" or "cu2502-50000" etc
        CodeHelper::CodeInfo ci = CodeHelper::extractStdCode(stdCode.c_str(), nullptr);
        WTSCommodityInfo* commInfo = m_bdMgr->getCommodity(ci._exchg, ci._product);
        if (commInfo) {
            multiplier = commInfo->getVolScale();
            if (multiplier <= 0) multiplier = 1.0;
            double pxTick = commInfo->getPriceTick();
            if (pxTick > 0) tickSize = pxTick;
        }
    }

    OptionInfo info;
    info.code = stdCode;
    info.product = product;
    info.expiry = expiry;
    info.strike = strike;
    info.right = right;
    info.multiplier = multiplier;
    info.tickSize = tickSize;

    auto od = std::make_shared<OptionData>(info);
    od->setInternalId(static_cast<uint32_t>(m_allOptions.size()));

    ExpiryDataPtr ed = __getOrCreateExpiryData(expiry);
    // 用 bdMgr 查精确到期日, 覆盖近似值
    if (m_bdMgr) {
        std::string rawCode = product + monthStr;
        WTSContractInfo* cInfo = m_bdMgr->getContract(rawCode.c_str(), exchg.c_str());
        if (cInfo && cInfo->getExpireDate() > 0) {
            ed->setCurSecOfDay(m_curSecOfDay);
            ed->updateDaysToExpiration(m_currentDate, cInfo->getExpireDate());
        }
    }
    od->setExpiryData(ed);

    StrikeDataPtr sd = __findOrCreateStrike(expiry, strike);
    // Wire option into strike and keep the reverse link; scanner execution
    // and parity-based strategies need the sibling leg from OptionData.
    if (sd) {
        if (right == OR_Call)
            sd->setCall(od);
        else
            sd->setPut(od);
        od->setStrikeData(sd);
    }

    m_allOptions.push_back(od);
    m_optionsByCode[stdCode] = od;

    // Set initial market
    OptionMarket mkt;
    mkt.underlyingPrice = getUnderlyingPrice();
    od->updateMarket(mkt);

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "OptionGrid: created {} exp={} strike={} {}",
        stdCode, expiry, strike, rightStr);

    lock.unlock();
    __notifyAddOption(od);
    return od;
}

// ============================================================================
// ExpiryData management
// ============================================================================
ExpiryDataPtr OptionGrid::__getOrCreateExpiryData(uint32_t expiry) {
    auto it = m_expiries.find(expiry);
    if (it != m_expiries.end())
        return it->second;

    auto ed = std::make_shared<ExpiryData>(expiry, m_optionProduct);
    ed->setBaseDataMgr(m_bdMgr);
    ed->setSessionInfo(m_sessInfo);
    ed->setStdPID(m_exchange + "." + m_optionProduct);

    // P2/G3: with an exclusive per-expiry underlying, bind THAT code and do
    // NOT route the global underlying into this expiry (previously both were
    // registered and a global tick overwrote the per-expiry price,
    // last-tick-won).  Otherwise bind the global underlying and register its
    // tick routing as before.  The strategy registers per-expiry underlyings
    // at on_init — before any ExpiryData exists — so the map is authoritative
    // by creation time.
    {
        auto ownIt = m_expiryOwnUnderlying.find(expiry);
        if (ownIt != m_expiryOwnUnderlying.end()) {
            ed->setUnderlyingCode(ownIt->second);
            ed->setHedgeCode(ownIt->second);   // default hedge; OTG override may refine
        } else {
            ed->setUnderlyingCode(m_underlyingCode);
            ed->setHedgeCode(m_underlyingCode);
            if (!m_underlyingCode.empty()) {
                // P0-② (audit 2026-11-18): only route the global underlying
                // tick into this expiry when its contract month matches the
                // expiry month.  The old behaviour registered the global MAIN
                // month code (e.g. SHFE.cu.2612) for every unconfigured expiry
                // (2702/2703/2704); its tick then carried ~97% of the weight
                // in the parity pool (1/futSpread vs ~1/thousands for option
                // parity points) and dragged the far-month synthetic forward
                // to the front future's price (observed: 202704 fwd=103511 vs
                // parity-implied 99430).  Index-type underlyings have no month
                // segment — keep the old routing there.
                const std::string gm = contractMonthOf(m_underlyingCode);
                const std::string em = std::to_string(expiry).substr(2); // YYYYMM → YYMM
                if (gm.size() != 4 || gm == em) {
                    setExpiryUnderlying(expiry, m_underlyingCode);
                }
            }
        }
    }

    // P0-4: apply forward-calculation config the strategy registered before
    // this ExpiryData existed (ExpiryData is created lazily on the first
    // tick, so on_init-time direct application silently dropped it).
    {
        auto cfgIt = m_expiryFwdCfg.find(expiry);
        if (cfgIt != m_expiryFwdCfg.end()) {
            ed->setIncludeFuture(cfgIt->second.includeFuture);
            ed->setMinStrikesForSynthetic(cfgIt->second.minStrikes);
            ed->setGoodPointsTable(cfgIt->second.goodPointsTable);
        }
    }

    // B2: Set risk-free rate from flat or curve
    double rate = m_riskFreeRate;
    if (!m_rateCurve.empty()) {
        // Will be set precisely after days-to-expiry is computed below.
        // For now, use flat as initial; update after updateDaysToExpiration.
    }
    ed->setRiskFreeRate(rate);
    if (!m_bdMgr && !m_holidays.empty()) {
        ed->setHolidays(&m_holidays);
    }

    // Set days to expiration
    // 优先从 IBaseDataMgr 获取精确到期日, fallback 用近似值
    uint32_t expireDate = 0;
    if (m_bdMgr) {
        // 从 expiry 构造合约代码 (如 ag2608) 查询到期日
        // expiry 格式: YYYYMM -> 需要品种前缀
        // 这里无法获取品种前缀, 由 createOption 调用时通过 bdMgr 查询
        // 留给 onTick 中通过 ContractInfo 更新
    }
    if (expireDate == 0)
        expireDate = (expiry / 100) * 10000 + (expiry % 100) * 100 + 15;

    if (m_currentDate > 0) {
        ed->setCurSecOfDay(m_curSecOfDay);
        ed->updateDaysToExpiration(m_currentDate, expireDate);
    }

    // B2: If a rate curve is configured, update the rate based on actual days-to-expiry
    if (!m_rateCurve.empty()) {
        double curveRate = getRateForDays(ed->daysToExpiry());
        ed->setRiskFreeRate(curveRate);
    }

    m_expiries[expiry] = ed;
    __notifyAddExpiry(ed);
    return ed;
}

// ============================================================================
// StrikeData management
// ============================================================================
StrikeDataPtr OptionGrid::__findOrCreateStrike(uint32_t expiry, strike_t strike) {
    auto& vec = m_strikesByExpiry[expiry];
    for (const auto& sd : vec) {
        if (std::abs(sd->getStrikePrice() - strike) < 1e-6)
            return sd;
    }

    ExpiryDataPtr ed = __getOrCreateExpiryData(expiry);
    auto sd = StrikeData::createWT(ed, strike);
    vec.push_back(sd);
    m_allStrikes.push_back(sd);
    return sd;
}

// ============================================================================
// Lookup methods
// ============================================================================
OptionDataPtr OptionGrid::get(uint32_t expiry, strike_t strike, OptionRight right) {
    std::shared_lock<std::shared_mutex> lock(m_gridMutex);
    auto it = m_strikesByExpiry.find(expiry);
    if (it == m_strikesByExpiry.end()) return nullptr;

    for (const auto& sd : it->second) {
        if (std::abs(sd->getStrikePrice() - strike) < 1e-6) {
            // StrikeData uses Right enum (RT_CALL=0, RT_PUT=1)
            // Our OptionRight is OR_Call=0, OR_Put=1 — same values
            return sd->get(static_cast<Right>(right));
        }
    }
    return nullptr;
}

OptionDataPtr OptionGrid::get(const std::string& code) {
    std::shared_lock<std::shared_mutex> lock(m_gridMutex);
    auto it = m_optionsByCode.find(code);
    return (it != m_optionsByCode.end()) ? it->second : nullptr;
}

ExpiryDataPtr OptionGrid::getExpiryData(uint32_t expiry) const {
    auto it = m_expiries.find(expiry);
    return (it != m_expiries.end()) ? it->second : nullptr;
}

// 2026-11-26 refactor: "front" is a QUERY, not state.  frontExpiry() returns
// the nearest expiry with daysToExpiry > 0 (fallback: earliest expiry in the
// map; nullptr when empty).  This replaces the former m_frontMonth /
// m_frontMonthExpiry state, reevaluateFrontMonth() and isFrontUnderlyingCode
// — see analysis/front-underlying-roles-analysis-20261126.md.
ExpiryDataPtr OptionGrid::frontExpiry() const {
    ExpiryDataPtr best;
    int32_t bestDte = -1;
    for (const auto& [exp, ed] : m_expiries) {
        if (!ed) continue;
        int32_t dte = ed->daysToExpiry();
        if (dte > 0 && (bestDte < 0 || dte < bestDte)) {
            bestDte = dte;
            best = ed;
        }
    }
    if (!best && !m_expiries.empty())
        best = m_expiries.begin()->second;   // all expired: earliest fallback
    return best;
}

ExpiryDataPtr OptionGrid::getFrontMonthExpiryData() {
    return frontExpiry();
}

void OptionGrid::purgeExpired()
{
    if (m_expiries.empty()) return;

    std::vector<uint32_t> dead;
    for (const auto& [exp, ed] : m_expiries) {
        if (ed && ed->daysToExpiry() < 0)
            dead.push_back(exp);
    }
    if (dead.empty()) return;

    for (uint32_t exp : dead) {
        m_expiries.erase(exp);
        m_strikesByExpiry.erase(exp);
    }

    m_allOptions.erase(std::remove_if(m_allOptions.begin(), m_allOptions.end(),
        [&](const OptionDataPtr& od) {
            return od && std::find(dead.begin(), dead.end(), od->getExpiry()) != dead.end();
        }), m_allOptions.end());
    m_allStrikes.erase(std::remove_if(m_allStrikes.begin(), m_allStrikes.end(),
        [&](const StrikeDataPtr& sd) {
            if (!sd) return false;
            uint32_t exp = sd->call() ? sd->call()->getExpiry()
                         : (sd->put() ? sd->put()->getExpiry() : 0);
            return std::find(dead.begin(), dead.end(), exp) != dead.end();
        }), m_allStrikes.end());

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "OptionGrid: purged {} expired expiries, {} options remain",
        dead.size(), m_allOptions.size());
}

void OptionGrid::refreshExpiryDays() {
    if (m_currentDate == 0) return;
    for (auto& [exp, ed] : m_expiries) {
        if (!ed) continue;
        uint32_t expireDate = ed->getExpirationDate();
        if (expireDate > 0) {
            ed->setCurSecOfDay(m_curSecOfDay);
            ed->updateDaysToExpiration(m_currentDate, expireDate);
        }
    }
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "refreshExpiryDays: date={}, {} expiries updated",
        m_currentDate, m_expiries.size());
}

bool OptionGrid::exists(const std::string& code) const {
    std::shared_lock<std::shared_mutex> lock(m_gridMutex);
    return m_optionsByCode.count(code) > 0;
}

// ============================================================================
// Strike lookups
// ============================================================================
StrikeDataPtr OptionGrid::findStrikeFromGrid(uint32_t expiry, double price) const {
    std::shared_lock<std::shared_mutex> lock(m_gridMutex);
    auto it = m_strikesByExpiry.find(expiry);
    if (it == m_strikesByExpiry.end() || it->second.empty())
        return nullptr;

    const auto& vec = it->second;
    StrikeDataPtr best = vec[0];
    double bestDiff = std::abs(price - best->getStrikePrice());
    for (const auto& sd : vec) {
        double diff = std::abs(price - sd->getStrikePrice());
        if (diff < bestDiff) { bestDiff = diff; best = sd; }
    }
    return best;
}

StrikeDataPtr OptionGrid::getAtmStrike(uint32_t expiry) {
    // R3: snap the reference price onto the listed strike grid first so the
    // nearest-search cannot land on a "virtual" midpoint between two listed
    // intervals (CFFEX IO 50/100, DCE MO 25/50/100 tiers).
    // P2/G4: prefer the per-expiry underlying price — multi-series setups
    // (CZCE SR309/SR701, per-month hedges) used to select the ATM strike off
    // the GLOBAL underlying, which can belong to another series.
    double refPrice = 0;
    ExpiryDataPtr ed = getExpiryData(expiry);
    if (ed && ed->isUnderlyingPriceValid())
        refPrice = ed->getUnderlyingPrice();
    if (refPrice <= 0)
        refPrice = getUnderlyingPrice();
    {
        std::shared_lock<std::shared_mutex> lock(m_gridMutex);
        auto fit = m_strikeFinders.find(expiry);
        if (fit != m_strikeFinders.end() && fit->second && refPrice > 0)
            refPrice = fit->second->getStrike(refPrice);
    }
    return findStrikeFromGrid(expiry, refPrice);
}

StrikeDataPtr OptionGrid::findOrCreateStrike(uint32_t expiry, strike_t strike) {
    return __findOrCreateStrike(expiry, strike);
}

std::vector<StrikeDataPtr> OptionGrid::getStrikesByExpiry(uint32_t expiry) const {
    std::shared_lock<std::shared_mutex> lock(m_gridMutex);
    auto it = m_strikesByExpiry.find(expiry);
    return (it != m_strikesByExpiry.end()) ? it->second : std::vector<StrikeDataPtr>();
}

// ============================================================================
// Forward calculation
// ============================================================================
double OptionGrid::getFrontForward() {
    auto fe = frontExpiry();
    return fe ? getAtmForward(fe->getExpiry()) : NAN;
}

double OptionGrid::getAtmForward(uint32_t expiry) {
    auto it = m_atmFwdCache.find(expiry);
    if (it != m_atmFwdCache.end()) return it->second;
    ExpiryDataPtr ed = getExpiryData(expiry);
    if (!ed) return NAN;
    double fwd = __getBestSyntheticPrice(ed);
    // Only cache valid forward values; don't cache NAN so subsequent calls
    // can recompute if market data improves within the same compute cycle.
    if (!std::isnan(fwd))
        m_atmFwdCache[expiry] = fwd;
    return fwd;
}

double OptionGrid::__getBestSyntheticPrice(const ExpiryDataPtr& ed) {
    if (!ed) return NAN;

    // Get underlying price (per-expiry or global)
    double underlyingPrice = 0;
    if (ed->isUnderlyingPriceValid())
        underlyingPrice = ed->getUnderlyingPrice();
    if (underlyingPrice <= 0)
        underlyingPrice = getUnderlyingPrice();

    // Put-call parity synthetic forward + optional future mid (quantbox design)
    // Both option-implied forwards and future mid participate in the same
    // weighted-average pool, each weighted by 1/spread (tighter = more weight).
    double sum = 0, totalWgt = 0;
    int validCount = 0;
    double time = m_computeTime;

    // Collect valid strikes for second pass (EMA spread update)
    struct ValidStrike {
        StrikeDataPtr sd;
        double callMid;
        double putMid;
        double discount;
    };
    std::vector<ValidStrike> validStrikes;

    // 1. Put-call parity from option strikes
    auto it = m_strikesByExpiry.find(ed->getExpiry());
    if (it != m_strikesByExpiry.end()) {
        double discount = ed->getDiscountFactor();
        if (discount <= 0) discount = 1.0;

        for (const auto& sd : it->second) {
            auto call = sd->call();
            auto put = sd->put();
            if (!call || !put) continue;

            double callBid = call->getBid(), callAsk = call->getAsk();
            double putBid = put->getBid(), putAsk = put->getAsk();
            if (callBid <= 0 || callAsk <= 0 || putBid <= 0 || putAsk <= 0) continue;

            double syn = sd->getStrikePrice() + 0.5 * (callBid + callAsk - putBid - putAsk) / discount;
            double synSprd = (callAsk - putBid - callBid + putAsk) / discount;

            if (callAsk / putBid < 10 && putAsk / callBid < 10 && synSprd > 1e-6) {
                double wgt = 1.0 / synSprd;
                totalWgt += wgt;
                sum += syn * wgt;
                validCount++;

                double callMid = 0.5 * (callBid + callAsk);
                double putMid = 0.5 * (putBid + putAsk);
                validStrikes.push_back({sd, callMid, putMid, discount});
            }
        }
    }

    // 2. Include future mid in weighted average (quantbox design for commodity options)
    if (ed->getIncludeFuture() && ed->isFutureValid()) {
        double futMid = ed->getFutureMid();
        double futSprd = ed->getFutureSpread();
        if (futSprd > 1e-6) {
            double wgt = 1.0 / futSprd;
            totalWgt += wgt;
            sum += futMid * wgt;
            validCount++;
        }
    }

    // If enough valid contributors, use weighted average forward
    if (validCount >= ed->effectiveMinStrikes() && totalWgt > 1e-12) {
        double synFwd = sum / totalWgt;
        ed->setSyntheticForward(synFwd, underlyingPrice, time);
        ed->setForward(synFwd);
        ed->setForwardReady(true);
        ed->setLastValidForwardTime(static_cast<uint64_t>(time * 1e6));

        // Diagnostic: first few forward computations
        {
            static int s_fwdDiag = 0;
            if (s_fwdDiag < 20) {
                s_fwdDiag++;
                WTSLogger::log_by_cat("strategy", LL_INFO,
                    "FWD OK #{} exp={} fwd={:.2f} validCount={} (options={} future={}) underlying={:.2f}",
                    s_fwdDiag, ed->getExpiry(), synFwd, validCount,
                    validCount - (ed->isFutureValid() ? 1 : 0),
                    ed->isFutureValid() ? 1 : 0, underlyingPrice);
            }
        }  // refresh sticky timer

        // Second pass: update per-strike ema_sprd_vs_atmfwd with the SPREAD
        // (strikeForward - synFwd), NOT the absolute strikeForward.
        // P1-c: also capture {strike, strikeForward} for the per-strike
        // forward interpolation in OptionPricer2 (__forwardSpreadFor).
        std::vector<std::pair<double, double>> synPts;
        synPts.reserve(validStrikes.size());
        for (const auto& vs : validStrikes) {
            double strikeForward = vs.sd->getStrikePrice() + (vs.callMid - vs.putMid) / vs.discount;
            double spread = strikeForward - synFwd;
            vs.sd->call()->values().ema_sprd_vs_atmfwd().update(time, spread);
            vs.sd->put()->values().ema_sprd_vs_atmfwd().update(time, spread);
            synPts.emplace_back(vs.sd->getStrikePrice(), strikeForward);
        }
        if (synPts.size() >= 3) {
            std::sort(synPts.begin(), synPts.end(),
                      [](const std::pair<double, double>& a, const std::pair<double, double>& b) {
                          return a.first < b.first;
                      });
            ed->setSynForwardPoints(std::move(synPts));
        }

        return synFwd;
    }

    // Not enough valid contributors: sticky semantics
    // If forward was previously ready, keep it ready until timeout expires
    if (ed->isForwardReady()) {
        uint64_t now = static_cast<uint64_t>(time * 1e6);
        uint64_t lastValid = ed->getLastValidForwardTime();
        // B29 fix: time-of-day clock wraps at midnight (night sessions).
        // Unsigned underflow used to produce ~1.8e19 and instantly kill the
        // sticky forward once per night session. Treat a backwards jump as a
        // new session: restart the freshness timer.
        int64_t dt = static_cast<int64_t>(now) - static_cast<int64_t>(lastValid);
        if (lastValid == 0 || dt < 0) {
            // First degradation (or session rollover): start the timer, keep sticky
            ed->setLastValidForwardTime(now);
            return ed->getForward();
        } else if (static_cast<uint64_t>(dt) > m_forwardStaleTimeoutUs) {
            // Timeout expired: forward is no longer valid
            ed->setForwardReady(false);
            return NAN;
        }
        // Within timeout: keep sticky, return last known forward
        return ed->getForward();
    }

    // Never was ready: forward not available
    return NAN;
}

// ============================================================================
// computeValues
// ============================================================================
void OptionGrid::computeValues(IOptionPricer* pricer) {
    IOptionPricer* p = pricer;
    if (!p) p = m_optionPricer.get();
    if (p) {
        m_atmFwdCache.clear();
        if (p->computeValues(this)) {
            __notifyComputeCompleted();
        }
    }
}

// ============================================================================
// Misc
// ============================================================================
std::vector<uint32_t> OptionGrid::getValidExpiries() const {
    std::vector<uint32_t> result;
    for (const auto& v : m_expiries) result.push_back(v.first);
    return result;
}

void OptionGrid::addListener(IOptionGridListener* listener) {
    m_listeners.push_back(listener);
}

void OptionGrid::removeListener(IOptionGridListener* listener) {
    m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), listener),
                      m_listeners.end());
}

void OptionGrid::__notifyAddOption(const OptionDataPtr& od) {
    for (auto* l : m_listeners) l->onAddOption(od);
}

void OptionGrid::__notifyAddExpiry(const ExpiryDataPtr& ed) {
    for (auto* l : m_listeners) l->onAddExpiry(ed);
}

void OptionGrid::__notifyComputeCompleted() {
    for (auto* l : m_listeners) l->onComputeValuesCompleted(this);
}

// B2: Linear interpolation of the rate curve
double OptionGrid::getRateForDays(int32_t days) const {
    if (m_rateCurve.empty()) return m_riskFreeRate;
    if (m_rateCurve.size() == 1) return m_rateCurve[0].second;

    // m_rateCurve is sorted by days (ascending)
    if (days <= m_rateCurve.front().first) return m_rateCurve.front().second;
    if (days >= m_rateCurve.back().first)  return m_rateCurve.back().second;

    for (size_t i = 1; i < m_rateCurve.size(); ++i) {
        if (days <= m_rateCurve[i].first) {
            double d0 = m_rateCurve[i-1].first,  r0 = m_rateCurve[i-1].second;
            double d1 = m_rateCurve[i].first,    r1 = m_rateCurve[i].second;
            if (d1 == d0) return r1;
            return r0 + (r1 - r0) * (days - d0) / (d1 - d0);
        }
    }
    return m_rateCurve.back().second;
}

} // namespace wt_option
