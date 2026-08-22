/*!
 * \file PairsTradingStrategy.h
 * \brief Pairs Trading Strategy (Cointegration-based)
 *
 * Strategy Logic:
 *   - Identify cointegrated pairs
 *   - Trade on deviation from long-run equilibrium
 *   - Beta-weighted position sizing
 *   - Dynamic hedge ratio adjustment
 *
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include "SpreadArbitrageTypes.h"
#include "ISpreadStrategy.h"
#include "SpreadCalculator.h"
#include "../../Share/RingBuffer.hpp"
#include <memory>

namespace futu
{

//==============================================================================
// Pairs Trading Configuration
//==============================================================================

struct PairsTradingConfig
{
    double entry_z_threshold; ///< Z-Score entry threshold
    double exit_z_threshold;  ///< Z-Score exit threshold
    double stop_loss_z;       ///< Stop loss Z-Score

    double max_position; ///< Maximum position size
    double base_qty;     ///< Base position size

    uint32_t min_samples;   ///< Minimum samples for cointegration
    double min_correlation; ///< Minimum correlation for valid pair
    double max_spread_std;  ///< Maximum spread standard deviation

    uint32_t lookback_window;    ///< Rolling window for beta estimation
    uint32_t rebalance_interval; ///< Beta rebalance interval (ticks)

    bool use_dynamic_beta; ///< Use dynamic beta adjustment
    double beta_smoothing; ///< Beta smoothing factor (EMA)

    uint32_t convergence_timeout; ///< Convergence timeout (seconds)

    PairsTradingConfig()
        : entry_z_threshold(2.0), exit_z_threshold(0.5), stop_loss_z(4.0), max_position(15.0), base_qty(1.0),
          min_samples(100), min_correlation(0.7), max_spread_std(100.0), lookback_window(200), rebalance_interval(100),
          use_dynamic_beta(true), beta_smoothing(0.05), convergence_timeout(7200)
    {}
};

//==============================================================================
// Cointegration Test Result
//==============================================================================

struct CointegrationResult
{
    bool is_cointegrated;  ///< Is pair cointegrated
    double p_value;        ///< ADF test p-value
    double test_statistic; ///< ADF test statistic
    double critical_value; ///< Critical value at 5%

    double beta;         ///< Cointegration coefficient
    double alpha;        ///< Intercept term
    double residual_std; ///< Residual standard deviation

    CointegrationResult()
        : is_cointegrated(false), p_value(1.0), test_statistic(0), critical_value(-2.86) // 5% critical value
          ,
          beta(1.0), alpha(0), residual_std(0)
    {}
};

//==============================================================================
// Pairs Trading Strategy
//==============================================================================

class PairsTradingStrategy : public ISpreadStrategy
{
public:
    PairsTradingStrategy();
    ~PairsTradingStrategy() = default;

    //==========================================================================
    // ISpreadStrategy Interface
    //==========================================================================

    SpreadSignal generateSignal(const SpreadState& state, uint64_t current_time) override;

    void update(const SpreadState& state, uint64_t timestamp) override
    {
        // V8-A4: 腿间配对去重 — calculator 已做 fresh-pairing (仅双腿都有新数据
        // 才推进 last_update), 但本策略私有历史此前每次 update 都 push, 任一腿
        // tick 会混入 (新腿价, 陈旧腿价) 样本, OLS beta 被陈旧价污染。
        // 与 calculator 同标准: 无新配对样本时跳过。
        if (state.last_update == 0 || state.last_update == _last_update)
            return;
        updatePrices(state.leg1_price, state.leg2_price, state.last_update);
    }

    /// 私有价格历史样本数 (V8-A4 配对去重的测试/诊断钩子)
    size_t priceSampleCount() const { return _price1_history.size(); }

    void configure(const SpreadPairConfig& cfg) override
    {
        _config.entry_z_threshold = cfg.entry_z_threshold;
        _config.exit_z_threshold = cfg.exit_z_threshold;
        _config.stop_loss_z = cfg.stop_loss_z;
        _config.max_position = cfg.max_spread_position;
        _config.lookback_window = cfg.lookback_window;
    }

    void reset() override;

    const char* typeName() const override { return "pairs_trading"; }

    //==========================================================================
    // Configuration
    //==========================================================================

    void setConfig(const PairsTradingConfig& config) { _config = config; }
    const PairsTradingConfig& getConfig() const { return _config; }

    //==========================================================================
    // Data Update
    //==========================================================================

    /// Update with new price pair
    void updatePrices(double price1, double price2, uint64_t timestamp);

    //==========================================================================
    // Analysis
    //==========================================================================

    /// Test for cointegration

    /// Get current hedge ratio
    double getHedgeRatio() const { return _current_beta; }

    /// Check if pair is valid for trading
    bool isValidPair() const { return _is_valid_pair; }

    static constexpr const char* getName() { return "PairsTrading"; }

private:
    //==========================================================================
    // Internal Methods
    //==========================================================================

    void updateBeta();
    void updateSpread();
    double calculateResidual(double price1, double price2) const;
    bool checkPairValidity() const;
    double calculatePositionSize(double zscore) const;
    double calculateConfidence(double zscore) const;

    //==========================================================================
    // Configuration
    //==========================================================================

    PairsTradingConfig _config;

    //==========================================================================
    // Price History
    //==========================================================================

    RingBuffer<double, 256> _price1_history;
    RingBuffer<double, 256> _price2_history;
    RingBuffer<double, 256> _residual_history;
    uint64_t _last_update;

    //==========================================================================
    // Beta and Spread
    //==========================================================================

    double _current_beta;
    double _current_alpha;
    double _residual_mean;
    double _residual_std;
    double _current_zscore;

    uint32_t _tick_count;
    uint32_t _last_rebalance_tick;

    //==========================================================================
    // Validity
    //==========================================================================

    bool _is_valid_pair;
    mutable double _current_correlation;

    //==========================================================================
    // Signal State
    //==========================================================================

    uint64_t _last_signal_time;
    double _entry_zscore;
};

} // namespace futu
