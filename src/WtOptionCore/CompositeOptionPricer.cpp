/*!
 * \file CompositeOptionPricer.cpp
 * \brief Composite Pricer with FAST/SLOW Dual-Path and Quoting Logic
 * 
 * Adapted from longbeach CompositeCommPricer:
 * - computeValues_FAST: theo-only update using cached vol (~50μs)
 * - computeValues_SLOW: full recalc with IV solve + curve refit (~5ms)
 * - computeValues(): time-based dispatcher
 */
#include "CompositeOptionPricer.h"
#include "OptionData.h"
#include "OptionGrid.h"
#include "OptionRisk.h"
#include "WtOptionStrategy.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include "../Share/TimeUtils.hpp"

namespace wt_option {

// Helper: Round to tick size
static double round_to_tick(double px, double tick_size) {
    if (tick_size <= 1e-9) return px;
    return std::round(px / tick_size) * tick_size;
}

// Helper: Sticky logic (anti-flicker)
static double apply_sticky(double current_px, double new_px, double tick_size) {
    double threshold = tick_size * 0.5;
    if (std::abs(new_px - current_px) < threshold) {
        return current_px;
    }
    return new_px;
}

// Helper: Compute option spread cost
static double getOptionCosts(const OptionValues& values, const ExpiryRiskConfig& config) {
    const OptionGreeks& greeks = values.greeks;
    double delta = std::abs(greeks.delta());
    double vega = greeks.vega();
    
    double sprd_fwd = config.spreadFut;
    double sprd_atmvol = config.spreadVol;
    double sprd_corr = 0.0;
    
    double variance = (delta * sprd_fwd) * (delta * sprd_fwd)
                    + (vega * sprd_atmvol) * (vega * sprd_atmvol)
                    + 2.0 * delta * sprd_fwd * vega * sprd_atmvol * sprd_corr;
                    
    double core_spread = std::sqrt(variance);
    core_spread *= config.spreadMultiplier;
    return core_spread;
}

CompositeOptionPricer::CompositeOptionPricer()
    : m_bReprice(false)
    , m_firstCompute(true)
{
}

CompositeOptionPricer::~CompositeOptionPricer()
{
}

//=============================================================================
// computeValues — time-based dispatcher (from longbeach pattern)
//=============================================================================

bool CompositeOptionPricer::computeValues(OptionGrid* grid)
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
    } else {
        computeValues_FAST(grid);
    }
    
    // Always compute our markets (quotes)
    computeOurMarkets(grid);
    
    m_bReprice = false;
    return true;
}

//=============================================================================
// FAST path — only theo update using cached vol (~50μs target)
// Skips: IV solve, curve fitting, forward recalc, Greeks decay
//=============================================================================

void CompositeOptionPricer::computeValues_FAST(OptionGrid* grid)
{
    if (!m_theoPricer) return;
    
    // Use initValuesCompute to refresh ExpiryInfo caches (cheap: O(n_expiry))
    m_theoPricer->initValuesCompute(grid);
    
    // Compute theo values for all options using cached ExpiryInfo
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

//=============================================================================
// SLOW path — full recalculation (~5ms target)
// Does: IV solve, curve refit, forward recalc, Greeks decay
//=============================================================================

void CompositeOptionPricer::computeValues_SLOW(OptionGrid* grid)
{
    if (!m_theoPricer) return;
    
    // Step 1: Compute implied values (IV solve from market prices)
    m_theoPricer->computeImpliedValues(grid);
    
    // Step 2: Full theoretical computation with fresh ExpiryInfo
    m_theoPricer->setReprice(true);
    m_theoPricer->computeValues(grid);
}

bool CompositeOptionPricer::computeImpliedValues(OptionGrid* grid)
{
    if (m_theoPricer) {
        return m_theoPricer->computeImpliedValues(grid);
    }
    return false;
}

void CompositeOptionPricer::onTick(const char* code, wtp::WTSTickData* tick)
{
    if (m_priceSignal) m_priceSignal->onTick(nullptr, tick);
    if (m_volSignal) m_volSignal->onTick(nullptr, tick);
}

//=============================================================================
// Market Making: computeOurMarkets
//=============================================================================

void CompositeOptionPricer::computeOurMarkets(OptionGrid* grid)
{
    if (!grid) return;
    
    auto& expiries = grid->getExpiries();
    for (auto& pair : expiries) {
        uint32_t expiry = pair.first;
        auto it = m_expiryConfigs.find(expiry);
        if (it == m_expiryConfigs.end()) continue;
        
        const ExpiryRiskConfig* pConfig = &it->second;
        if (!pConfig->enable) continue;
        
        auto expiryData = pair.second;
        if (!expiryData) continue;
        
        // Quote Underlying
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

void CompositeOptionPricer::computeOurMarketsForUnderlying(UnderlyingTradingData* underlying, const ExpiryRiskConfig& config)
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

//=============================================================================
// Alpha & Risk Adjustment (from longbeach)
//=============================================================================

void CompositeOptionPricer::alpha_adjustment(OptionData* option, const ExpiryRiskConfig& config)
{
    OptionValues& values = option->beginUpdateValues();
    values.volBias = 0; 
    
    if (m_priceSignal) {
        double strength = m_priceSignal->getValue();
        double delta = values.greeks.delta();
        values.priceBias += strength * delta * 100.0;
    }
    
    if (m_volSignal) {
        double volStrength = m_volSignal->getValue();
        values.volBias += volStrength;
    }
}

void CompositeOptionPricer::risk_adjustment(OptionData* option, const ExpiryRiskConfig& config)
{
    OptionValues& values = option->beginUpdateValues();
    values.priceBias = 0;
    
    if (!m_risk) return;
    
    uint32_t expiry = option->getExpiry();
    auto expiryGreeks = m_risk->getExpiryGreeks(expiry);
    if (!expiryGreeks) return;
    
    // Delta Risk
    double currentDelta = expiryGreeks->getPortfolioDelta();
    double riskTolDelta = std::max(1.0, config.riskTolDelta);
    double costDelta = config.spreadFut * 0.5; 
    
    double normRiskDelta = std::max(-1.0, std::min(1.0, currentDelta / riskTolDelta));
    double shiftDelta = -normRiskDelta * costDelta;
    
    // Vega Risk
    double currentVega = expiryGreeks->getOptionGreeks().vega();
    double riskTolVega = std::max(1.0, config.riskTolVega);
    double costVega = config.spreadVol * 0.5;
    
    double normRiskVega = std::max(-1.0, std::min(1.0, currentVega / riskTolVega));
    double shiftVega = -normRiskVega * costVega;
    
    const OptionGreeks& greeks = values.greeks;
    values.priceBias = shiftDelta * greeks.delta() + shiftVega * greeks.vega();
}

//=============================================================================
// Option Quote Computation
//=============================================================================

void CompositeOptionPricer::computeOurMarketsForOption(OptionData* option, const ExpiryData* expiryData, const ExpiryRiskConfig& config)
{
    OptionValues& values = option->beginUpdateValues();
    if (!values.isPriced) {
        values.ourBid = 0;
        values.ourAsk = 0;
        values.ourBidSize = 0;
        values.ourAskSize = 0;
        return;
    }
    
    // Adjustments
    alpha_adjustment(option, config);
    risk_adjustment(option, config);
    
    double mid = values.theoreticalPrice;
    double adj = values.priceBias;
    double alpha = values.volBias; 
    
    // Spread
    double cost = getOptionCosts(values, config);
    double bid_spread = cost / 2.0;
    double ask_spread = cost / 2.0;
    
    double tick_size = option->getInfo().tickSize;
    double min_spread = std::max(config.minSpread, tick_size); 
    
    bid_spread = std::max(bid_spread, min_spread / 2.0);
    ask_spread = std::max(ask_spread, min_spread / 2.0);
    
    double shift = alpha + adj; 
    bid_spread = std::max(0.0, bid_spread - shift);
    ask_spread = std::max(0.0, ask_spread + shift);
    
    if (bid_spread + ask_spread < min_spread) {
        double missing = min_spread - (bid_spread + ask_spread);
        bid_spread += missing / 2.0;
        ask_spread += missing / 2.0;
    }
    
    // Inventory Skewing
    double pos_shift = 0.0;
    if (m_risk) {
        auto optRisk = m_risk->getRiskData(option->getCode());
        if (optRisk) {
            double netDelta = optRisk->positionGreeks.delta();
            double netVega = optRisk->positionGreeks.vega();
            
            double optDelta = option->greeks().delta();
            double optVega = option->greeks().vega();

            // Delta Skew if absolute delta > threshold
            if (std::abs(netDelta) > config.maxPositionDelta) {
                // Directional shift: negative when long, positive when short
                double deltaExcess = netDelta > 0 ? (netDelta - config.maxPositionDelta) : (netDelta + config.maxPositionDelta);
                
                // For Calls (positive delta), a long delta position means we want to sell calls -> lower price -> positive pos_shift
                // For Puts (negative delta), a long delta position means we want to buy puts -> higher price -> negative pos_shift
                double globalShift = deltaExcess * config.riskShiftDeltaRatio;
                double localShift = globalShift * std::abs(optDelta); // Weight by the option's specific delta

                double shift_val = std::round(localShift) * tick_size;
                if (option->getRight() == OptionRight::Put) {
                    shift_val = -shift_val;
                }
                pos_shift += shift_val;
            }
        
            // Vega Skew if absolute vega > threshold
            if (std::abs(netVega) > config.maxPositionVega) {
                // Directional shift: negative when long vega, positive when short vega
                // This assumes long vega implies we bought too many options (overall long volatility), so we lower bids to buy less.
                double vegaExcess = netVega > 0 ? (netVega - config.maxPositionVega) : (netVega + config.maxPositionVega);
                
                double globalShift = vegaExcess * config.riskShiftVegaRatio;
                double localShift = globalShift * std::abs(optVega); // Weight by the option's specific vega
                
                pos_shift += std::round(localShift) * tick_size;
            }
        }
    }

    double bid_price = mid - bid_spread - pos_shift;
    double ask_price = mid + ask_spread - pos_shift;
    
    // Rounding
    bid_price = round_to_tick(bid_price, tick_size);
    ask_price = round_to_tick(ask_price, tick_size);
    
    // Trade Shock (Anti-Ping) Protection
    if (m_strategy) {
        std::string code = option->getCode();
        double lastBuy = m_strategy->getLastBuyFillPrice(code);
        double lastSell = m_strategy->getLastSellFillPrice(code);
        uint64_t lastTime = m_strategy->getLastFillTime(code);
        
        // Apply shock widening if recently filled (e.g., within 5 seconds)
        // Ignoring time decay for now, strictly applying tick widening based on last fill price
        if (lastBuy > 0) {
            bid_price = std::min(bid_price, lastBuy - config.shockTicks * tick_size);
            ask_price = std::max(ask_price, lastBuy + tick_size); // Prevent thrashing
        }
        if (lastSell > 0) {
            ask_price = std::max(ask_price, lastSell + config.shockTicks * tick_size);
            bid_price = std::min(bid_price, lastSell - tick_size); // Prevent thrashing
        }
        
        bid_price = std::max(bid_price, tick_size); // Ensure > 0
    }
    
    // Sticky Logic
    if (values.ourBid > 0) bid_price = apply_sticky(values.ourBid, bid_price, tick_size);
    if (values.ourAsk > 0) ask_price = apply_sticky(values.ourAsk, ask_price, tick_size);
    
    // Size
    int32_t bid_size = config.maxOrderSize;
    int32_t ask_size = config.maxOrderSize;
    
    // Delta Range Filter
    double delta = std::abs(values.greeks.delta());
    if (delta < config.deltaMin || delta > config.deltaMax) {
        bid_size = 0;
        ask_size = 0;
    }
    
    // Update Values
    double prevBid = values.ourBid;
    double prevAsk = values.ourAsk;
    int32_t prevBidSize = values.ourBidSize;
    int32_t prevAskSize = values.ourAskSize;

    values.ourBid = std::max(0.0, bid_price);
    values.ourAsk = std::max(tick_size, ask_price);
    values.ourBidSize = bid_size;
    values.ourAskSize = ask_size;
    
    if (values.ourBid >= values.ourAsk) {
        values.ourBid = values.ourAsk - tick_size;
    }

    // Check for changes
    uint64_t now = TimeUtils::getLocalTimeNow();
    bool changed = (std::abs(values.ourBid - prevBid) > 1e-6 ||
                    std::abs(values.ourAsk - prevAsk) > 1e-6 ||
                    values.ourBidSize != prevBidSize ||
                    values.ourAskSize != prevAskSize);

    if (changed) {
        values.quoteChangeTime = now;
    }

    // Track quote statistics
    QuoteSnapshot snap;
    snap.bidPrice = values.ourBid;
    snap.askPrice = values.ourAsk;
    snap.bidSize = values.ourBidSize;
    snap.askSize = values.ourAskSize;
    snap.timestamp = now * 1000; // TimeUtils returns ms, convert to us
    
    // Bilateral validation: valid price/size on both sides
    snap.isBilateralValid = (snap.bidSize > 0 && snap.askSize > 0 && 
                             snap.bidPrice > 0 && snap.askPrice > 0 &&
                             snap.bidPrice < snap.askPrice);
                             
    if (snap.isBilateralValid) {
        snap.spread = snap.askPrice - snap.bidPrice;
        double mid = (snap.askPrice + snap.bidPrice) * 0.5;
        snap.spreadPct = (mid > 0) ? (snap.spread / mid) : 0;
    }

    m_quoteStats.onQuoteUpdate(option->getCode(), snap);
    option->commitUpdateValues();
}

//=============================================================================
// Delegated Methods
//=============================================================================

double CompositeOptionPricer::getATMVol(uint32_t expiry) const {
    return m_theoPricer ? m_theoPricer->getATMVol(expiry) : 0.0;
}
void CompositeOptionPricer::setATMVol(uint32_t expiry, double vol) {
    if (m_theoPricer) m_theoPricer->setATMVol(expiry, vol);
}
double CompositeOptionPricer::getATMForward(uint32_t expiry) const {
    return m_theoPricer ? m_theoPricer->getATMForward(expiry) : 0.0;
}
double CompositeOptionPricer::getMaturity(uint32_t expiry) const {
    return m_theoPricer ? m_theoPricer->getMaturity(expiry) : 0.0;
}
double CompositeOptionPricer::getVol(uint32_t expiry, double strike) const {
    return m_theoPricer ? m_theoPricer->getVol(expiry, strike) : 0.0;
}
IVolCurvePtr CompositeOptionPricer::getVolCurve(uint32_t expiry) const {
    return m_theoPricer ? m_theoPricer->getVolCurve(expiry) : nullptr;
}
void CompositeOptionPricer::setReprice(bool bReprice) {
    m_bReprice = bReprice;
    if (m_theoPricer) m_theoPricer->setReprice(bReprice);
}
void CompositeOptionPricer::setExpiryConfig(uint32_t expiry, const ExpiryRiskConfig& config) {
    m_expiryConfigs[expiry] = config;
}

} // namespace wt_option
