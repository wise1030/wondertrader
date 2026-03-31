#include "GammaScalpOptionPricer.h"
#include "OptionData.h"
#include "OptionGrid.h"
#include "OptionRisk.h"
#include "WtOptionStrategy.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include "../Share/TimeUtils.hpp"
#include "../WTSTools/WTSLogger.h"

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
    const OptionGreeks& greeks = values.greeks;
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
    
    auto& expiries = grid->getExpiries();
    for (auto& pair : expiries) {
        auto expiryData = pair.second;
        if (!expiryData) continue;
        const auto& strikes = expiryData->getStrikes();
        for (const auto& sPair : strikes) {
            auto strikeData = sPair.second;
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
    
    auto& expiries = grid->getExpiries();
    for (auto& pair : expiries) {
        uint32_t expiry = pair.first;
        auto it = m_expiryConfigs.find(expiry);
        if (it == m_expiryConfigs.end()) continue;
        
        const GammaScalpConfig* pConfig = &it->second;
        if (!pConfig->enable) continue;
        
        auto expiryData = pair.second;
        if (!expiryData) continue;
        
        if (pConfig->quoteUnderlying) {
            auto undData = expiryData->getUnderlyingTradingData();
            if (undData) {
                 computeOurMarketsForUnderlying(undData.get(), *pConfig);
            }
        }
        
        const auto& strikes = expiryData->getStrikes();
        for (const auto& sPair : strikes) {
            auto sData = sPair.second;
            if (sData->call()) computeOurMarketsForOption(sData->call().get(), expiryData.get(), *pConfig);
            if (sData->put()) computeOurMarketsForOption(sData->put().get(), expiryData.get(), *pConfig);
        }
    }
}

void GammaScalpOptionPricer::computeOurMarketsForUnderlying(UnderlyingTradingData* underlying, const GammaScalpConfig& config)
{
    UnderlyingValues& values = underlying->beginUpdateValues();
    
    double mid = underlying->getMid();
    if (mid <= 0) mid = underlying->getMarket().last;
    if (mid <= 0) return;
    
    double spread = config.minUnderlyingSpread;
    double bid_price = mid - spread / 2.0;
    double ask_price = mid + spread / 2.0;
    double tick_size = config.minUnderlyingSpread;
    
    values.ourBid = std::floor(bid_price / tick_size) * tick_size;
    values.ourAsk = std::ceil(ask_price / tick_size) * tick_size;
    if (values.ourAsk <= values.ourBid) values.ourAsk = values.ourBid + tick_size;
    values.ourBidSize = config.underlyingOrderSize;
    values.ourAskSize = config.underlyingOrderSize;
    
    underlying->commitUpdateValues();
}

void GammaScalpOptionPricer::gamma_theta_adjustment(OptionData* option, const GammaScalpConfig& config)
{
    OptionValues& values = option->beginUpdateValues();
    values.priceBias = 0;
    
    const OptionGreeks& greeks = values.greeks;
    
    // We favor high Gamma/Theta ratios.
    // Theta is usually negative for long options, Gamma is positive.
    // If we want to buy, higher Gamma and lower |Theta| is better.
    if (std::abs(greeks.theta()) > 1e-6 && greeks.gamma() > 1e-6) {
        double ratio = greeks.gamma() / std::abs(greeks.theta());
        // Simple heuristic bias based on ratio
        // We bias up to +0.005 for high ratio, down to -0.005 for low ratio
        double medianRatio = 0.5; // Example median ratio, strategy dependent
        double skew = (ratio - medianRatio) * 0.01; 
        
        // Clamp the skew bias
        skew = std::max(-0.01, std::min(0.01, skew));
        values.priceBias += skew;
    }
}

void GammaScalpOptionPricer::computeOurMarketsForOption(OptionData* option, const ExpiryData* expiryData, const GammaScalpConfig& config)
{
    OptionValues& values = option->beginUpdateValues();
    if (!values.isPriced) {
        values.ourBid = 0;
        values.ourAsk = 0;
        values.ourBidSize = 0;
        values.ourAskSize = 0;
        return;
    }
    
    gamma_theta_adjustment(option, config);
    
    double mid = values.theoreticalPrice;
    double adj = values.priceBias;
    
    double cost = getOptionCosts(values, config);
    double bid_spread = cost / 2.0;
    double ask_spread = cost / 2.0;
    
    double tick_size = option->getInfo().tickSize;
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
    
    if (m_strategy) {
        std::string code = option->getCode();
        double lastBuy = m_strategy->getLastBuyFillPrice(code);
        double lastSell = m_strategy->getLastSellFillPrice(code);
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
    
    if (values.ourBid > 0) bid_price = apply_sticky(values.ourBid, bid_price, tick_size);
    if (values.ourAsk > 0) ask_price = apply_sticky(values.ourAsk, ask_price, tick_size);
    
    int32_t bid_size = config.maxOrderSize;
    int32_t ask_size = config.maxOrderSize;
    
    double delta = std::abs(values.greeks.delta());
    double currentPos = option->getPosition();
    
    // Deep ITM/OTM filter: only allow closing out existing positions
    if (delta < config.deltaMin || delta > config.deltaMax) {
        if (!config.enableAutoClose) {
            bid_size = 0;
            ask_size = 0;
        } else {
            if (currentPos > 0) {
                bid_size = 0;        // We're long, no more buys
                ask_size = currentPos; // Sell to close
            } else if (currentPos < 0) {
                bid_size = std::abs(currentPos); // Buy to close
                ask_size = 0;       // No more sells
            } else {
                bid_size = 0;
                ask_size = 0;
            }
        }
    }
    
    uint64_t now = TimeUtils::getLocalTimeNow();
    values.ourBid = std::max(0.0, bid_price);
    values.ourAsk = std::max(tick_size, ask_price);
    values.ourBidSize = bid_size;
    values.ourAskSize = ask_size;
    
    if (values.ourBid >= values.ourAsk) {
        values.ourBid = values.ourAsk - tick_size;
    }

    QuoteSnapshot snap;
    snap.bidPrice = values.ourBid;
    snap.askPrice = values.ourAsk;
    snap.bidSize = values.ourBidSize;
    snap.askSize = values.ourAskSize;
    snap.timestamp = now * 1000;
    
    snap.isBilateralValid = (snap.bidSize > 0 && snap.askSize > 0 && 
                             snap.bidPrice > 0 && snap.askPrice > 0 &&
                             snap.bidPrice < snap.askPrice);
                             
    if (snap.isBilateralValid) {
        snap.spread = snap.askPrice - snap.bidPrice;
        double m = (snap.askPrice + snap.bidPrice) * 0.5;
        snap.spreadPct = (m > 0) ? (snap.spread / m) : 0;
    }

    m_quoteStats.onQuoteUpdate(option->getCode(), snap);
    option->commitUpdateValues();
}

void GammaScalpOptionPricer::evaluateDynamicHedging(OptionGrid* grid)
{
    if (!grid || !m_risk || !m_strategy) return;
    
    // Limit hedging frequency to avoid thrashing (e.g. max once per second)
    uint64_t nowMs = TimeUtils::getLocalTimeNow();
    if (nowMs - m_lastHedgeCheckMs < 1000) return;
    
    double undPrice = grid->getUnderlyingPrice();
    if (undPrice <= 0) return;
    
    // 1. Get total portfolio Greeks
    double totalDelta = m_risk->getPortfolioDelta();
    double totalGamma = m_risk->getPositionGreeks().gamma();
    // double totalTheta = m_risk->getPositionGreeks().theta();
    
    // If we have negligible options position, do not hedge.
    if (std::abs(totalGamma) < 1e-4) return;
    
    // 2. Expected Hedging Band Strategy (Whalley-Wilmott)
    // Formula bandwidth ~ (1.5 * e^(-r(T-t)) * Cost / Gamma)^ (1/3)
    // Here we use a simplified empirical threshold approach from the config
    
    // Find the relevant config
    GammaScalpConfig activeConfig;
    if (!m_expiryConfigs.empty()) {
        activeConfig = m_expiryConfigs.begin()->second; // Just using the first one's config for simplicity
    }
    
    double cost = activeConfig.transactionCost;
    double vol = activeConfig.impliedVolatility;
    double riskAversion = activeConfig.hedgeThresholdRisk;
    
    // Optimal Hedging Bandwidth H (approx)
    double H = std::pow((1.5 * cost * undPrice) / (riskAversion * vol * vol), 1.0/3.0);
    // Scale by Gamma
    H = H * totalGamma; // Expected delta deflection
    
    // Ensure a min absolute bandwidth
    H = std::max(H, activeConfig.maxOrderSize * 1.0);
    
    // 3. Check Delta against bandwidth
    if (std::abs(totalDelta) > H) {
        // Trigger hedge to bring Delta closer to 0
        m_lastHedgeCheckMs = nowMs;
        
        // Target delta to hedge (e.g., hedge half the excess or back to 0)
        double hedgeQty = -totalDelta; 
        
        // Round to integer quantity
        int32_t orderQty = static_cast<int32_t>(std::round(std::abs(hedgeQty)));
        if (orderQty == 0) return;
        
        // Send order
        bool isBuy = (hedgeQty > 0);
        
        // Execute at a marketable price (cross worst case spread)
        double execPrice = undPrice;
        if (isBuy) {
            execPrice += activeConfig.minUnderlyingSpread;
        } else {
            execPrice -= activeConfig.minUnderlyingSpread;
        }
        
        WTSLogger::log_by_cat("strategy", LL_INFO, 
            "GammaScalp: Delta %.2f > Band %.2f, Hedging qty %d at %.2f", totalDelta, H, isBuy?orderQty:-orderQty, execPrice);
            
        std::string hedgeCode = grid->getExpiries().empty() ? "" : grid->getExpiries().begin()->second->getUnderlyingCode();
        if(!hedgeCode.empty()) {
             m_strategy->sendOrder(hedgeCode, isBuy, execPrice, orderQty);
        }
    }
}

// Delegations to Standard Pricer
double GammaScalpOptionPricer::getATMVol(uint32_t expiry) const {
    return m_theoPricer ? m_theoPricer->getATMVol(expiry) : 0.0;
}
void GammaScalpOptionPricer::setATMVol(uint32_t expiry, double vol) {
    if (m_theoPricer) m_theoPricer->setATMVol(expiry, vol);
}
double GammaScalpOptionPricer::getATMForward(uint32_t expiry) const {
    return m_theoPricer ? m_theoPricer->getATMForward(expiry) : 0.0;
}
double GammaScalpOptionPricer::getMaturity(uint32_t expiry) const {
    return m_theoPricer ? m_theoPricer->getMaturity(expiry) : 0.0;
}
double GammaScalpOptionPricer::getVol(uint32_t expiry, double strike) const {
    return m_theoPricer ? m_theoPricer->getVol(expiry, strike) : 0.0;
}
IVolCurvePtr GammaScalpOptionPricer::getVolCurve(uint32_t expiry) const {
    return m_theoPricer ? m_theoPricer->getVolCurve(expiry) : nullptr;
}
void GammaScalpOptionPricer::setReprice(bool bReprice) {
    m_bReprice = bReprice;
    if (m_theoPricer) m_theoPricer->setReprice(bReprice);
}
void GammaScalpOptionPricer::setExpiryConfig(uint32_t expiry, const GammaScalpConfig& config) {
    m_expiryConfigs[expiry] = config;
}

} // namespace wt_option
