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
#include "FutuConfig.h"
#include "AlphaTypes.h"
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
    double      alpha_weight;
    double      book_weight;
    double      self_trade_weight;
    double      extreme_signal_weight;  ///< Extreme signal discount weight (default 0.8)
    double      vpin_threshold;
    uint32_t    vpin_window;
    double      vpin_bucket_size;
    uint32_t    vpin_min_warmup_buckets;  ///< Warmup gate: skip VPIN scoring until N full buckets
    
    ToxicityParams()
        : adverse_threshold(0.10)
        , alpha_weight(0.3)
        , book_weight(0.3)
        , self_trade_weight(0.4)
        , extreme_signal_weight(0.8)
        , vpin_threshold(0.7)
        , vpin_window(50)
        , vpin_bucket_size(1000)
        , vpin_min_warmup_buckets(5) {}
    
    static ToxicityParams fromVariant(wtp::WTSVariant* v) {
        ToxicityParams p;
        p.adverse_threshold = FutuConfig::readDouble(v, "adverseThreshold", 0.10);
        p.vpin_threshold = FutuConfig::readDouble(v, "vpinThreshold", 0.10);
        p.vpin_window = FutuConfig::readUInt32(v, "window", 50);
        p.vpin_bucket_size = FutuConfig::readDouble(v, "bucketSize", 1000);
        p.vpin_min_warmup_buckets = FutuConfig::readUInt32(v, "minWarmupBuckets", 5);
        p.alpha_weight = FutuConfig::readDouble(v, "alphaWeight", 0.3);
        p.book_weight = FutuConfig::readDouble(v, "bookWeight", 0.3);
        p.self_trade_weight = FutuConfig::readDouble(v, "selfTradeWeight", 0.4);
        p.extreme_signal_weight = FutuConfig::readDouble(v, "extremeSignalWeight", 0.8);
        return p;
    }
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
    
    //==========================================================================
    // Synthetic Signal Fusion (integrated)
    //==========================================================================
    
    /// Feed tick inference data into the internal fusion engine
    void feedTickInference(const InferredTransaction& tick_inf);
    
    /// Feed book signal into the internal fusion engine
    void feedBookSignal(const DepthImbalanceSignal& book_sig);
    
    /// Enable/disable fusion engine
    void setFusionEnabled(bool enabled) { _fusion_enabled = enabled; }
    bool isFusionEnabled() const { return _fusion_enabled; }
    
    /// Access the internal fusion engine for advanced configuration
    SyntheticSignalFusion& getFusion() { return _signal_fusion; }
    const SyntheticSignalFusion& getFusion() const { return _signal_fusion; }
    
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
    // Fusion Cycle (called by UftFutuMmStrategy per tick)
    //==========================================================================
    
    /// Run fusion and feed results back into toxicity detection
    void runFusionCycle();
    
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
    
    // Synthetic signal fusion (integrated from standalone module)
    SyntheticSignalFusion _signal_fusion;
    bool _fusion_enabled = true;
    
    // Legacy data (for backward compatibility)
    BookAnalysisResult _latest_book_analysis;
    bool _has_book_data = false;
    
    // Cached analysis
    mutable ToxicityMetrics _cached_metrics;
    mutable bool _cache_dirty = true;
    
    void updateCache() const;
};

} // namespace futu