/*!
 * \file PairsTradingStrategy.cpp
 * \brief Pairs Trading Strategy Implementation
 */
#include "PairsTradingStrategy.h"
#include <cmath>
#include <algorithm>

namespace futu {

PairsTradingStrategy::PairsTradingStrategy()
    : _last_update(0)
    , _current_beta(1.0)
    , _current_alpha(0)
    , _residual_mean(0)
    , _residual_std(0)
    , _current_zscore(0)
    , _tick_count(0)
    , _last_rebalance_tick(0)
    , _is_valid_pair(false)
    , _current_correlation(0)
    , _last_signal_time(0)
    , _entry_zscore(0)
{
}

void PairsTradingStrategy::updatePrices(double price1, double price2, uint64_t timestamp)
{
    _price1_history.push(price1);
    _price2_history.push(price2);
    _last_update = timestamp;
    _tick_count++;
    
    updateBeta();
    updateSpread();
    
    // Periodic validity check
    if (_tick_count % 100 == 0)
    {
        _is_valid_pair = checkPairValidity();
    }
}

void PairsTradingStrategy::updateBeta()
{
    size_t n = _price1_history.size();
    if (n < _config.min_samples)
        return;
    
    // Check if we need to rebalance beta
    if (_config.use_dynamic_beta && 
        (_tick_count - _last_rebalance_tick) >= _config.rebalance_interval)
    {
        // OLS regression: P1 = alpha + beta * P2 + epsilon
        double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
        size_t start = (n > _config.lookback_window) ? (n - _config.lookback_window) : 0;
        
        for (size_t i = start; i < n; ++i)
        {
            double x = _price2_history[i];
            double y = _price1_history[i];
            sum_x += x;
            sum_y += y;
            sum_xy += x * y;
            sum_xx += x * x;
        }
        
        size_t count = n - start;
        double denom = count * sum_xx - sum_x * sum_x;
        
        if (std::abs(denom) > 1e-10)
        {
            double new_beta = (count * sum_xy - sum_x * sum_y) / denom;
            double new_alpha = (sum_y - new_beta * sum_x) / count;
            
            // Smooth beta update
            _current_beta = _config.beta_smoothing * new_beta + 
                           (1 - _config.beta_smoothing) * _current_beta;
            _current_alpha = _config.beta_smoothing * new_alpha + 
                            (1 - _config.beta_smoothing) * _current_alpha;
        }
        
        _last_rebalance_tick = _tick_count;
    }
}

void PairsTradingStrategy::updateSpread()
{
    size_t n = _price1_history.size();
    if (n < 2)
        return;
    
    // Calculate residual: spread = P1 - beta * P2 - alpha
    double residual = calculateResidual(_price1_history[n-1], _price2_history[n-1]);
    _residual_history.push(residual);
    
    size_t res_n = _residual_history.size();
    if (res_n < _config.min_samples)
        return;
    
    // Calculate residual statistics
    double sum = 0, sq_sum = 0;
    size_t start = std::max(0, (int)res_n - (int)_config.lookback_window);
    
    for (size_t i = start; i < res_n; ++i)
    {
        sum += _residual_history[i];
    }
    _residual_mean = sum / (res_n - start);
    
    for (size_t i = start; i < res_n; ++i)
    {
        double diff = _residual_history[i] - _residual_mean;
        sq_sum += diff * diff;
    }
    size_t df = res_n - start - 1;  // 自由度
    if (df > 0)
    {
        _residual_std = std::sqrt(sq_sum / static_cast<double>(df));
    }
    else
    {
        _residual_std = 0;
        return;  // 样本不足，不计算zscore
    }
    
    // Calculate Z-Score
    if (_residual_std > 1e-10)
    {
        _current_zscore = (residual - _residual_mean) / _residual_std;
    }
}

double PairsTradingStrategy::calculateResidual(double price1, double price2) const
{
    return price1 - _current_beta * price2 - _current_alpha;
}

bool PairsTradingStrategy::checkPairValidity() const
{
    size_t n = _price1_history.size();
    if (n < _config.min_samples)
        return false;
    
    // 只使用 lookback_window 内的数据计算相关性（与 beta 估计一致）
    size_t start = (n > _config.lookback_window) ? (n - _config.lookback_window) : 0;
    size_t count = n - start;
    
    // Calculate correlation
    double mean1 = 0, mean2 = 0;
    for (size_t i = start; i < n; ++i)
    {
        mean1 += _price1_history[i];
        mean2 += _price2_history[i];
    }
    mean1 /= count;
    mean2 /= count;
    
    double cov = 0, var1 = 0, var2 = 0;
    for (size_t i = start; i < n; ++i)
    {
        double d1 = _price1_history[i] - mean1;
        double d2 = _price2_history[i] - mean2;
        cov += d1 * d2;
        var1 += d1 * d1;
        var2 += d2 * d2;
    }
    
    // 修复：将相关性存储到 _current_correlation（之前是局部变量，从未赋值）
    _current_correlation = (var1 > 1e-10 && var2 > 1e-10) ? 
                           cov / std::sqrt(var1 * var2) : 0;
    
    // Check validity criteria
    if (std::abs(_current_correlation) < _config.min_correlation)
        return false;
    
    if (_residual_std > _config.max_spread_std)
        return false;
    
    return true;
}

CointegrationResult PairsTradingStrategy::testCointegration() const
{
    CointegrationResult result;
    
    size_t n = _residual_history.size();
    if (n < 30)
        return result;
    
    // Simplified ADF test: regressing delta_residual on residual_lag
    // Model: Δy_t = γ * y_{t-1} + ε_t
    // H0: γ >= 0 (不协整) vs H1: γ < 0 (协整)
    
    double sum_y = 0, sum_x = 0, sum_xy = 0, sum_xx = 0;
    
    for (size_t i = 1; i < n; ++i)
    {
        double y = _residual_history[i] - _residual_history[i-1];  // Delta residual
        double x = _residual_history[i-1];  // Lagged residual
        sum_y += y;
        sum_x += x;
        sum_xy += x * y;
        sum_xx += x * x;
    }
    
    size_t count = n - 1;
    double denom = count * sum_xx - sum_x * sum_x;
    
    if (std::abs(denom) < 1e-10)
        return result;
    
    double gamma = (count * sum_xy - sum_x * sum_y) / denom;
    
    // 计算 gamma 的标准误差
    double se_gamma = std::sqrt(1.0 / denom);
    result.test_statistic = gamma / se_gamma;
    
    // 基于 MacKinnon (1996) 近似 p-value 计算
    // ADF 检验统计量服从修正的 Dickey-Fuller 分布
    // 使用经验公式: log(p) ≈ a + b*|t| + c*|t|^2
    // 参数来自 MacKinnon Table 1, Model 1 (no constant, no trend), N>25
    double t_stat = std::abs(result.test_statistic);
    
    if (t_stat > 0.01)
    {
        // MacKinnon 近似参数（无截距无趋势模型）
        static const double a = -0.762;
        static const double b = -1.738;
        static const double c = -0.0942;
        
        double log_p = a + b * t_stat + c * t_stat * t_stat;
        result.p_value = std::exp(log_p);
        result.p_value = std::max(0.001, std::min(1.0, result.p_value));
    }
    else
    {
        result.p_value = 0.99;  // 几乎不显著
    }
    
    // 协整判断：test_statistic 显著为负 且 p_value 足够小
    result.is_cointegrated = (result.test_statistic < result.critical_value) && 
                              (result.p_value < 0.05);
    result.beta = _current_beta;
    result.alpha = _current_alpha;
    result.residual_std = _residual_std;
    
    return result;
}

SpreadSignal PairsTradingStrategy::generateSignal(const SpreadState& state, uint64_t current_time)
{
    SpreadSignal signal;
    signal.pair_id = state.pair_id;
    signal.source = ArbitrageStrategy::PAIRS_TRADING;
    signal.timestamp = current_time;
    
    // Check validity
    if (!_is_valid_pair || _residual_history.size() < _config.min_samples)
    {
        signal.type = SpreadSignalType::NONE;
        return signal;
    }
    
    // No position - check for entry
    if (!state.hasPosition())
    {
        if (_current_zscore > _config.entry_z_threshold)
        {
            // Residual too high - expect mean reversion
            signal.type = SpreadSignalType::OPEN_SHORT_SPREAD;
            signal.confidence = calculateConfidence(_current_zscore);
            signal.suggested_size = calculatePositionSize(_current_zscore);
            signal.entry_zscore = _current_zscore;
            signal.target_zscore = _config.exit_z_threshold;
            signal.reason = "Residual above threshold, expecting mean reversion";
            _entry_zscore = _current_zscore;
        }
        else if (_current_zscore < -_config.entry_z_threshold)
        {
            signal.type = SpreadSignalType::OPEN_LONG_SPREAD;
            signal.confidence = calculateConfidence(_current_zscore);
            signal.suggested_size = calculatePositionSize(_current_zscore);
            signal.entry_zscore = _current_zscore;
            signal.target_zscore = -_config.exit_z_threshold;
            signal.reason = "Residual below threshold, expecting mean reversion";
            _entry_zscore = _current_zscore;
        }
    }
    // Has position - check for exit
    else
    {
        // Stop loss
        if (std::abs(_current_zscore) > _config.stop_loss_z)
        {
            signal.type = SpreadSignalType::STOP_LOSS;
            signal.confidence = 1.0;
            signal.suggested_size = std::abs(state.spread_position);
            signal.reason = "Stop loss: residual deviation too large";
        }
        // Timeout
        else if (state.positionDuration(current_time) > _config.convergence_timeout)
        {
            signal.type = SpreadSignalType::TIMEOUT_EXIT;
            signal.confidence = 0.8;
            signal.suggested_size = std::abs(state.spread_position);
            signal.reason = "Timeout: pair did not converge";
        }
        // 退出条件：zscore 回归到0附近才退出（避免过早平仓）
        else if (state.spread_position > 0 && _current_zscore > -_config.exit_z_threshold * 0.3)
        {
            signal.type = SpreadSignalType::CLOSE_LONG_SPREAD;
            signal.confidence = 0.9;
            signal.suggested_size = state.spread_position;
            signal.reason = "Residual reverted near zero, closing long spread";
        }
        else if (state.spread_position < 0 && _current_zscore < _config.exit_z_threshold * 0.3)
        {
            signal.type = SpreadSignalType::CLOSE_SHORT_SPREAD;
            signal.confidence = 0.9;
            signal.suggested_size = std::abs(state.spread_position);
            signal.reason = "Residual reverted near zero, closing short spread";
        }
    }
    
    _last_signal_time = current_time;
    return signal;
}

double PairsTradingStrategy::calculatePositionSize(double zscore) const
{
    double abs_z = std::abs(zscore);
    double ratio = abs_z / _config.entry_z_threshold;
    double size = _config.base_qty * (0.8 + 0.4 * std::min(ratio - 1.0, 1.0));
    return std::min(size, _config.max_position);
}

double PairsTradingStrategy::calculateConfidence(double zscore) const
{
    double abs_z = std::abs(zscore);
    double ratio = abs_z / _config.entry_z_threshold;
    return std::min(0.5 + (ratio - 1.0) * 0.25, 0.95);
}

void PairsTradingStrategy::reset()
{
    _price1_history.clear();
    _price2_history.clear();
    _residual_history.clear();
    _current_beta = 1.0;
    _current_alpha = 0;
    _residual_mean = 0;
    _residual_std = 0;
    _current_zscore = 0;
    _tick_count = 0;
    _last_rebalance_tick = 0;
    _is_valid_pair = false;
    _current_correlation = 0;
    _last_signal_time = 0;
    _entry_zscore = 0;
}

} // namespace futu
