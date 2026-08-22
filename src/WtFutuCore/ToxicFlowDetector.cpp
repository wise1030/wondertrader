/*!
 * \file ToxicFlowDetector.cpp
 * \brief Toxic Order Flow Detection Implementation (Facade)
 *
 * Combines PredictiveToxicity and RealizedToxicity for unified interface.
 */
#include "ToxicFlowDetector.h"
#include "SelfTradeCalibrator.h"
#include "MarketDataContext.h"
#include "../Includes/WTSDataDef.hpp"
#include <algorithm>
#include <cmath>

namespace futu
{

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------

ToxicFlowDetector::ToxicFlowDetector() : _calibrator(nullptr), _cache_dirty(true) {}

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
    // V8-T6: alpha 通道内 ofi/trade 权重归一化 (和=1) --
    // 原样透传 alpha_weight/book_weight (0.5/0.3, 和=0.8) 使 alpha_toxicity
    // 尺度被压缩且随配置漂移; 归一后 alpha_toxicity 严格 [0,1]
    double w_sum = params.alpha_weight + params.book_weight;
    if (w_sum > 0) {
        pred_cfg.ofi_weight = params.alpha_weight / w_sum;
        pred_cfg.trade_weight = params.book_weight / w_sum;
    } else {
        WTSLogger::warn("ToxicFlowDetector: alpha_weight+book_weight <= 0, fallback to 0.5/0.5");
        pred_cfg.ofi_weight = 0.5;
        pred_cfg.trade_weight = 0.5;
    }
    pred_cfg.vpin_weight = params.vpin_weight;
    pred_cfg.extreme_threshold = params.extreme_signal_threshold;
    pred_cfg.min_warmup_buckets = params.vpin_min_warmup_buckets;
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
    if (!_cache_dirty)
        return;

    _cached_metrics = ToxicityMetrics();

    auto pred_result = _predictive.analyze();

    _cached_metrics.predictive_toxicity = pred_result.combined_score;
    _cached_metrics.toxic_score = pred_result.combined_score;

    // extreme_signal作为独立保护层，在最终加权后叠加
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
    if (real_result.total_fills >= 3) // Need minimum sample size
    {
        double realized_weight = _params.self_trade_weight;
        _cached_metrics.toxic_score =
            (1.0 - realized_weight) * _cached_metrics.toxic_score + realized_weight * real_result.decayed_score;
    }

    // Apply extreme_signal as independent protection layer AFTER realized weighting
    if (pred_result.extreme_signal > 0) {
        _cached_metrics.toxic_score =
            std::max(_cached_metrics.toxic_score, pred_result.extreme_signal * _params.extreme_signal_weight);
    }

    _cached_metrics.is_toxic = _cached_metrics.toxic_score > _params.adverse_threshold
                               // V8-T4: 恢复 VPIN 独立触发条件 -- 此前 pred 的
                               // OR 条件被门面丢弃, vpin 单独触发需 combined 通道
                               // >2×vpin_threshold (0.5 加权稀释), 配置语义差 2 倍
                               || (pred_result.vpin_ready && pred_result.vpin > _params.vpin_threshold);

    if (_cached_metrics.is_toxic) {
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

} // namespace futu
