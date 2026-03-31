/*!
 * \file SimplexScanner.cpp
 * \brief Simplex scanner implementation
 */

#include "SimplexScanner.h"
#include <algorithm>
#include <cmath>

namespace wt_option {

SimplexScanner::SimplexScanner(const SimplexScannerConfig& config)
    : IScanModule("SimplexScanner")
    , m_config(config)
    , m_lastSolveTime(0)
{
    m_lastResult.isValid = false;
}

void SimplexScanner::onStart() {
    setEnabled(true);
}

void SimplexScanner::onStop() {
    setEnabled(false);
}

void SimplexScanner::onTick(const OptionGrid* grid) {
    if (!isEnabled() || !grid) return;
    
    // Run optimization periodically
    auto result = findOptimalPortfolio(const_cast<OptionGrid*>(grid));
    
    if (result.isValid && result.expectedProfit > m_config.minProfit) {
        // Fire hit for each leg
        for (const auto& leg : result.legs) {
            ScannerHitEvent event;
            event.option = leg.option.get();
            event.signal = leg.contribution;
            event.reason = getName() + ": Simplex optimal: qty=" + std::to_string(leg.quantity);
            notifyHit(event);
        }
    }
}

void SimplexScanner::onOptionUpdate(OptionData* option) {
    // Individual updates don't trigger recalc
}

std::vector<OptionDataPtr> SimplexScanner::getCandidates(OptionGrid* grid) {
    std::vector<OptionDataPtr> candidates;
    
    grid->forEachOption([&candidates, this](OptionData* option) {
        if (!option) return;
        
        // Filter by liquidity
        double spread = option->getAsk() - option->getBid();
        double mid = option->getMid();
        if (mid > 0 && spread / mid < 0.1) {  // Max 10% spread
            auto strike = option->getStrikeData();
            if (strike) {
                bool isCall = (option->getInfo().right == OptionRight::Call);
                auto& optPtr = isCall ? strike->call() : strike->put();
                if (optPtr) candidates.push_back(optPtr);
            }
        }
    });
    
    return candidates;
}

OptimalPortfolio SimplexScanner::findOptimalPortfolio(OptionGrid* grid) {
    OptimalPortfolio result;
    result.isValid = false;
    
    if (!grid) return result;
    
    auto candidates = getCandidates(grid);
    if (candidates.size() < 2) return result;
    
    // Simplified optimization: find best risk/reward combinations
    // Full implementation would use actual LP solver
    
    // Sort by expected profit contribution
    std::sort(candidates.begin(), candidates.end(),
        [](const OptionDataPtr& a, const OptionDataPtr& b) {
            double profitA = a->getTheoPrice() - a->getAsk();
            double profitB = b->getTheoPrice() - b->getAsk();
            return profitA > profitB;
        });
    
    // Build portfolio greedily
    double totalDelta = 0;
    double totalGamma = 0;
    double totalVega = 0;
    
    for (const auto& option : candidates) {
        if (result.legs.size() >= static_cast<size_t>(m_config.maxLegs)) {
            break;
        }
        
        const auto& greeks = option->greeks();
        
        // Check if adding this option would violate constraints
        if (std::abs(totalDelta + greeks.delta()) > m_config.deltaLimit) {
            continue;
        }
        if (std::abs(totalGamma + greeks.gamma()) > m_config.gammaLimit) {
            continue;
        }
        if (std::abs(totalVega + greeks.vega()) > m_config.vegaLimit) {
            continue;
        }
        
        PortfolioLeg leg;
        leg.option = option;
        leg.quantity = 1;
        leg.price = option->getAsk();
        leg.contribution = option->getTheoPrice() - option->getAsk();
        
        result.legs.push_back(leg);
        result.expectedProfit += leg.contribution;
        
        totalDelta += greeks.delta();
        totalGamma += greeks.gamma();
        totalVega += greeks.vega();
    }
    
    result.delta = totalDelta;
    result.gamma = totalGamma;
    result.vega = totalVega;
    result.isValid = !result.legs.empty();
    
    m_lastResult = result;
    return result;
}

bool SimplexScanner::solveLP(const std::vector<OptionDataPtr>& candidates,
                              OptimalPortfolio& result) {
    // Placeholder for actual LP solver integration
    // Would use LpSolver from optioncore
    return false;
}

bool SimplexScanner::checkConstraints(const OptimalPortfolio& portfolio) {
    if (std::abs(portfolio.delta) > m_config.deltaLimit) return false;
    if (std::abs(portfolio.gamma) > m_config.gammaLimit) return false;
    if (std::abs(portfolio.vega) > m_config.vegaLimit) return false;
    if (m_config.useMarginConstraint && portfolio.margin > m_config.marginLimit) {
        return false;
    }
    return true;
}

} // namespace wt_option
