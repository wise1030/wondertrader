/*!
 * \file SyntheticSignalFusion.h
 * \brief Multi-Source Signal Fusion for Synthetic Transaction Data
 * 
 * Fuses signals from multiple sources to create synthetic transaction data:
 *   1. TickTransactionInferer: Tick-level inference
 *   2. MarketDataContext: Depth imbalance signals
 *   3. SelfTradeCalibrator: Ground truth calibration
 * 
 * Uses adaptive weighting based on:
 *   - Market volatility
 *   - Liquidity conditions
 *   - Sample size of self-trade calibration
 */
#pragma once

#include <string>
#include <deque>
#include <cstdint>
#include "../Includes/WTSMarcos.h"
#include "TickTransactionInferer.h"
#include "SelfTradeCalibrator.h"
#include "MarketDataContext.h"

namespace futu {

//==============================================================================
// Fusion Configuration
//==============================================================================

struct FusionConfig
{
    // Base weights (should sum to 1.0)
    double tick_inference_base_weight;      ///< Weight for tick inference
    double book_signal_base_weight;         ///< Weight for order book signal
    double self_trade_base_weight;          ///< Weight for self-trade calibration
    
    // Adaptive parameters
    bool adaptive_weights;                  ///< Enable adaptive weight adjustment
    double min_self_trade_samples;          ///< Minimum samples for self-trade weight
    double volatility_sensitivity;          ///< Sensitivity to volatility (0-1)
    double liquidity_sensitivity;           ///< Sensitivity to liquidity (0-1)
    
    // Confidence thresholds
    double min_confidence;                  ///< Minimum confidence threshold
    double strong_signal_threshold;         ///< Threshold for strong signal
    
    // Window settings
    uint32_t imbalance_window_ms;           ///< Window for imbalance calculation
    
    FusionConfig()
        : tick_inference_base_weight(0.4)
        , book_signal_base_weight(0.4)
        , self_trade_base_weight(0.2)
        , adaptive_weights(true)
        , min_self_trade_samples(5.0)
        , volatility_sensitivity(0.5)
        , liquidity_sensitivity(0.3)
        , min_confidence(0.3)
        , strong_signal_threshold(0.7)
        , imbalance_window_ms(5000)
    {}
};

//==============================================================================
// Depth Imbalance Signal (from MarketDataContext)
// Note: This is now an alias for BookAnalysisResult to unify signal types
//==============================================================================

using DepthImbalanceSignal = BookAnalysisResult;

//==============================================================================
// Synthetic Transaction Data
//==============================================================================

/// Fused synthetic transaction data
struct SyntheticTransactionData
{
    double price;                   ///< Transaction price
    double volume;                  ///< Inferred volume
    bool is_buy_initiated;          ///< Buy-initiated flag
    double confidence;              ///< Overall confidence [0, 1]
    uint64_t timestamp;             ///< Timestamp
    
    // Direction signal (-1 to 1)
    double direction_signal;        ///< Fused direction signal
    
    // Weight breakdown
    double tick_inference_weight;   ///< Actual weight used for tick inference
    double book_signal_weight;      ///< Actual weight used for book signal
    double self_trade_weight;       ///< Actual weight used for self-trade
    
    // Quality indicators
    bool is_strong_signal;          ///< Strong directional signal
    bool has_calibration;           ///< Has self-trade calibration
    
    SyntheticTransactionData()
        : price(0), volume(0), is_buy_initiated(false)
        , confidence(0), timestamp(0)
        , direction_signal(0)
        , tick_inference_weight(0), book_signal_weight(0), self_trade_weight(0)
        , is_strong_signal(false), has_calibration(false)
    {}
};

//==============================================================================
// Fused Trade Imbalance Result
//==============================================================================

/// Trade imbalance result compatible with MicroAlphaEngine
struct FusedTradeImbalance
{
    double net_flow;            ///< Net trade flow (buy - sell)
    double imbalance_ratio;     ///< Normalized imbalance [-1, 1]
    double large_trade_ratio;   ///< Large trade ratio
    double confidence;          ///< Average confidence
    uint64_t timestamp;
    
    // Fusion quality
    double fusion_quality;      ///< Quality of fusion (0-1)
    uint32_t sample_count;      ///< Number of samples used
    
    FusedTradeImbalance()
        : net_flow(0), imbalance_ratio(0), large_trade_ratio(0)
        , confidence(0), timestamp(0)
        , fusion_quality(0), sample_count(0)
    {}
};

//==============================================================================
// Adaptive Weights
//==============================================================================

struct AdaptiveWeights
{
    double tick;        ///< Weight for tick inference
    double book;        ///< Weight for order book
    double self_trade;  ///< Weight for self-trade calibration
    
    AdaptiveWeights()
        : tick(0.4), book(0.4), self_trade(0.2)
    {}
};

//==============================================================================
// Synthetic Signal Fusion
//==============================================================================

class SyntheticSignalFusion
{
public:
    SyntheticSignalFusion();
    explicit SyntheticSignalFusion(const FusionConfig& config);
    ~SyntheticSignalFusion() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const FusionConfig& config) { _config = config; }
    const FusionConfig& getConfig() const { return _config; }
    
    void setContract(const std::string& code) { _code = code; }
    const std::string& getContract() const { return _code; }
    
    //==========================================================================
    // Signal Input
    //==========================================================================
    
    /// Add tick inference signal
    void addTickInference(const InferredTransaction& tick_inf);
    
    /// Add order book signal
    void addBookSignal(const DepthImbalanceSignal& book_sig);
    
    /// Add book analysis result (alternative interface)
    void addBookAnalysis(const BookAnalysisResult& analysis);
    
    /// Add self-trade calibration
    void addSelfTradeCalibration(const CalibrationResult& calib);
    
    //==========================================================================
    // Fusion
    //==========================================================================
    
    /// Fuse all signals into synthetic transaction
    SyntheticTransactionData fuse();
    
    /// Get fused trade imbalance (for MicroAlphaEngine)
    FusedTradeImbalance getFusedImbalance() const;
    
    //==========================================================================
    // Market State (for adaptive weights)
    //==========================================================================
    
    /// Update volatility estimate
    void updateVolatility(double volatility);
    
    /// Update liquidity estimate
    void updateLiquidity(double liquidity);
    
    /// Update market state from tick (updates volatility estimate)
    void onTick(const std::string& code, double mid_price, uint64_t timestamp);
    
    //==========================================================================
    // Weights
    //==========================================================================
    
    /// Get current adaptive weights
    AdaptiveWeights getCurrentWeights() const;
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void reset();
    
private:
    std::string _code;
    FusionConfig _config;
    
    // Latest signals
    InferredTransaction _latest_tick_inf;
    BookAnalysisResult _latest_book_sig;  // Using BookAnalysisResult (DepthImbalanceSignal is alias)
    CalibrationResult _latest_calibration;
    
    // Market state
    double _volatility;
    double _liquidity;
    
    // Signal history for imbalance calculation
    struct FusedSample {
        double signed_volume;
        double confidence;
        uint64_t timestamp;
    };
    std::deque<FusedSample> _fused_history;
    
    // Accumulated stats
    double _accumulated_buy_vol;
    double _accumulated_sell_vol;
    double _large_volume;
    double _total_volume;
    
    // Flags
    bool _has_tick_inf;
    bool _has_book_sig;
    bool _has_calibration;
    
    // Accuracy tracking for adaptive weights
    struct SourceAccuracy {
        std::vector<bool> predictions;
        double accuracy{0.5};
        uint32_t window{100};
        
        void addPrediction(bool predicted_up, bool actual_up) {
            bool correct = (predicted_up == actual_up);
            predictions.push_back(correct);
            if (predictions.size() > window) {
                predictions.erase(predictions.begin());
            }
            uint32_t correct_count = 0;
            for (bool p : predictions) {
                if (p) correct_count++;
            }
            if (!predictions.empty()) {
                accuracy = static_cast<double>(correct_count) / predictions.size();
            }
        }
    };
    
    SourceAccuracy _tick_accuracy;
    SourceAccuracy _book_accuracy;
    SourceAccuracy _self_trade_accuracy;
    
    double _last_tick_direction{0};
    double _last_book_direction{0};
    double _last_self_trade_direction{0};
    double _last_recorded_price{0};
    
    //==========================================================================
    // Internal Methods
    //==========================================================================
    
    /// Calculate adaptive weights based on market conditions
    AdaptiveWeights calculateAdaptiveWeights() const;
    
    /// Estimate volatility from recent data
    double estimateVolatility() const;
    
    /// Prune old history
    void pruneHistory(uint64_t current_time);
};

} // namespace futu
