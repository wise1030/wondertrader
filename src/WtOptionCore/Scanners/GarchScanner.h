/*!
 * \file GarchScanner.h
 * \brief GARCH volatility-based scanner for option trading
 * 
 * Migrated from longbeach/optiontrader/GarchScanner.h
 * Uses GARCH model predictions to identify mispriced options.
 */

#pragma once

#include "IScanModule.h"
#include <map>
#include <deque>

namespace wt_option {

/**
 * @brief GARCH scanner configuration
 */
struct GarchScannerConfig : public ScannerConfig {
    double omega = 0.000001;            // GARCH omega parameter
    double alpha = 0.1;                 // GARCH alpha parameter
    double beta = 0.85;                 // GARCH beta parameter
    int32_t lookbackPeriod = 20;        // Days for volatility estimation
    double volDiffThreshold = 0.05;     // Min vol difference to trigger (5%)
    int32_t maxOpenPositions = 10;      // Maximum open positions
    int32_t maxOrderSize = 100;         // Maximum order size
    int32_t minDaysToExpiry = 5;        // Minimum days to expiry
    int32_t maxDaysToExpiry = 30;       // Maximum days to expiry
    
    /// Get effective max open positions for expiry (mirrors longbeach per_month constraints)
    int32_t getMaxOpenPositions(uint32_t expiry) const {
        auto* ov = getExpiryOverride(expiry);
        if (ov && ScannerExpiryOverrides::isSet(ov->maxPosOpt)) return ov->maxPosOpt;
        return maxOpenPositions;
    }
    
    /// Get effective max order size for expiry
    int32_t getMaxOrderSize(uint32_t expiry) const {
        auto* ov = getExpiryOverride(expiry);
        if (ov && ScannerExpiryOverrides::isSet(ov->maxOrderSize)) return ov->maxOrderSize;
        return maxOrderSize;
    }
};

/**
 * @brief GARCH model for volatility forecasting
 */
class GarchModel {
public:
    GarchModel(double omega = 0.000001, double alpha = 0.1, double beta = 0.85);
    
    /**
     * @brief Add new return observation
     * @param ret Daily return (e.g., 0.01 for 1%)
     */
    void addReturn(double ret);
    
    /**
     * @brief Get current GARCH volatility forecast
     * @return Annualized volatility
     */
    double getVolatility() const;
    
    /**
     * @brief Forecast volatility N days ahead
     * @param days Number of days
     * @return Annualized volatility forecast
     */
    double forecast(int32_t days) const;
    
    /**
     * @brief Reset the model
     */
    void reset();
    
private:
    double m_omega;     // Long-run variance weight
    double m_alpha;     // Shock weight
    double m_beta;      // Persistence weight
    double m_variance;  // Current conditional variance
    double m_longRunVar;// Unconditional variance
    std::deque<double> m_returns;
    int32_t m_maxLookback;
};

/**
 * @brief GARCH-based volatility scanner
 * 
 * Compares GARCH-forecasted volatility with implied volatility
 * to identify mispriced options.
 */
class GarchScanner : public IScanModule {
public:
    GarchScanner(const ScannerConfig& config);
    
    void onStart() override;
    void onStop() override;
    void onTick(const OptionGrid* grid) override;
    void onUnderlyingUpdate(double price) override;
    void onPanic() override;
    
private:
    void updateGarchModel(double price);
    void scanOptions(const OptionGrid* grid);
    void evaluateOption(OptionData* option, double garchVol, const OptionGrid* grid);
    
    GarchScannerConfig m_config;
    GarchModel m_garchModel;
    
    uint64_t m_lastScanTime;
    bool m_isPanicked;
    
    double m_lastPrice;
    uint32_t m_lastPriceDate;
    int32_t m_currentOpenPositions;
};

using GarchScannerPtr = std::shared_ptr<GarchScanner>;

} // namespace wt_option
