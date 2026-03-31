/*!
 * \file StandardOptionPricer.cpp
 * \brief Standard Option Pricer with ExpiryInfo Caching
 * 
 * Adapted from longbeach CommPricer:
 * - initValuesCompute() builds ExpiryInfo cache per expiry
 * - computeValue() uses cached params for single-option pricing
 * - computeImpliedValues() solves IV using cached forward/maturity
 */
#include "StandardOptionPricer.h"
#include "OptionData.h"
#include "OptionGrid.h"
#include "Black76.h"
#include "BlackScholes.h"
#include <cmath>
#include <iostream>

namespace wt_option {

//=============================================================================
// ExpiryInfo methods
//=============================================================================

double StandardOptionPricer::ExpiryInfo::getVolForStrike(double strike) const
{
    if (volCurve && volCurve->isInitialized()) {
        double v = volCurve->getVol(strike, atmForward);
        if (v > 0.0001) return v;
    }
    return atmVol;
}

void StandardOptionPricer::ExpiryInfo::computeForward(double underlyingPrice)
{
    if (underlyingPrice <= 0 || maturity <= 0) return;
    
    // Cost-of-carry forward
    atmForward = underlyingPrice * std::exp((riskFreeRate - dividendYield) * maturity);
    
    // Try synthetic forward from put-call parity
    if (expiryData) {
        double synForward = expiryData->calculateSyntheticForward();
        if (synForward > 0) {
            atmForward = synForward;
        }
    }
    
    discount = std::exp(-riskFreeRate * maturity);
}

//=============================================================================
// StandardOptionPricer
//=============================================================================

StandardOptionPricer::StandardOptionPricer()
    : m_bReprice(false)
{
}

StandardOptionPricer::~StandardOptionPricer()
{
}

//=============================================================================
// Lifecycle: initValuesCompute → computeValue → finalizeCompute
// (Adapted from longbeach CommPricer pattern)
//=============================================================================

bool StandardOptionPricer::initValuesCompute(OptionGrid* grid)
{
    if (!grid) return false;
    
    m_currentGrid = grid;
    // Optimization: Don't clear(), instead reuse existing entries
    // Mark all as invalid first or just update them?
    // Better to mark active expiries and remove stale ones later
    std::vector<uint32_t> activeExpiries;
    
    double underlying = grid->getUnderlyingPrice();
    
    auto& expiries = grid->getExpiries();
    for (auto& pair : expiries) {
        auto expiryData = pair.second;
        if (!expiryData) continue;
        
        activeExpiries.push_back(pair.first);
        
        // Get or create ExpiryInfo
        ExpiryInfo& ei = m_expiryInfo[pair.first]; // Reuse or insert
        
        ei.expiry = pair.first;
        ei.expiryData = expiryData.get();
        ei.maturity = expiryData->getTimeToExpiry();
        ei.riskFreeRate = expiryData->getRiskFreeRate();
        ei.dividendYield = expiryData->getDividendYield();
        ei.atmVol = expiryData->getATMVol();
        
        // Get vol curve
        ei.volCurve = grid->getVolatilityCurve(pair.first);
        
        // Compute forward price
        ei.computeForward(underlying);
        
        // Update expiry data's ATM forward
        if (ei.atmForward > 0) {
            expiryData->setATMForward(ei.atmForward);
        }
    }
    
    // Remove stale expiries
    for (auto it = m_expiryInfo.begin(); it != m_expiryInfo.end(); ) {
        bool isActive = false;
        for (uint32_t exp : activeExpiries) {
            if (exp == it->first) { isActive = true; break; }
        }
        
        if (!isActive) {
            it = m_expiryInfo.erase(it);
        } else {
            ++it;
        }
    }
    
    return true;
}

void StandardOptionPricer::computeValue(OptionData* option)
{
    if (!option) return;
    
    uint32_t expiry = option->getExpiry();
    auto it = m_expiryInfo.find(expiry);
    if (it == m_expiryInfo.end()) return;
    
    const ExpiryInfo& ei = it->second;
    if (!ei.isValid()) return;
    
    calculateTheoretical(option, ei);
    option->commitUpdateValues(); // Atomic swap
}

void StandardOptionPricer::finalizeCompute(OptionGrid* grid)
{
    // No cleanup needed — ExpiryInfo cache remains valid for
    // accessor methods (getATMVol, getATMForward, etc.)
    m_currentGrid = nullptr;
}

//=============================================================================
// computeValues / computeImpliedValues
//=============================================================================

bool StandardOptionPricer::computeValues(OptionGrid* grid)
{
    if (!grid) return false;
    
    // Phase 1: Build ExpiryInfo caches
    initValuesCompute(grid);
    
    // Phase 2: Compute each option using cached ExpiryInfo
    auto& expiries = grid->getExpiries();
    for (auto& pair : expiries) {
        auto expiryData = pair.second;
        if (!expiryData) continue;
        
        const auto& strikes = expiryData->getStrikes();
        for (const auto& sPair : strikes) {
            auto strikeData = sPair.second;
            if (!strikeData) continue;
            
            auto call = strikeData->call();
            if (call) computeValue(call.get());
            
            auto put = strikeData->put();
            if (put) computeValue(put.get());
        }
    }
    
    // Phase 3: Finalize
    finalizeCompute(grid);
    
    return true;
}

bool StandardOptionPricer::computeImpliedValues(OptionGrid* grid)
{
    if (!grid) return false;
    
    // Build ExpiryInfo if not already cached
    if (m_expiryInfo.empty()) {
        initValuesCompute(grid);
    }
    
    auto& expiries = grid->getExpiries();
    for (auto& pair : expiries) {
        auto expiryData = pair.second;
        if (!expiryData) continue;
        
        auto it = m_expiryInfo.find(pair.first);
        if (it == m_expiryInfo.end()) continue;
        const ExpiryInfo& ei = it->second;
        if (!ei.isValid()) continue;
        
        const auto& strikes = expiryData->getStrikes();
        for (const auto& sPair : strikes) {
            auto strikeData = sPair.second;
            if (!strikeData) continue;
            
            auto call = strikeData->call();
            if (call) {
                calculateImplied(call.get(), ei);
                call->commitUpdateValues(); // Atomic swap
            }
            
            auto put = strikeData->put();
            if (put) {
                calculateImplied(put.get(), ei);
                put->commitUpdateValues(); // Atomic swap
            }
        }
    }
    return true;
}

//=============================================================================
// Pricing Calculations (using cached ExpiryInfo)
//=============================================================================

void StandardOptionPricer::calculateTheoretical(OptionData* option, const ExpiryInfo& ei)
{
    if (!option) return;
    
    // Write to secondary buffer lock-free
    OptionValues& val = option->beginUpdateValues();
    double K = option->getStrike();
    double vol = ei.getVolForStrike(K);
    
    if (vol <= 0.0001) vol = 0.0001;
    
    if (m_useBlack76) {
        Black76::calculate(ei.atmForward, K, ei.maturity, ei.riskFreeRate, vol, 
                          option->getRight(), val);
    } else {
        BlackScholes::calculate(ei.atmForward, K, ei.maturity, ei.riskFreeRate, 
                               0.0, vol, option->getRight(), val);
    }
    
    val.impliedVol = vol;
    val.underlyingPrice = ei.atmForward;
    val.timeToExpiry = ei.maturity;
    val.isPriced = true;
}

void StandardOptionPricer::calculateImplied(OptionData* option, const ExpiryInfo& ei)
{
    if (!option) return;
    
    double price = (option->getBid() + option->getAsk()) / 2.0;
    if (price <= 0) price = option->getLast();
    if (price <= 0) return;
    
    double K = option->getStrike();
    double iv = 0.0;
    
    if (m_useBlack76) {
        iv = Black76::impliedVol(price, ei.atmForward, K, ei.maturity, 
                                ei.riskFreeRate, option->getRight());
    } else {
        iv = BlackScholes::impliedVol(price, ei.atmForward, K, ei.maturity, 
                                     ei.riskFreeRate, 0.0, option->getRight());
    }
    
    if (iv > 0) {
        option->beginUpdateValues().impliedVol = iv;
    }
}

//=============================================================================
// Accessor Methods (return from cached ExpiryInfo)
//=============================================================================

const StandardOptionPricer::ExpiryInfo* StandardOptionPricer::getExpiryInfo(uint32_t expiry) const
{
    auto it = m_expiryInfo.find(expiry);
    return (it != m_expiryInfo.end()) ? &it->second : nullptr;
}

double StandardOptionPricer::getATMVol(uint32_t expiry) const
{
    auto* ei = getExpiryInfo(expiry);
    return ei ? ei->atmVol : 0.0;
}

void StandardOptionPricer::setATMVol(uint32_t expiry, double vol)
{
    auto it = m_expiryInfo.find(expiry);
    if (it != m_expiryInfo.end()) {
        it->second.atmVol = vol;
    }
}

double StandardOptionPricer::getATMForward(uint32_t expiry) const
{
    auto* ei = getExpiryInfo(expiry);
    return ei ? ei->atmForward : 0.0;
}

double StandardOptionPricer::getMaturity(uint32_t expiry) const
{
    auto* ei = getExpiryInfo(expiry);
    return ei ? ei->maturity : 0.0;
}

double StandardOptionPricer::getVol(uint32_t expiry, double strike) const
{
    auto* ei = getExpiryInfo(expiry);
    return ei ? ei->getVolForStrike(strike) : 0.0;
}

IVolCurvePtr StandardOptionPricer::getVolCurve(uint32_t expiry) const
{
    auto* ei = getExpiryInfo(expiry);
    return ei ? ei->volCurve : nullptr;
}

} // namespace wt_option
