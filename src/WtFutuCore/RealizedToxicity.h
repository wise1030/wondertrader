/*!
 * \file RealizedToxicity.h
 * \brief Realized Toxicity Detection (Post-trade Analysis)
 * 
 * Handles post-trade toxicity analysis:
 *   - Self-trade calibration results
 *   - Actual adverse selection measurement
 *   - Fill quality analysis
 * 
 * This is the "after the fact" toxicity - measuring actual adverse selection
 * from filled orders. Updated on trade events (lower frequency than Predictive).
 * 
 * Performance: Updated on trade events only (not every tick)
 */
#pragma once

#include <cstdint>
#include "../Includes/WTSMarcos.h"
#include "SelfTradeCalibrator.h"
#include "MarketDataContext.h"

namespace futu {

/// Realized toxicity configuration
struct RealizedToxicityConfig
{
    double      weight;                 ///< Weight for realized toxicity in combined score
    uint32_t    min_samples;            ///< Minimum samples before using realized score
    double      decay_factor;           ///< Time decay factor (per second)
    
    RealizedToxicityConfig()
        : weight(0.4)
        , min_samples(3)
        , decay_factor(0.01) {}
};

/// Realized toxicity result
struct RealizedToxicityResult
{
    double      adverse_ratio;          ///< Ratio of adverse fills
    double      avg_adverse_move;       ///< Average adverse price move (ticks)
    uint32_t    total_fills;            ///< Total fills analyzed
    uint32_t    adverse_fills;          ///< Fills with adverse selection
    double      confidence;             ///< Confidence level (based on sample size)
    double      direction_bias;         ///< Direction bias of adverse fills
    double      decayed_score;          ///< Time-decayed toxicity score
    
    RealizedToxicityResult()
        : adverse_ratio(0), avg_adverse_move(0), total_fills(0), adverse_fills(0)
        , confidence(0), direction_bias(0), decayed_score(0) {}
};

/// Realized Toxicity Detector
class RealizedToxicity
{
public:
    RealizedToxicity();
    ~RealizedToxicity() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const RealizedToxicityConfig& cfg) { _cfg = cfg; }
    const RealizedToxicityConfig& getConfig() const { return _cfg; }
    
    /// Set external SelfTradeCalibrator (owned by strategy)
    void setSelfTradeCalibrator(SelfTradeCalibrator* calibrator) { _calibrator = calibrator; }
    SelfTradeCalibrator* getSelfTradeCalibrator() const { return _calibrator; }
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// Update with calibration result
    void onCalibration(const CalibrationResult& calibration);
    
    /// Update with order book analysis
    void onBookAnalysis(double imbalance_score);
    
    //==========================================================================
    // Analysis
    //==========================================================================
    
    /// Compute realized toxicity
    RealizedToxicityResult analyze() const;
    
    /// Quick toxicity score
    double getToxicityScore() const;
    
    //==========================================================================
    // State
    //==========================================================================
    
    bool hasData() const { return _has_calibration_data; }
    
    void reset();

private:
    RealizedToxicityConfig _cfg;
    SelfTradeCalibrator* _calibrator = nullptr;
    
    // Calibration data
    CalibrationResult _latest_calibration;
    BookAnalysisResult _latest_book;
    bool _has_calibration_data = false;
    bool _has_book_data = false;
    
    // Cached result
    mutable RealizedToxicityResult _cached_result;
    mutable bool _cache_dirty = true;
    
    void updateCache() const;
};

} // namespace futu
