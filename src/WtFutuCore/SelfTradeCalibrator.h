/*!
 * \file SelfTradeCalibrator.h
 * \brief Self-Trade Calibration for Toxicity Detection
 * 
 * Uses self-trade fills as ground truth for calibration:
 *   - Records fill prices and tracks subsequent price movements
 *   - Calculates realized adverse selection ratio
 *   - Provides calibration signals for synthetic transaction inference
 * 
 * This component serves as the "Ground Truth" for the overall signal fusion,
 * since it uses actual trading results rather than inferred data.
 */
#pragma once

#include <string>
#include <cstdint>
#include "../Includes/WTSMarcos.h"
#include "../Includes/FasterDefs.h"
#include "../Share/RingBuffer.hpp"

namespace futu {

//==============================================================================
// Fill Record
//==============================================================================

/// Record of a self-trade fill for analysis
struct SelfFillRecord
{
    std::string code;           ///< Contract code
    uint64_t fill_time;         ///< Fill timestamp
    double fill_price;          ///< Fill price
    double fill_qty;            ///< Fill quantity
    bool is_buy;                ///< True if we bought
    
    // Market state at fill
    double mid_at_fill;         ///< Mid price at fill
    double spread_at_fill;      ///< Spread at fill
    
    // Post-fill analysis
    double price_move_after;    ///< Price move after fill (in ticks)
    bool was_adverse;           ///< True if adverse selection occurred
    double toxicity_score;      ///< Individual toxicity score
    
    // Timing
    uint64_t analysis_time;     ///< When this record was analyzed
    
    SelfFillRecord()
        : fill_time(0), fill_price(0), fill_qty(0), is_buy(false)
        , mid_at_fill(0), spread_at_fill(0)
        , price_move_after(0), was_adverse(false), toxicity_score(0)
        , analysis_time(0)
    {}
};

//==============================================================================
// Calibration Result
//==============================================================================

/// Calibration result from self-trade analysis
struct CalibrationResult
{
    double direction_bias;      ///< Direction bias [-1, 1], positive = buy bias
    double toxicity_level;      ///< Overall toxicity level [0, 1]
    bool high_toxicity;         ///< High toxicity warning flag
    int recommended_side;       ///< Recommended side (1=buy, -1=sell, 0=neutral)
    
    // Statistics
    double sample_size;         ///< Number of samples used
    double confidence;          ///< Confidence of calibration [0, 1]
    
    // Breakdown
    double buy_adverse_ratio;   ///< Adverse ratio for buys
    double sell_adverse_ratio;  ///< Adverse ratio for sells
    
    CalibrationResult()
        : direction_bias(0), toxicity_level(0), high_toxicity(false)
        , recommended_side(0), sample_size(0), confidence(0)
        , buy_adverse_ratio(0), sell_adverse_ratio(0)
    {}
};

//==============================================================================
// Toxicity Metrics (Extended)
//==============================================================================

/// Extended toxicity metrics from self-trade calibration
struct SelfTradeToxicityMetrics
{
    double predictive_toxicity;     ///< Pre-trade toxicity estimate
    double realized_toxicity;       ///< Post-trade realized toxicity
    double toxicity_score;          ///< Combined score
    bool is_toxic;                  ///< Toxicity flag
    int toxic_side;                 ///< Toxic side (1=avoid sell, -1=avoid buy)
    
    // Detailed stats
    double avg_adverse_move;        ///< Average adverse move (ticks)
    uint32_t total_fills;           ///< Total fills analyzed
    uint32_t adverse_fills;         ///< Number of adverse fills
    
    SelfTradeToxicityMetrics()
        : predictive_toxicity(0), realized_toxicity(0), toxicity_score(0)
        , is_toxic(false), toxic_side(0)
        , avg_adverse_move(0), total_fills(0), adverse_fills(0)
    {}
};

//==============================================================================
// Configuration
//==============================================================================

struct SelfTradeCalibratorConfig
{
    uint32_t lookback_trades;       ///< Number of trades to look back
    uint32_t toxicity_window_ms;    ///< Time window for toxicity calculation
    double adverse_threshold;       ///< Threshold for adverse selection
    uint32_t min_samples;           ///< Minimum samples for calibration
    double tick_size;               ///< Contract tick size
    double move_threshold_ticks;    ///< Price move threshold in ticks
    
    SelfTradeCalibratorConfig()
        : lookback_trades(50)
        , toxicity_window_ms(5000)
        , adverse_threshold(0.6)
        , min_samples(5)
        , tick_size(1.0)
        , move_threshold_ticks(1.0)
    {}
};

//==============================================================================
// Self Trade Calibrator
//==============================================================================

class SelfTradeCalibrator
{
public:
    SelfTradeCalibrator();
    ~SelfTradeCalibrator() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const SelfTradeCalibratorConfig& config) { _config = config; }
    const SelfTradeCalibratorConfig& getConfig() const { return _config; }
    
    void setTickSize(double tickSize) { _config.tick_size = tickSize; }
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// Record a fill for calibration
    void recordFill(
        const std::string& code,
        double price, double qty, bool is_buy,
        double mid_price, double spread,
        uint64_t timestamp
    );
    
    /// Update with new tick (to track price after fills)
    void onTick(const std::string& code, double mid_price, uint64_t timestamp);
    
    //==========================================================================
    // Analysis
    //==========================================================================
    
    /// Get calibration result for a contract
    CalibrationResult getCalibration(const std::string& code) const;
    
    /// Get toxicity metrics for a contract
    SelfTradeToxicityMetrics getToxicityMetrics(const std::string& code) const;
    
    /// Get quick toxicity score
    double getToxicityScore(const std::string& code) const;
    
    /// Check if high toxicity
    bool isHighToxicity(const std::string& code) const;
    
    //==========================================================================
    // Statistics
    //==========================================================================
    
    /// Get fill history for a contract (returns pointer to RingBuffer data)
    /// Note: Returns nullptr if contract not found
    const RingBuffer<SelfFillRecord, 128>* getFillHistory(const std::string& code) const;
    
    /// Get sample count for a contract
    uint32_t getSampleCount(const std::string& code) const;
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void reset();
    void resetContract(const std::string& code);
    
    /// Reset calibration for a contract (clear history, allow fresh start after toxicity cooloff)
    void resetCalibration(const std::string& code);
    
    /// Apply time-based decay - reduce weight of old fills
    void decayCalibration(const std::string& code, uint64_t current_time, uint64_t decay_window_ms = 30000);
    
private:
    SelfTradeCalibratorConfig _config;
    
    // Fill history per contract (using RingBuffer for performance)
    struct ContractFillState {
        RingBuffer<SelfFillRecord, 128> fill_history;  // capacity must be power of 2
        double mid_price;
        uint64_t timestamp;
        mutable CalibrationResult cached_result;
        mutable bool cache_dirty;
        
        ContractFillState() : mid_price(0), timestamp(0), cache_dirty(true) {}
    };
    wtp::wt_hashmap<std::string, ContractFillState> _contract_states;
    
    //==========================================================================
    // Internal Methods
    //==========================================================================
    
    /// Analyze fill records for toxicity
    void analyzeFills(const std::string& code) const;
    
    /// Calculate realized toxicity for a single fill
    double calculateRealizedToxicity(const SelfFillRecord& fill, double current_mid) const;
    
    /// Check if fill was adverse selection
    bool checkAdverse(const SelfFillRecord& fill, double current_mid) const;
    
    /// Prune old fills from history
    void pruneHistory(const std::string& code, uint64_t current_time);
};

} // namespace futu
