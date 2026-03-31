/*!
 * \file IOptionPricer.h
 * \project	WonderTrader
 *
 * \brief Unified Interface for Option Pricing Models
 * 
 * Adapted from longbeach CommPricer/CompositeOptionPricer patterns:
 * - ExpiryInfo-based caching for per-expiry parameters
 * - initValuesCompute/computeValue/finalizeCompute lifecycle
 * - computeValues_FAST/SLOW dual-path support
 */
#pragma once

#include <memory>
#include <string>
#include <map>
#include "../Includes/WTSMarcos.h"

namespace wt_option {

class OptionGrid;
class OptionData;
class ExpiryData;
class OptionRisk;
class CurveFitter;
class IVolCurve;
using IVolCurvePtr = std::shared_ptr<IVolCurve>;

class IOptionPricer
{
public:
    virtual ~IOptionPricer() {}

    //=========================================================================
    // Core Compute Interface
    //=========================================================================

    /**
     * @brief Compute theoretical values for the entire grid
     * 
     * Default implementation calls:
     * 1. initValuesCompute(grid) — build ExpiryInfo caches
     * 2. computeValue(option) — for each option
     * 3. finalizeCompute(grid) — cleanup
     */
    virtual bool computeValues(OptionGrid* grid) = 0;

    /**
     * @brief Compute implied values (IV) from market prices
     */
    virtual bool computeImpliedValues(OptionGrid* grid) { return false; }

    //=========================================================================
    // Lifecycle Methods (from longbeach CommPricer pattern)
    //=========================================================================

    /**
     * @brief Initialize computation state for current cycle
     * 
     * Build ExpiryInfo caches: maturity, ATMForward, ATMVol, volCurve.
     * Called at the start of each computeValues() cycle.
     */
    virtual bool initValuesCompute(OptionGrid* grid) { return true; }

    /**
     * @brief Compute values for a single option
     * 
     * Uses cached ExpiryInfo to avoid redundant per-option lookups.
     */
    virtual void computeValue(OptionData* option) {}

    /**
     * @brief Finalize computation after all options processed
     * 
     * Called at the end of each computeValues() cycle.
     */
    virtual void finalizeCompute(OptionGrid* grid) {}

    //=========================================================================
    // Per-Expiry Data Access (cached in ExpiryInfo)
    //=========================================================================

    /**
     * @brief Get ATM Volatility for an expiry
     */
    virtual double getATMVol(uint32_t expiry) const = 0;
    virtual void setATMVol(uint32_t expiry, double vol) = 0;

    /**
     * @brief Get ATM Forward Price for an expiry
     */
    virtual double getATMForward(uint32_t expiry) const = 0;

    /**
     * @brief Get Time to Maturity (years)
     */
    virtual double getMaturity(uint32_t expiry) const = 0;

    /**
     * @brief Get Volatility at specific strike
     */
    virtual double getVol(uint32_t expiry, double strike) const = 0;

    /**
     * @brief Get volatility curve for an expiry
     */
    virtual IVolCurvePtr getVolCurve(uint32_t expiry) const { return nullptr; }

    //=========================================================================
    // Control
    //=========================================================================

    /**
     * @brief Set reprice flag (force full recalculation)
     */
    virtual void setReprice(bool bReprice) = 0;

    /**
     * @brief Called when underlying price changes
     */
    virtual void onUnderlyingPriceChanged(double newPrice) {}

    /**
     * @brief Check if pricer is in panic mode
     */
    virtual bool isPanicked() const { return false; }
};

using IOptionPricerPtr = std::shared_ptr<IOptionPricer>;

} // namespace wt_option
