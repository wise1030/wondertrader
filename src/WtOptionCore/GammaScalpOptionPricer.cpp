/*!
 * \file GammaScalpOptionPricer.cpp
 * \brief Gamma Scalping Pricer implementation
 *
 * Updated: writes to OptionValues::ourMarket() (MultiMarket) for CTG compatibility,
 * uses function callbacks instead of direct strategy dependency.
 */
#include "GammaScalpOptionPricer.h"
#include "OptionData.h"
#include "OptionGrid.h"
#include "OptionRisk.h"
#include "../Share/TimeUtils.hpp"
#include "../WTSTools/WTSLogger.h"
#include <algorithm>
#include <cmath>

namespace wt_option {

static double round_to_tick(double px, double tick_size) {
    if (tick_size <= 1e-9) return px;
    return std::round(px / tick_size) * tick_size;
}

static double apply_sticky(double current_px, double new_px, double tick_size) {
    double threshold = tick_size * 0.5;
    if (std::abs(new_px - current_px) < threshold) {
        return current_px;
    }
    return new_px;
}

static double getOptionCosts(const OptionValues& values, const GammaScalpConfig& config) {
    const OptionGreeks& greeks = values.greeks();
    double delta = std::abs(greeks.delta());
    double vega = greeks.vega();

    double variance = (delta * config.spreadFut) * (delta * config.spreadFut)
                    + (vega * config.spreadVol) * (vega * config.spreadVol);

    double core_spread = std::sqrt(variance);
    core_spread *= config.spreadMultiplier;
    return core_spread;
}

GammaScalpOptionPricer::GammaScalpOptionPricer()
    : m_bReprice(false)
    , m_firstCompute(true)
{
}

GammaScalpOptionPricer::~GammaScalpOptionPricer()
{
}

bool GammaScalpOptionPricer::computeValues(OptionGrid* grid)
{
    auto now = clock_t::now();
    bool doSlow = m_bReprice || m_firstCompute;

    if (!doSlow && !m_firstCompute) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastSlowCompute).count();
        doSlow = (elapsed >= static_cast<int64_t>(m_slowComputePeriodMs));
    }

    if (doSlow) {
        computeValues_SLOW(grid);
        m_lastSlowCompute = now;
        m_firstCompute = false;

        // Evaluate dynamic hedge checking on the slow path (~100ms)
        evaluateDynamicHedging(grid);
    } else {
        computeValues_FAST(grid);
    }

    computeOurMarkets(grid);
    m_bReprice = false;
    return true;
}

void GammaScalpOptionPricer::computeValues_FAST(OptionGrid* grid)
{
    if (!m_theoPricer) return;
    m_theoPricer->initValuesCompute(grid);

    for (auto& pair : grid->expiries()) {
        auto expiryData = pair.second;
        if (!expiryData) continue;
        auto strikes = grid->getStrikesByExpiry(pair.first);
        for (const auto& strikeData : strikes) {
            if (!strikeData) continue;
            if (strikeData->call()) m_theoPricer->computeValue(strikeData->call().get());
            if (strikeData->put()) m_theoPricer->computeValue(strikeData->put().get());
        }
    }
    m_theoPricer->finalizeCompute(grid);
}

void GammaScalpOptionPricer::computeValues_SLOW(OptionGrid* grid)
{
    if (!m_theoPricer) return;
    m_theoPricer->computeImpliedValues(grid);
    m_theoPricer->setReprice(true);
    m_theoPricer->computeValues(grid);
}

bool GammaScalpOptionPricer::computeImpliedValues(OptionGrid* grid)
{
    if (m_theoPricer) {
        return m_theoPricer->computeImpliedValues(grid);
    }
    return false;
}

void GammaScalpOptionPricer::onTick(const char* code, wtp::WTSTickData* tick)
{
    // Implementation not necessary unless alpha signals are used
}

void GammaScalpOptionPricer::computeOurMarkets(OptionGrid* grid)
{
    if (!grid) return;

    for (auto& pair : grid->expiries()) {
        uint32_t expiry = pair.first;
        auto it = m_expiryConfigs.find(expiry);
        if (it == m_expiryConfigs.end()) continue;

        const GammaScalpConfig* pConfig = &it->second;
        if (!pConfig->enable) continue;

        auto expiryData = pair.second;
        if (!expiryData) continue;

        if (pConfig->quoteUnderlying) {
            auto undData = expiryData->getHedgeUTD();
            if (undData) {
                 computeOurMarketsForUnderlying(undData, *pConfig);
            }
        }

        auto strikes = grid->getStrikesByExpiry(expiry);
        for (const auto& sData : strikes) {
            if (sData->call()) computeOurMarketsForOption(sData->call().get(), expiryData.get(), *pConfig);
            if (sData->put()) computeOurMarketsForOption(sData->put().get(), expiryData.get(), *pConfig);
        }
    }
}

void GammaScalpOptionPricer::computeOurMarketsForUnderlying(UnderlyingTradingData* underlying, const GammaScalpConfig& config)
{
    double mid = underlying->getMid();
    if (mid <= 0) return;

    double spread = config.minUnderlyingSpread;
    double bid_price = mid - spread / 2.0;
    double ask_price = mid + spread / 2.0;
    double tick_size = config.minUnderlyingSpread;

    double ourBid = std::floor(bid_price / tick_size) * tick_size;
    double ourAsk = std::ceil(ask_price / tick_size) * tick_size;
    if (ourAsk <= ourBid) ourAsk = ourBid + tick_size;

    MultiMarket& mkt = underlying->ourMarket();
    mkt.clear();
    mkt.setBid(PriceSize(ourBid, config.underlyingOrderSize));
    mkt.setAsk(PriceSize(ourAsk, config.underlyingOrderSize));
}

void GammaScalpOptionPricer::gamma_theta_adjustment(OptionData* option, const GammaScalpConfig& config)
{
    OptionValues& values = option->values(0);
    const OptionGreeks& greeks = values.greeks();

    // We favor high Gamma/Theta ratios.
    if (std::abs(greeks.theta()) > 1e-6 && greeks.gamma() > 1e-6) {
        double ratio = greeks.gamma() / std::abs(greeks.theta());
        double medianRatio = 0.5;
        double skew = (ratio - medianRatio) * 0.01;
        skew = std::max(-0.01, std::min(0.01, skew));
        // Store skew as alpha total (used in bid/ask spread adjustment below)
        values.alpha().total = skew;
    }
}

void GammaScalpOptionPricer::computeOurMarketsForOption(OptionData* option, const ExpiryData* expiryData, const GammaScalpConfig& config)
{
    OptionValues& values = option->values(0);
    MultiMarket& mkt = values.ourMarket();

    // Save previous quotes BEFORE clearing (for sticky price logic)
    PriceSize prevBid = mkt.getBestBid();
    PriceSize prevAsk = mkt.getBestAsk();
    mkt.clear();

    if (!values.isPriced()) {
        return;
    }

    gamma_theta_adjustment(option, config);

    double mid = values.theo();
    double adj = values.alpha().total;

    double cost = getOptionCosts(values, config);
    double bid_spread = cost / 2.0;
    double ask_spread = cost / 2.0;

    double tick_size = option->getTickSize();
    if (tick_size <= 0) tick_size = 1e-6;
    double min_spread = std::max(config.minSpread, tick_size);

    bid_spread = std::max(bid_spread, min_spread / 2.0);
    ask_spread = std::max(ask_spread, min_spread / 2.0);

    // Applying the bias to skew spread
    bid_spread = std::max(0.0, bid_spread - adj);
    ask_spread = std::max(0.0, ask_spread + adj);

    if (bid_spread + ask_spread < min_spread) {
        double missing = min_spread - (bid_spread + ask_spread);
        bid_spread += missing / 2.0;
        ask_spread += missing / 2.0;
    }

    double bid_price = mid - bid_spread;
    double ask_price = mid + ask_spread;

    bid_price = round_to_tick(bid_price, tick_size);
    ask_price = round_to_tick(ask_price, tick_size);

    // Trade shock back-away (via fill price provider callback)
    if (m_fillPriceProvider) {
        const std::string& code = option->getCode();
        double lastBuy = m_fillPriceProvider(code, true);
        double lastSell = m_fillPriceProvider(code, false);
        if (lastBuy > 0) {
            bid_price = std::min(bid_price, lastBuy - config.shockTicks * tick_size);
            ask_price = std::max(ask_price, lastBuy + tick_size);
        }
        if (lastSell > 0) {
            ask_price = std::max(ask_price, lastSell + config.shockTicks * tick_size);
            bid_price = std::min(bid_price, lastSell - tick_size);
        }
        bid_price = std::max(bid_price, tick_size);
    }

    // Sticky prices (use saved values from before clear)
    if (!prevBid.empty()) bid_price = apply_sticky(prevBid.px(), bid_price, tick_size);
    if (!prevAsk.empty()) ask_price = apply_sticky(prevAsk.px(), ask_price, tick_size);

    int32_t bid_size = config.maxOrderSize;
    int32_t ask_size = config.maxOrderSize;

    double delta = std::abs(values.greeks().delta());
    int32_t currentPos = static_cast<int32_t>(option->getPosition());

    // Deep ITM/OTM filter: only allow closing out existing positions
    if (delta < config.deltaMin || delta > config.deltaMax) {
        if (!config.enableAutoClose) {
            bid_size = 0;
            ask_size = 0;
        } else {
            if (currentPos > 0) {
                bid_size = 0;
                ask_size = currentPos;
            } else if (currentPos < 0) {
                bid_size = std::abs(currentPos);
                ask_size = 0;
            } else {
                bid_size = 0;
                ask_size = 0;
            }
        }
    }

    bid_price = std::max(0.0, bid_price);
    ask_price = std::max(tick_size, ask_price);
    if (bid_price >= ask_price) {
        bid_price = ask_price - tick_size;
    }

    if (bid_size > 0 && bid_price > 0)
        mkt.setBid(PriceSize(bid_price, static_cast<uint32_t>(bid_size)));
    if (ask_size > 0 && ask_price > 0)
        mkt.setAsk(PriceSize(ask_price, static_cast<uint32_t>(ask_size)));
}

void GammaScalpOptionPricer::evaluateDynamicHedging(OptionGrid* grid)
{
    if (!grid || !m_risk || !m_orderSender) return;

    // Limit hedging frequency to avoid thrashing (e.g. max once per second)
    uint64_t nowMs = TimeUtils::getLocalTimeNow();
    if (nowMs - m_lastHedgeCheckMs < 1000) return;

    double undPrice = grid->getUnderlyingPrice();
    if (undPrice <= 0) return;

    // 1. Get total portfolio Greeks
    double totalDelta = m_risk->getPortfolioDelta();
    double totalGamma = m_risk->getPositionGreeks() ? m_risk->getPositionGreeks()->gamma() : 0.0;

    // If we have negligible options position, do not hedge.
    if (std::abs(totalGamma) < 1e-4) return;

    // 2. Expected Hedging Band Strategy (Whalley-Wilmott)
    GammaScalpConfig activeConfig;
    if (!m_expiryConfigs.empty()) {
        activeConfig = m_expiryConfigs.begin()->second;
    }

    double cost = activeConfig.transactionCost;
    double vol = activeConfig.impliedVolatility;
    double riskAversion = activeConfig.hedgeThresholdRisk;

    // Optimal Hedging Bandwidth H (approx)
    double H = std::pow((1.5 * cost * undPrice) / (riskAversion * vol * vol), 1.0/3.0);
    H = H * totalGamma;
    H = std::max(H, activeConfig.maxOrderSize * 1.0);

    // 3. Check Delta against bandwidth
    if (std::abs(totalDelta) > H) {
        m_lastHedgeCheckMs = nowMs;

        double hedgeQty = -totalDelta;
        int32_t orderQty = static_cast<int32_t>(std::round(std::abs(hedgeQty)));
        if (orderQty == 0) return;

        bool isBuy = (hedgeQty > 0);
        double execPrice = undPrice;
        if (isBuy) {
            execPrice += activeConfig.minUnderlyingSpread;
        } else {
            execPrice -= activeConfig.minUnderlyingSpread;
        }

        WTSLogger::log_by_cat("strategy", LL_INFO,
            "GammaScalp: Delta {:.2f} > Band {:.2f}, Hedging qty {} at {:.2f}",
            totalDelta, H, isBuy ? orderQty : -orderQty, execPrice);

        std::string hedgeCode;
        if (!grid->expiries().empty()) {
            hedgeCode = grid->expiries().begin()->second->getUnderlyingCode();
        }
        if (!hedgeCode.empty()) {
            m_orderSender(hedgeCode, isBuy, execPrice, orderQty);
        }
    }
}

// ============================================================================
// IOptionPricer pure virtual implementations (delegate to m_theoPricer)
// ============================================================================

bool GammaScalpOptionPricer::initValuesCompute(OptionGrid* grid) {
    return m_theoPricer ? m_theoPricer->initValuesCompute(grid) : false;
}

void GammaScalpOptionPricer::computeValue(OptionData* option) {
    if (m_theoPricer) m_theoPricer->computeValue(option);
}

void GammaScalpOptionPricer::finalizeCompute(OptionGrid* grid) {
    if (m_theoPricer) m_theoPricer->finalizeCompute(grid);
}

IVolCurvePtr GammaScalpOptionPricer::getVolCurve(uint32_t expiry) const {
    return m_theoPricer ? m_theoPricer->getVolCurve(expiry) : nullptr;
}

IVolCurvePtr GammaScalpOptionPricer::getVolCurve2(uint32_t expiry) const {
    return m_theoPricer ? m_theoPricer->getVolCurve2(expiry) : nullptr;
}

IVolCurvePtr GammaScalpOptionPricer::getFwdCurve(uint32_t expiry) const {
    return m_theoPricer ? m_theoPricer->getFwdCurve(expiry) : nullptr;
}

double GammaScalpOptionPricer::getMaturity(uint32_t expiry) const {
    return m_theoPricer ? m_theoPricer->getMaturity(expiry) : 0.0;
}

double GammaScalpOptionPricer::getATMForward(uint32_t expiry) const {
    return m_theoPricer ? m_theoPricer->getATMForward(expiry) : 0.0;
}

void GammaScalpOptionPricer::setATMVol(uint32_t expiry, double vol) {
    if (m_theoPricer) m_theoPricer->setATMVol(expiry, vol);
}

double GammaScalpOptionPricer::getATMVol(uint32_t expiry) const {
    return m_theoPricer ? m_theoPricer->getATMVol(expiry) : 0.0;
}

void GammaScalpOptionPricer::setReprice(bool bReprice) {
    m_bReprice = bReprice;
    if (m_theoPricer) m_theoPricer->setReprice(bReprice);
}

void GammaScalpOptionPricer::setTraceLevel(int32_t i) {
    if (m_theoPricer) m_theoPricer->setTraceLevel(i);
}

int32_t GammaScalpOptionPricer::getTraceLevel() const {
    return m_theoPricer ? m_theoPricer->getTraceLevel() : 0;
}

void GammaScalpOptionPricer::setExpiryConfig(uint32_t expiry, const GammaScalpConfig& config) {
    m_expiryConfigs[expiry] = config;
}

} // namespace wt_option
