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

#include "../Includes/WTSDataDef.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSStruct.h"
#include "../Share/CodeHelper.hpp"
#include "../WTSTools/WTSLogger.h"
#include "../Share/fmtlib.h"

namespace wt_option {

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
// Underlying price
// ============================================================================
double OptionGrid::getUnderlyingPrice() const {
    std::shared_lock<std::shared_mutex> lock(m_priceMutex);
    return m_underlyingPrice;
}

void OptionGrid::setUnderlyingPrice(double price) {
    std::unique_lock<std::shared_mutex> lock(m_priceMutex);
    m_underlyingPrice = price;
}

void OptionGrid::onUnderlyingTick(double price) {
    setUnderlyingPrice(price);
    if (m_optionPricer && price > 0) {
        computeValues(m_optionPricer.get());
    }
}

// ============================================================================
// onTick — tick-driven contract discovery + market update
// ============================================================================
void OptionGrid::onTick(const std::string& stdCode, const wtp::WTSTickData* tick) {
    if (!tick) return;

    // Underlying tick?
    if (stdCode == m_underlyingCode) {
        onUnderlyingTick(tick->price());
        return;
    }

    // Option tick?
    if (!CodeHelper::isStdChnFutOptCode(stdCode.c_str()))
        return;

    // Build market snapshot from tick
    OptionMarket mkt;
    mkt.bid = tick->bidprice(0);
    mkt.ask = tick->askprice(0);
    mkt.last = tick->price();
    mkt.bidSize = tick->bidqty(0);
    mkt.askSize = tick->askqty(0);
    mkt.underlyingPrice = getUnderlyingPrice();

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
                uint32_t curDate = 20260703; // TODO: from stra_get_date()
                ed->updateDaysToExpiration(curDate, ci->getExpireDate());
            }
        }
    }
}

// ============================================================================
// onTick (TickDataRef) — async path, no WTSTickData* dependency
// ============================================================================
void OptionGrid::onTick(const std::string& stdCode, const TickDataRef& tick) {
    // Underlying tick?
    if (stdCode == m_underlyingCode) {
        onUnderlyingTick(tick.price);
        return;
    }

    // Option tick?
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
    od->setExpiryData(ed);

    StrikeDataPtr sd = __findOrCreateStrike(expiry, strike);
    // Wire option into strike
    if (sd) {
        if (right == OR_Call)
            sd->setCall(od);
        else
            sd->setPut(od);
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
    ed->setHedgeCode(m_underlyingCode);
    if (!m_bdMgr && !m_holidays.empty()) {
        ed->setHolidays(&m_holidays);
    }

    // Set days to expiration
    // In live mode: query from IBaseDataMgr::getContract → getExpireDate()
    // In backtest: m_bdMgr is null, approximate from expiry YYYYMM → YYYYMM15
    uint32_t approxExpDate = (expiry / 100) * 10000 + (expiry % 100) * 100 + 15;
    // TODO: currentDate from WT session, expireDate from contracts.json via IBaseDataMgr
    ed->updateDaysToExpiration(20260703, approxExpDate);

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

ExpiryDataPtr OptionGrid::getFrontMonthExpiryData() {
    if (m_frontMonthExpiry) return m_frontMonthExpiry;
    if (!m_expiries.empty()) return m_expiries.begin()->second;
    return nullptr;
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
    return findStrikeFromGrid(expiry, getUnderlyingPrice());
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
    if (m_frontMonth == 0 && !m_expiries.empty())
        m_frontMonth = m_expiries.begin()->first;
    return getAtmForward(m_frontMonth);
}

double OptionGrid::getAtmForward(uint32_t expiry) {
    auto it = m_atmFwdCache.find(expiry);
    if (it != m_atmFwdCache.end()) return it->second;
    ExpiryDataPtr ed = getExpiryData(expiry);
    if (!ed) return NAN;
    double fwd = __getBestSyntheticPrice(ed);
    m_atmFwdCache[expiry] = fwd;
    return fwd;
}

double OptionGrid::__getBestSyntheticPrice(const ExpiryDataPtr& ed) {
    if (!ed) return NAN;

    double upx = getUnderlyingPrice();
    if (upx > 0) {
        ed->setForward(upx);
        ed->setForwardReady(true);
        return upx;
    }

    // Put-call parity fallback
    double sum = 0, totalWgt = 0;
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
            }
        }
    }

    if (totalWgt < 1e-12) {
        ed->setForwardReady(false);
        return NAN;
    }

    double fwd = sum / totalWgt;
    ed->setForward(fwd);
    ed->setForwardReady(true);
    return fwd;
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

} // namespace wt_option
