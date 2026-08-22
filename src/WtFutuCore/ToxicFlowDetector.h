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
#include "PredictiveToxicity.h"
#include "RealizedToxicity.h"

namespace futu
{

/// Legacy toxicity parameters (for backward compatibility)
struct ToxicityParams
{
    double adverse_threshold;
    double alpha_weight;
    double book_weight;
    double self_trade_weight;
    double extreme_signal_weight;     ///< Extreme signal discount weight (default 0.8)
    double extreme_signal_threshold;  ///< V8-R2: extreme 判定门槛 (单通道分数>此值才兜底, 默认 0.9)
    double vpin_weight;               ///< V8-T6: combined 中 VPIN 通道权重 (alpha 通道 = 1-w, 默认 0.5)
    double vpin_threshold;
    uint32_t vpin_window;
    double vpin_bucket_size;
    uint32_t vpin_min_warmup_buckets; ///< Warmup gate: skip VPIN scoring until N full buckets

    ToxicityParams()
        : adverse_threshold(0.10), alpha_weight(0.3), book_weight(0.3), self_trade_weight(0.4),
          extreme_signal_weight(0.8), extreme_signal_threshold(0.9), vpin_weight(0.5),
          vpin_threshold(0.10) // H2: 统一与 fromVariant 默认一致 (此前构造 0.7 vs fromVariant 0.10, 差 7 倍)
          ,
          vpin_window(50), vpin_bucket_size(1000), vpin_min_warmup_buckets(5)
    {}

    static ToxicityParams fromVariant(wtp::WTSVariant* v)
    {
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
        p.extreme_signal_threshold = FutuConfig::readDouble(v, "extremeSignalThreshold", 0.9);
        p.vpin_weight = FutuConfig::readDouble(v, "vpinWeight", 0.5);

        // V8-T6: 加载期边界校验 (此前越界值静默生效)
        if (!(p.adverse_threshold > 0 && p.adverse_threshold <= 1)) {
            WTSLogger::warn("ToxicityParams: adverseThreshold={} invalid, fallback 0.10", p.adverse_threshold);
            p.adverse_threshold = 0.10;
        }
        if (!(p.vpin_threshold > 0 && p.vpin_threshold <= 1)) {
            WTSLogger::warn("ToxicityParams: vpinThreshold={} invalid, fallback 0.10", p.vpin_threshold);
            p.vpin_threshold = 0.10;
        }
        if (!(p.alpha_weight >= 0 && p.book_weight >= 0 && p.alpha_weight + p.book_weight > 0)) {
            WTSLogger::warn("ToxicityParams: alphaWeight+bookWeight invalid, fallback 0.3/0.3");
            p.alpha_weight = 0.3;
            p.book_weight = 0.3;
        }
        if (!(p.self_trade_weight >= 0 && p.self_trade_weight <= 1)) {
            WTSLogger::warn("ToxicityParams: selfTradeWeight={} invalid, fallback 0.4", p.self_trade_weight);
            p.self_trade_weight = 0.4;
        }
        if (!(p.vpin_weight >= 0 && p.vpin_weight <= 1)) {
            WTSLogger::warn("ToxicityParams: vpinWeight={} invalid, fallback 0.5", p.vpin_weight);
            p.vpin_weight = 0.5;
        }
        if (!(p.extreme_signal_threshold >= 0.5 && p.extreme_signal_threshold <= 1)) {
            WTSLogger::warn("ToxicityParams: extremeSignalThreshold={} invalid, fallback 0.9",
                            p.extreme_signal_threshold);
            p.extreme_signal_threshold = 0.9;
        }
        return p;
    }
};

/// Toxicity analysis result
struct ToxicityMetrics
{
    double predictive_toxicity;
    double realized_adverse_ratio;
    double toxic_score;
    bool is_toxic;
    int toxic_side;
    double avg_adverse_move;
    uint32_t total_fills;
    uint32_t adverse_fills;

    ToxicityMetrics()
        : predictive_toxicity(0), realized_adverse_ratio(0), toxic_score(0), is_toxic(false), toxic_side(0),
          avg_adverse_move(0), total_fills(0), adverse_fills(0)
    {}
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

    void onSelfTradeCalibration(const CalibrationResult& calibration);

    // V8-R3: SyntheticSignalFusion 已整体删除 (数据入口 feed* 零外部调用,
    // hasAnySource() 恒 false, runFusionCycle 每 tick 空转; 其三源硬编码的
    // 融合模式扩展性逊于 SignalAggregator 的 slot+lambda 开闭设计,
    // 可取思想已记录于 AGENTS.md R4 方案)

    // V8-R3: detectEnhancedToxicity/onBookAnalysis 死接口已删 (零外部调用;
    // book 通道数据从未接入 realized)

    //==========================================================================
    // VPIN - Delegated to PredictiveToxicity
    //==========================================================================

    void setBucketSize(double bucket_size);

    void onTrade(double price, double qty, bool isBuy, uint64_t timestamp);

    void onTickVolume(const char* stdCode, const wtp::WTSTickData* tick);

    //==========================================================================
    // Analysis
    //==========================================================================

    /// Get combined toxicity metrics (cached until cache_dirty)
    ToxicityMetrics analyze() const;

    /// Quick toxicity score
    double getToxicityScore() const;

    /// Average adverse move from realized toxicity (ticks)
    double getAvgAdverseMove() const;

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

    // Cached analysis
    mutable ToxicityMetrics _cached_metrics;
    mutable bool _cache_dirty = true;

    void updateCache() const;
};

} // namespace futu