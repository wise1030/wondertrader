/*!
 * \file ToxicFlowDetector.cpp
 * \brief Toxic Order Flow Detection Implementation
 * 
 * Note: Self-trade fill tracking is delegated to SelfTradeCalibrator.
 *       This detector focuses on combining predictive signals with realized toxicity.
 */
#include "ToxicFlowDetector.h"
#include "SyntheticSignalFusion.h"
#include "SelfTradeCalibrator.h"
#include "OrderBookAnalyzer.h"
#include <algorithm>
#include <cmath>

namespace futu {

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------

ToxicFlowDetector::ToxicFlowDetector()
    : _has_alpha_data(false)
    , _has_book_data(false)
    , _has_calibration_data(false)
    , _cache_dirty(true)
{
}

//------------------------------------------------------------------------------
// Reset
//------------------------------------------------------------------------------

void ToxicFlowDetector::reset()
{
    _has_alpha_data = false;
    _has_book_data = false;
    _has_calibration_data = false;
    _cache_dirty = true;
    _cached_metrics = ToxicityMetrics();
}

//------------------------------------------------------------------------------
// Data Input
//------------------------------------------------------------------------------

void ToxicFlowDetector::updateMarketAlpha(const AlphaResult& alpha, const TradeImbalanceResult& tradeImb)
{
    _latest_alpha = alpha;
    _latest_trade_imb = tradeImb;
    _has_alpha_data = true;
    _cache_dirty = true;
}

//------------------------------------------------------------------------------
// Enhanced Detection Methods
//------------------------------------------------------------------------------

void ToxicFlowDetector::onSyntheticAlpha(const SyntheticTransactionData& synth_trans, const AlphaResult& alpha)
{
    // Update alpha data
    _latest_alpha = alpha;
    _has_alpha_data = true;
    
    // Create a synthetic trade imbalance from the transaction
    _latest_trade_imb.net_flow = synth_trans.is_buy_initiated ? 
        synth_trans.volume : -synth_trans.volume;
    _latest_trade_imb.imbalance_ratio = synth_trans.direction_signal;
    // Note: TradeImbalanceResult doesn't have confidence field
    // Use large_trade_ratio as a proxy for signal quality
    _latest_trade_imb.large_trade_ratio = synth_trans.confidence;
    
    _cache_dirty = true;
}

void ToxicFlowDetector::onBookAnalysis(const BookAnalysisResult& book_analysis)
{
    _latest_book_analysis = book_analysis;
    _has_book_data = true;
    _cache_dirty = true;
}

void ToxicFlowDetector::onSelfTradeCalibration(const CalibrationResult& calibration)
{
    _latest_calibration = calibration;
    _has_calibration_data = true;
    _cache_dirty = true;
}

//------------------------------------------------------------------------------
// Cache Update
//------------------------------------------------------------------------------

void ToxicFlowDetector::updateCache() const
{
    if (!_cache_dirty) return;
    
    _cached_metrics = ToxicityMetrics();
    
    //==========================================================================
    // 1. Predictive Toxicity (from Alpha signals)
    //==========================================================================
    
    double t_ofi = 0.0;
    double t_trade = 0.0;
    double predictive_toxicity = 0.0;
    double alpha_extreme = 0.0;
    
    if (_has_alpha_data)
    {
        // OFI Toxicity
        t_ofi = std::abs(_latest_alpha.ofi_component);
        
        // Trade Imbalance Toxicity
        t_trade = std::abs(_latest_trade_imb.imbalance_ratio) * 
                  (0.5 + 0.5 * _latest_trade_imb.large_trade_ratio);
                  
        predictive_toxicity = (t_ofi + t_trade) / 2.0;
        
        // 只有当预测性指标超过极端阈值 (例如 0.90) 时，才具备"一票否决权"
        if (t_ofi > 0.90 || t_trade > 0.90)
        {
            alpha_extreme = std::max(t_ofi, t_trade);
        }
    }
    
    _cached_metrics.predictive_toxicity = predictive_toxicity;
    
    //==========================================================================
    // 2. Realized Toxicity (from SelfTradeCalibrator)
    //==========================================================================
    
    double realized_adverse_ratio = 0.0;
    
    // 优先使用传入的 calibration 数据
    if (_has_calibration_data)
    {
        realized_adverse_ratio = _latest_calibration.toxicity_level;
        _cached_metrics.realized_adverse_ratio = realized_adverse_ratio;
        _cached_metrics.total_fills = static_cast<uint32_t>(_latest_calibration.sample_size);
        _cached_metrics.adverse_fills = static_cast<uint32_t>(
            _latest_calibration.sample_size * _latest_calibration.toxicity_level
        );
    }
    
    //==========================================================================
    // 3. Combined Toxicity Score
    //==========================================================================
    
    // 自身真实成交亏损作为最重要的 Ground Truth (第一防线)
    // 极端 Alpha 信号作为并行的并集触发器 (第二防线)
    // 采用 max 代替线性平权，防止 Alpha 轻微抖动造成的过度防御
    _cached_metrics.toxic_score = std::max(realized_adverse_ratio, alpha_extreme);
        
    _cached_metrics.is_toxic = _cached_metrics.toxic_score > _params.adverse_threshold;
    
    //==========================================================================
    // 4. Determine Toxic Side
    //==========================================================================
    
    if (_cached_metrics.is_toxic && _has_alpha_data)
    {
        // 毒性方向与大单和OFI方向一致
        if (_latest_alpha.ofi_component > 0 && _latest_trade_imb.imbalance_ratio > 0)
            _cached_metrics.toxic_side = 1;  // 买盘毒性（正在暴涨，不要挂空单）
        else if (_latest_alpha.ofi_component < 0 && _latest_trade_imb.imbalance_ratio < 0)
            _cached_metrics.toxic_side = -1; // 卖盘毒性（正在暴跌，不要挂多单）
        else
            _cached_metrics.toxic_side = 0;  // 冲突情况，无方向
    }
    else
    {
        _cached_metrics.toxic_side = 0;
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
    // 如果有 calibration 数据，返回平均逆向跳动
    if (_has_calibration_data)
    {
        // 从 calibration 的毒性水平和样本数估算
        return _latest_calibration.toxicity_level * 2.0;  // 简化估算
    }
    return _cached_metrics.avg_adverse_move;
}

//------------------------------------------------------------------------------
// Enhanced Toxicity Detection
//------------------------------------------------------------------------------

ToxicityMetrics ToxicFlowDetector::detectEnhancedToxicity(
    const BookAnalysisResult& book_sig,
    const CalibrationResult& self_calib,
    const AlphaResult& alpha) const
{
    ToxicityMetrics result;
    
    // 1. Alpha-based toxicity (predictive)
    double alpha_toxicity = 0.0;
    double t_ofi = std::abs(alpha.ofi_component);
    double t_trade = std::abs(alpha.trade_component);
    
    // Non-linear combination: use maximum of OFI and trade
    alpha_toxicity = std::max(t_ofi, t_trade);
    
    // 2. Order book toxicity
    double book_toxicity = 0.0;
    book_toxicity = std::abs(book_sig.imbalance_score) * book_sig.toxicity_score;
    
    // 3. Self-trade toxicity (ground truth, realized)
    double self_toxicity = self_calib.toxicity_level;
    
    // 4. Weighted combination
    result.predictive_toxicity = alpha_toxicity * _params.alpha_weight + 
                                  book_toxicity * _params.book_weight;
    result.realized_adverse_ratio = self_toxicity;
    
    // Final toxicity score: max of realized and predictive
    result.toxic_score = std::max(
        self_toxicity * _params.self_trade_weight,
        result.predictive_toxicity
    );
    
    // If we have calibration data with sufficient samples, boost the realized component
    if (self_calib.sample_size >= 5) {
        double calib_confidence = self_calib.confidence;
        result.toxic_score = std::max(
            result.toxic_score,
            self_toxicity * _params.self_trade_weight * calib_confidence
        );
    }
    
    // Determine if toxic
    result.is_toxic = result.toxic_score > _params.adverse_threshold;
    
    // Determine toxic side
    if (result.is_toxic) {
        // Combine direction signals
        double direction = 0.0;
        
        // Alpha direction
        direction += _params.alpha_weight * alpha.alpha;
        
        // Book direction
        direction += _params.book_weight * book_sig.imbalance_score;
        
        // Self-trade bias (inverted: avoid the toxic side)
        direction -= _params.self_trade_weight * self_calib.direction_bias;
        
        // Positive = buy toxicity (market going up, avoid sell quotes)
        // Negative = sell toxicity (market going down, avoid buy quotes)
        if (direction > 0.2) {
            result.toxic_side = 1;
        } else if (direction < -0.2) {
            result.toxic_side = -1;
        } else {
            result.toxic_side = 0;
        }
    }
    
    // Copy statistics from self-calibration
    result.total_fills = static_cast<uint32_t>(self_calib.sample_size);
    result.adverse_fills = static_cast<uint32_t>(
        self_calib.sample_size * self_calib.toxicity_level
    );
    
    return result;
}

} // namespace futu