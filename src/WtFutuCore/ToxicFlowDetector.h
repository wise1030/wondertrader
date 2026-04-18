/*!
 * \file ToxicFlowDetector.h
 * \brief Toxic Order Flow Detection (Facade)
 * 
 * Unified interface combining:
 *   - PredictiveToxicity: VPIN, OFI, Alpha signals (every tick)
 *   - RealizedToxicity: Self-trade calibration (on trade events)
 * 
 * When toxicity is high, strategies should:
 *   - Widen spreads
 *   - Reduce quote sizes
 *   - Possibly pause quoting temporarily
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include <string>
#include <cstdint>
#include "../Includes/WTSMarcos.h"
#include "MicroAlphaEngine.h"
#include "SelfTradeCalibrator.h"
#include "MarketDataContext.h"
#include "SyntheticSignalFusion.h"
#include "PredictiveToxicity.h"
#include "RealizedToxicity.h"

namespace futu {

// Forward declarations
struct SyntheticTransactionData;

/// Legacy toxicity parameters (for backward compatibility)
struct ToxicityParams
{
    double      adverse_threshold;
    double      informed_prob_threshold;
    double      alpha_weight;
    double      book_weight;
    double      self_trade_weight;
    double      vpin_threshold;
    uint32_t    vpin_window;
    double      vpin_bucket_size;
    
    ToxicityParams()
        : adverse_threshold(0.6)
        , informed_prob_threshold(0.7)
        , alpha_weight(0.3)
        , book_weight(0.3)
        , self_trade_weight(0.4)
        , vpin_threshold(0.85)
        , vpin_window(50)
        , vpin_bucket_size(1000) {}
};

/// Toxicity analysis result
struct ToxicityMetrics
{
    double      predictive_toxicity;
    double      realized_adverse_ratio;
    double      toxic_score;
    bool        is_toxic;
    int         toxic_side;
    double      avg_adverse_move;
    uint32_t    total_fills;
    uint32_t    adverse_fills;
    
    ToxicityMetrics()
        : predictive_toxicity(0), realized_adverse_ratio(0), toxic_score(0)
        , is_toxic(false), toxic_side(0), avg_adverse_move(0)
        , total_fills(0), adverse_fills(0) {}
};

/// Toxic Flow Detector (Facade combining Predictive and Realized)
class ToxicFlowDetector
{
public:
    ToxicFlowDetector();
    ~ToxicFlowDetector() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setParams(const ToxicityParams& params);
    const ToxicityParams& getParams() const { return _params; }
    
    /// Set external SelfTradeCalibrator (for realized toxicity)
    void setSelfTradeCalibrator(SelfTradeCalibrator* calibrator);
    SelfTradeCalibrator* getSelfTradeCalibrator() const { return _calibrator; }
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// Update with alpha and trade imbalance signals
    void updateMarketAlpha(const AlphaResult& alpha, const TradeImbalanceResult& tradeImb);
    
    //==========================================================================
    // Enhanced Detection (for markets without L2 transaction data)
    //==========================================================================
    
    void onSyntheticAlpha(const SyntheticTransactionData& synth_trans, const AlphaResult& alpha);
    void onBookAnalysis(const BookAnalysisResult& book_analysis);
    void onSelfTradeCalibration(const CalibrationResult& calibration);
    
    /// Enhanced toxicity detection combining all sources
    ToxicityMetrics detectEnhancedToxicity(
        const BookAnalysisResult& book_sig,
        const CalibrationResult& self_calib,
        const AlphaResult& alpha
    );
    
    //==========================================================================
    // Analysis
    //==========================================================================
    
    /// Compute toxicity metrics (combines predictive + realized)
    ToxicityMetrics analyze() const;
    
    /// Get quick toxicity score (0-1)
    double getToxicityScore() const;
    
    /// Check if current flow is toxic
    inline bool isToxicFlow() const
    {
        updateCache();
        return _cached_metrics.is_toxic;
    }
    
    /// Get toxic side (1=Buy, -1=Sell, 0=None)
    inline int getToxicSide() const
    {
        updateCache();
        return _cached_metrics.toxic_side;
    }
    
    //==========================================================================
    // Individual Metrics
    //==========================================================================
    
    double getAvgAdverseMove() const;
    
    //==========================================================================
    // VPIN (Volume Bucket) Analysis - Delegated to PredictiveToxicity
    //==========================================================================
    
    void setBucketSize(double bucket_size);
    void onTrade(double price, double qty, bool isBuy, uint64_t timestamp);
    void onTickVolume(const char* stdCode, const wtp::WTSTickData* tick);
    double getVPIN() const { return _predictive.getVPIN(); }
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void reset();
    
    //==========================================================================
    // Component Access (for advanced use)
    //==========================================================================
    
    PredictiveToxicity& getPredictive() { return _predictive; }
    const PredictiveToxicity& getPredictive() const { return _predictive; }
    
    RealizedToxicity& getRealized() { return _realized; }
    const RealizedToxicity& getRealized() const { return _realized; }

private:
    ToxicityParams _params;
    SelfTradeCalibrator* _calibrator = nullptr;
    
    // Sub-components
    PredictiveToxicity _predictive;
    RealizedToxicity _realized;
    
    // Legacy data (for backward compatibility)
    BookAnalysisResult _latest_book_analysis;
    bool _has_book_data = false;
    
    // Cached analysis
    mutable ToxicityMetrics _cached_metrics;
    mutable bool _cache_dirty = true;
    
    void updateCache() const;
};

} // namespace futu