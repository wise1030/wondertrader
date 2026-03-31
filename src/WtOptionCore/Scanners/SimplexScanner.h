/*!
 * \file SimplexScanner.h
 * \brief Simplex optimization scanner for option trading
 * 
 * Migrated from longbeach/optiontrader/SimplexScanner.h
 * Uses linear programming to find optimal option portfolios.
 */

#pragma once

#include "IScanModule.h"
#include "../OptionGrid.h"
#include <vector>

namespace wt_option {

/**
 * @brief Simplex scanner configuration
 */
struct SimplexScannerConfig : public ScannerConfig {
    double minProfit = 100;              // Min portfolio profit
    double maxRisk = 10000;              // Max portfolio risk
    int32_t maxLegs = 4;                 // Max legs in portfolio
    double deltaLimit = 100;             // Delta constraint
    double gammaLimit = 50;              // Gamma constraint
    double vegaLimit = 1000;             // Vega constraint
    bool useMarginConstraint = true;
    double marginLimit = 100000;
    
    SimplexScannerConfig() {
        name = "SimplexScanner";
    }
};

/**
 * @brief Portfolio leg
 */
struct PortfolioLeg {
    OptionDataPtr option;
    int32_t quantity;
    double price;
    double contribution;
};

/**
 * @brief Optimal portfolio result
 */
struct OptimalPortfolio {
    std::vector<PortfolioLeg> legs;
    double expectedProfit;
    double maxLoss;
    double delta;
    double gamma;
    double vega;
    double margin;
    bool isValid;
};

/**
 * @brief Simplex scanner
 * 
 * Uses optimization to find best risk/reward option portfolios.
 */
class SimplexScanner : public IScanModule {
public:
    SimplexScanner(const SimplexScannerConfig& config);
    
    void onStart() override;
    void onStop() override;
    void onTick(const OptionGrid* grid) override;
    void onOptionUpdate(OptionData* option) override;
    void onUnderlyingUpdate(double price) override {}
    
    OptimalPortfolio findOptimalPortfolio(OptionGrid* grid);
    
protected:
    // Collect candidate options
    std::vector<OptionDataPtr> getCandidates(OptionGrid* grid);
    
    // Optimization
    bool solveLP(const std::vector<OptionDataPtr>& candidates,
                 OptimalPortfolio& result);
    
    // Constraint checking
    bool checkConstraints(const OptimalPortfolio& portfolio);
    
private:
    SimplexScannerConfig m_config;
    uint64_t m_lastSolveTime;
    OptimalPortfolio m_lastResult;
};

} // namespace wt_option
