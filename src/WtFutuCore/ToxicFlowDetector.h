/*!
 * \file ToxicFlowDetector.h
 * \brief Toxic Order Flow Detection for Market Making
 * 
 * Detects adverse selection risk from informed traders:
 *   - Combines predictive signals (OFI, Trade Imbalance)
 *   - Integrates with SelfTradeCalibrator for realized toxicity
 *   - Provides toxicity score for risk management
 * 
 * When toxicity is high, strategies should:
 *   - Widen spreads
 *   - Reduce quote sizes
 *   - Possibly pause quoting temporarily
 * 
 * Note: Self-trade fill tracking is delegated to SelfTradeCalibrator.
 *       Use setSelfTradeCalibrator() to connect the calibrator.
 */
#pragma once

#include <string>
#include <cstdint>
#include "../Includes/WTSMarcos.h"
#include "MicroAlphaEngine.h"
#include "SelfTradeCalibrator.h"
#include "OrderBookAnalyzer.h"
#include "SyntheticSignalFusion.h"

namespace futu {

// Forward declarations
struct SyntheticTransactionData;

/// Toxicity detection parameters
struct ToxicityParams
{
    double      adverse_threshold;      ///< Threshold for adverse selection ratio
    double      informed_prob_threshold;///< Threshold for informed trader probability
    
    // Toxicity source weights for enhanced detection
    double      alpha_weight;           ///< Weight for alpha-based toxicity
    double      book_weight;            ///< Weight for order book imbalance toxicity
    double      self_trade_weight;      ///< Weight for self-trade calibration toxicity
    
    ToxicityParams()
        : adverse_threshold(0.6)
        , informed_prob_threshold(0.7)
        , alpha_weight(0.3)
        , book_weight(0.3)
        , self_trade_weight(0.4)    // Self-trade as ground truth, highest weight
    {}
};

/// Toxicity analysis result
struct ToxicityMetrics
{
    double      predictive_toxicity;      // 基于 OFI 和 大单流计算的毒性 (事前)
    double      realized_adverse_ratio;   // 基于实际 Fill 的毒性 (事后)
    double      toxic_score;              // 混合最终毒性
    bool        is_toxic;                 // 是否触发熔断/展宽
    int         toxic_side;               // 毒性方向: 1(暴涨毒性:避免挂空), -1(暴跌毒性:避免挂多), 0(安全)

    double      avg_adverse_move;         ///< Average adverse price move (in ticks)
    uint32_t    total_fills;              ///< Total fills analyzed
    uint32_t    adverse_fills;            ///< Fills with adverse selection
    
    ToxicityMetrics()
        : predictive_toxicity(0)
        , realized_adverse_ratio(0)
        , toxic_score(0)
        , is_toxic(false)
        , toxic_side(0)
        , avg_adverse_move(0)
        , total_fills(0)
        , adverse_fills(0)
    {}
};

/// Toxic Flow Detector
class ToxicFlowDetector
{
public:
    ToxicFlowDetector();
    ~ToxicFlowDetector() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setParams(const ToxicityParams& params) { _params = params; }
    const ToxicityParams& getParams() const { return _params; }
    
    /// Set external SelfTradeCalibrator (for realized toxicity)
    void setSelfTradeCalibrator(SelfTradeCalibrator* calibrator) { _self_trade_calibrator = calibrator; }
    SelfTradeCalibrator* getSelfTradeCalibrator() const { return _self_trade_calibrator; }
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// 每当 MicroAlphaEngine 更新时，将最新的指标喂给检测器
    void updateMarketAlpha(const AlphaResult& alpha, const TradeImbalanceResult& tradeImb);
    
    //==========================================================================
    // Enhanced Detection (for markets without L2 transaction data)
    //==========================================================================
    
    /// Update with synthetic transaction data (fused signal)
    void onSyntheticAlpha(const SyntheticTransactionData& synth_trans, const AlphaResult& alpha);
    
    /// Update with order book analysis
    void onBookAnalysis(const BookAnalysisResult& book_analysis);
    
    /// Update with self-trade calibration
    void onSelfTradeCalibration(const CalibrationResult& calibration);
    
    /// Enhanced toxicity detection combining all sources
    ToxicityMetrics detectEnhancedToxicity(
        const BookAnalysisResult& book_sig,
        const CalibrationResult& self_calib,
        const AlphaResult& alpha
    ) const;
    
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
    
    /// Get average adverse move in ticks
    double getAvgAdverseMove() const;
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void reset();
    
private:
    ToxicityParams _params;
    
    // External calibrator for realized toxicity (owned by strategy)
    SelfTradeCalibrator* _self_trade_calibrator = nullptr;
    
    // Market Alpha Data
    AlphaResult _latest_alpha;
    TradeImbalanceResult _latest_trade_imb;
    bool _has_alpha_data = false;
    
    // Enhanced detection data
    BookAnalysisResult _latest_book_analysis;
    CalibrationResult _latest_calibration;
    bool _has_book_data = false;
    bool _has_calibration_data = false;
    
    // Cached analysis
    mutable ToxicityMetrics _cached_metrics;
    mutable bool _cache_dirty;
    
    void updateCache() const;
};

} // namespace futu
