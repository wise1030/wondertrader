/*!
 * \file ToxicFlowDetector.cpp
 * \brief Toxic Order Flow Detection Implementation (Facade)
 * 
 * Combines PredictiveToxicity and RealizedToxicity for unified interface.
 */
#include "ToxicFlowDetector.h"
#include "SyntheticSignalFusion.h"
#include "SelfTradeCalibrator.h"
#include "MarketDataContext.h"
#include "../Includes/WTSDataDef.hpp"
#include <algorithm>
#include <cmath>

namespace futu {

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------

ToxicFlowDetector::ToxicFlowDetector()
    : _calibrator(nullptr)
    , _has_book_data(false)
    , _cache_dirty(true)
{
}

//------------------------------------------------------------------------------
// Configuration
//------------------------------------------------------------------------------

void ToxicFlowDetector::setParams(const ToxicityParams& params)
{
    _params = params;
    
    // Configure sub-components
    PredictiveToxicityConfig pred_cfg;
    pred_cfg.vpin_threshold = params.vpin_threshold;
    pred_cfg.vpin_window = params.vpin_window;
    pred_cfg.vpin_bucket_size = params.vpin_bucket_size;
    pred_cfg.alpha_threshold = params.adverse_threshold;
    pred_cfg.ofi_weight = params.alpha_weight;
    pred_cfg.trade_weight = params.book_weight;
    _predictive.setConfig(pred_cfg);
    
    RealizedToxicityConfig real_cfg;
    real_cfg.weight = params.self_trade_weight;
    real_cfg.min_samples = 3;
    _realized.setConfig(real_cfg);
}

void ToxicFlowDetector::setSelfTradeCalibrator(SelfTradeCalibrator* calibrator)
{
    _calibrator = calibrator;
    _realized.setSelfTradeCalibrator(calibrator);
}

//------------------------------------------------------------------------------
// Reset
//------------------------------------------------------------------------------

void ToxicFlowDetector::reset()
{
    _has_book_data = false;
    _cache_dirty = true;
    _cached_metrics = ToxicityMetrics();
    
    _predictive.reset();
    _realized.reset();
}

//------------------------------------------------------------------------------
// Data Input
//------------------------------------------------------------------------------

void ToxicFlowDetector::updateMarketAlpha(const AlphaResult& alpha, const TradeImbalanceResult& tradeImb)
{
    _predictive.updateAlpha(alpha, tradeImb);
    _cache_dirty = true;
}

void ToxicFlowDetector::onSyntheticAlpha(const SyntheticTransactionData& synth_trans, const AlphaResult& alpha)
{
    // Update predictive with synthetic data
    _predictive.updateAlpha(alpha, TradeImbalanceResult{});
    _cache_dirty = true;
}

void ToxicFlowDetector::onBookAnalysis(const BookAnalysisResult& book_analysis)
{
    _latest_book_analysis = book_analysis;
    _has_book_data = true;
    _realized.onBookAnalysis(book_analysis.imbalance_score);
    _cache_dirty = true;
}

void ToxicFlowDetector::onSelfTradeCalibration(const CalibrationResult& calibration)
{
    _realized.onCalibration(calibration);
    _cache_dirty = true;
}

//------------------------------------------------------------------------------
// VPIN - Delegated to PredictiveToxicity
//------------------------------------------------------------------------------

void ToxicFlowDetector::setBucketSize(double bucket_size)
{
    _predictive.setBucketSize(bucket_size);
}

void ToxicFlowDetector::onTrade(double price, double qty, bool isBuy, uint64_t timestamp)
{
    _predictive.onTrade(price, qty, isBuy, timestamp);
}

void ToxicFlowDetector::onTickVolume(const char* stdCode, const wtp::WTSTickData* tick)
{
    _predictive.onTickVolume(stdCode, tick);
}

//------------------------------------------------------------------------------
// Cache Update
//------------------------------------------------------------------------------

void ToxicFlowDetector::updateCache() const
{
    if (!_cache_dirty) return;
    
    _cached_metrics = ToxicityMetrics();
    
    auto pred_result = _predictive.analyze();
    
    _cached_metrics.predictive_toxicity = pred_result.combined_score;
    _cached_metrics.toxic_score = pred_result.combined_score;
    
    // FIX P2-9: extreme_signal作为独立保护层，在最终加权后叠加
    // 原代码在realized加权之前就把extreme_signal混入toxic_score，然后realized_weight
    // 会稀释extreme_signal的保护效果。例如:
    //   extreme_signal=0.9, combined_score=0.3, self_trade_weight=0.4
    //   原代码: toxic_score = max(0.3, 0.9*0.8)=0.72, 然后 0.6*0.72+0.4*realized = 被稀释
    //   修复后: 先算weighted_score = 0.6*0.3+0.4*realized, 再取max(weighted, 0.9*0.8)
    // extreme_signal是硬性保护信号，不应被realized稀释。
    
    // Integrate realized toxicity into combined score
    // self_trade_weight controls how much realized adverse ratio contributes
    auto real_result = _realized.analyze();
    _cached_metrics.realized_adverse_ratio = real_result.adverse_ratio;
    if (real_result.total_fills >= 3)  // Need minimum sample size
    {
        double realized_weight = _params.self_trade_weight;
        _cached_metrics.toxic_score = (1.0 - realized_weight) * _cached_metrics.toxic_score
                                    + realized_weight * real_result.decayed_score;
    }
    
    // Apply extreme_signal as independent protection layer AFTER realized weighting
    if (pred_result.extreme_signal > 0)
    {
        _cached_metrics.toxic_score = std::max(_cached_metrics.toxic_score, 
                                               pred_result.extreme_signal * _params.extreme_signal_weight);
    }
    
    _cached_metrics.is_toxic = _cached_metrics.toxic_score > _params.adverse_threshold;
    
    if (_cached_metrics.is_toxic)
    {
        _cached_metrics.toxic_side = pred_result.toxic_side;
    }
    
    _cache_dirty = false;
}

//------------------------------------------------------------------------------
// Analysis
//------------------------------------------------------------------------------

ToxicityMetrics ToxicFlowDetector::analyze() const
{
    updateCache();
    return _cached_metrics;
}

double ToxicFlowDetector::getToxicityScore() const
{
    updateCache();
    return _cached_metrics.toxic_score;
}

double ToxicFlowDetector::getAvgAdverseMove() const
{
    auto real_result = _realized.analyze();
    return real_result.avg_adverse_move;
}

//------------------------------------------------------------------------------
// Enhanced Toxicity Detection
//------------------------------------------------------------------------------

ToxicityMetrics ToxicFlowDetector::detectEnhancedToxicity(
    const BookAnalysisResult& book_sig,
    const CalibrationResult& self_calib,
    const AlphaResult& alpha)
{
    // Update components
    _predictive.updateAlpha(alpha, TradeImbalanceResult{});
    _realized.onCalibration(self_calib);
    _realized.onBookAnalysis(book_sig.imbalance_score);
    
    // Return combined analysis
    return analyze();
}

//==============================================================================
// Synthetic Signal Fusion Integration
//==============================================================================

void ToxicFlowDetector::feedTickInference(const InferredTransaction& tick_inf)
{
    if (_fusion_enabled)
    {
        _signal_fusion.addTickInference(tick_inf);
        _cache_dirty = true;
    }
}

void ToxicFlowDetector::feedBookSignal(const DepthImbalanceSignal& book_sig)
{
    if (_fusion_enabled)
    {
        _signal_fusion.addBookSignal(book_sig);
        _cache_dirty = true;
    }
}

void ToxicFlowDetector::runFusionCycle()
{
    if (!_fusion_enabled) return;
    
    // Fuse all available signals into synthetic transaction data
    SyntheticTransactionData synth = _signal_fusion.fuse();
    
    // Feed fused result back into toxicity detection
    AlphaResult alpha;
    alpha.alpha = synth.direction_signal;
    alpha.confidence = synth.confidence;
    alpha.timestamp = synth.timestamp;
    
    onSyntheticAlpha(synth, alpha);
}

} // namespace futu
