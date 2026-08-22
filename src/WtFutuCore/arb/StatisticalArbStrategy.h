/*!
 * \file StatisticalArbStrategy.h
 * \brief Statistical Arbitrage Strategy (Multi-factor)
 *
 * Strategy Logic:
 *   - Multi-factor model for spread prediction
 *   - Feature-based signal generation
 *   - Machine learning signal combination
 *   - Risk-adjusted position sizing
 *
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include "SpreadArbitrageTypes.h"
#include "ISpreadStrategy.h"
#include "SpreadCalculator.h"
#include "../../Share/RingBuffer.hpp"
#include <memory>
#include <array>

namespace futu
{

//==============================================================================
// Statistical Arbitrage Configuration
//==============================================================================

struct StatisticalArbConfig
{
    double entry_threshold;     ///< Composite signal threshold for entry
    double exit_threshold;      ///< Exit threshold
    double stop_loss_threshold; ///< Stop loss threshold

    double max_position; ///< Maximum position size
    double base_qty;     ///< Base position size

    uint32_t feature_window; ///< Window for feature calculation
    uint32_t min_samples;    ///< Minimum samples required

    // Feature weights (V8-A3: weight_mspread 已随死因子删除, 4 因子归一)
    double weight_zscore;      ///< Z-Score feature weight
    double weight_momentum;    ///< Momentum feature weight
    double weight_volatility;  ///< Volatility feature weight
    double weight_correlation; ///< Correlation feature weight

    uint32_t convergence_timeout; ///< Timeout for convergence (seconds)

    StatisticalArbConfig()
        : entry_threshold(0.7), exit_threshold(0.3), stop_loss_threshold(1.5), max_position(15.0), base_qty(1.0),
          feature_window(100), min_samples(50), weight_zscore(0.30), weight_momentum(0.20), weight_volatility(0.15),
          weight_correlation(0.20), convergence_timeout(5400)
    {}
};

//==============================================================================
// Statistical Features
//==============================================================================

struct StatisticalFeatures
{
    double zscore;            ///< Normalized Z-Score
    double zscore_momentum;   ///< Z-Score change rate
    double volatility_ratio;  ///< Volatility ratio (leg1/leg2)
    double correlation_trend; ///< Correlation trend

    double composite_signal;  ///< Weighted composite signal
    double signal_confidence; ///< Signal confidence

    // Feature quality metrics
    double feature_stability; ///< Stability of features
    bool is_valid;            ///< Are features valid

    // V8-A3: mspread_imbalance/volume_imbalance 因子已删除 -- 其输入
    // (SpreadState 微结构字段) 全链路从未填充 (SpreadCalculator 只收腿的最新价,
    // 无合成价差盘口/成交量数据), 恒 0 参与 composite 属死权重; 待 L2 数据
    // 管道就绪后 (R4) 再按真实语义重建。

    StatisticalFeatures()
        : zscore(0), zscore_momentum(0), volatility_ratio(1), correlation_trend(0), composite_signal(0),
          signal_confidence(0), feature_stability(0), is_valid(false)
    {}
};



//==============================================================================
// Statistical Arbitrage Strategy
//==============================================================================

class StatisticalArbStrategy : public ISpreadStrategy
{
public:
    StatisticalArbStrategy();
    ~StatisticalArbStrategy() = default;

    //==========================================================================
    // ISpreadStrategy Interface
    //==========================================================================

    SpreadSignal generateSignal(const SpreadState& state, uint64_t current_time) override;

    void update(const SpreadState& state, uint64_t timestamp) override { updateState(state, timestamp); }

    void configure(const SpreadPairConfig& cfg) override { _config.max_position = cfg.max_spread_position; }

    void reset() override;

    const char* typeName() const override { return "statistical_arb"; }

    //==========================================================================
    // Configuration
    //==========================================================================

    void setConfig(const StatisticalArbConfig& config) { _config = config; }
    const StatisticalArbConfig& getConfig() const { return _config; }

    //==========================================================================
    // Data Update
    //==========================================================================

    /// Update with spread state
    void updateState(const SpreadState& state, uint64_t timestamp);

    //==========================================================================
    // Feature Calculation
    //==========================================================================

    /// Calculate statistical features
    StatisticalFeatures calculateFeatures(const SpreadState& state);

    /// Get current features
    const StatisticalFeatures& getCurrentFeatures() const { return _features; }

    //==========================================================================
    // Performance Tracking
    //==========================================================================

    static constexpr const char* getName() { return "StatisticalArb"; }

private:
    //==========================================================================
    // Internal Methods
    //==========================================================================

    void updateFeatureHistory();
    double calculateZScoreFeature(const SpreadState& state) const;
    double calculateMomentumFeature(const SpreadState& state) const;
    double calculateVolatilityFeature(const SpreadState& state) const;
    double calculateCorrelationFeature(const SpreadState& state) const;

    double calculatePositionSize(const StatisticalFeatures& features) const;
    double calculateConfidence(const StatisticalFeatures& features) const;

    //==========================================================================
    // Configuration
    //==========================================================================

    StatisticalArbConfig _config;

    //==========================================================================
    // Feature History
    //==========================================================================

    RingBuffer<double, 128> _zscore_history;
    RingBuffer<double, 128> _volatility_history;
    RingBuffer<double, 128> _correlation_history;
    RingBuffer<double, 128> _signal_history;

    uint64_t _last_update;

    //==========================================================================
    // Current Features
    //==========================================================================

    StatisticalFeatures _features;
    double _prev_zscore;
    double _prev_correlation;

    //==========================================================================
    // Performance Tracking
    //==========================================================================


    //==========================================================================
    // Adaptive Weights
    //==========================================================================


    //==========================================================================
    // Signal State
    //==========================================================================

    uint64_t _last_signal_time;
    double _entry_signal;
};

} // namespace futu
