/*!
 * \file SpreadCalculator.cpp
 * \brief Spread Calculator Implementation
 *
 * Performance Optimizations:
 *   - Welford's online algorithm for O(1) mean/variance updates
 *   - Scalar calculations for RingBuffer compatibility
 *
 * NOTE: SIMD removed because RingBuffer uses non-contiguous memory layout.
 *       SIMD vector loads (_mm256_loadu_pd) require contiguous memory,
 *       but RingBuffer's operator[] maps logical indices to physical indices.
 *       Using SIMD on RingBuffer causes boundary violations and crashes.
 */
#include "SpreadCalculator.h"
#include "../../WTSTools/WTSLogger.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace futu
{

//==============================================================================
// SpreadCalculator Implementation
//==============================================================================

SpreadCalculator::SpreadCalculator()
    : _spread_type(SpreadType::WEIGHTED), _leg1_ratio(1.0), _leg2_ratio(1.0), _leg1_price(0), _leg2_price(0),
      _leg1_multiplier(1.0), _leg2_multiplier(1.0), _last_leg1_update(0), _last_leg2_update(0), _leg1_fresh(false),
      _leg2_fresh(false), _current_spread(0), _spread_mean(0), _spread_std(0), _zscore(0), _ema_spread(0),
      _correlation(0), _beta(1.0), _smoothed_beta(1.0) // 初始值 1.0，与 beta 同步
      ,
      _half_life(0), _last_update(0), _initialized(false), _ewma_var(0), _welford_n(0)
{}

void SpreadCalculator::setLegRatios(double leg1_ratio, double leg2_ratio)
{
    _leg1_ratio = leg1_ratio;
    _leg2_ratio = leg2_ratio;
}

void SpreadCalculator::onLeg1Tick(double price, uint64_t timestamp)
{
    _leg1_price = price;
    _last_leg1_update = timestamp;
    _leg1_fresh = true; // 标记leg1有新数据

    // 同步tick配对机制
    // 之前每次收到任一合约tick都push，导致另一合约用旧价格
    // 大量log_return=0稀释了beta计算，使同品种跨期beta降到BETA_MIN
    // 现在改为：只在leg2也有新数据（_leg2_fresh=true）时才push
    // push后清除_leg2_fresh标记，避免重复push
    if (_leg2_price > 0 && _leg2_fresh) {
        _current_spread = calculateSpread(_leg1_price, _leg2_price);
        _spread_history.push(_current_spread);
        _leg1_history.push(_leg1_price);
        _leg2_history.push(_leg2_price);
        _last_update = timestamp;
        _leg2_fresh = false; // 消费leg2的新鲜标记

        updateStatistics();
    }
}

void SpreadCalculator::onLeg2Tick(double price, uint64_t timestamp)
{
    _leg2_price = price;
    _last_leg2_update = timestamp;
    _leg2_fresh = true; // 标记leg2有新数据

    // 对称处理 — leg2新数据到达时标记为fresh
    // 下次leg1 tick到来时检查_leg2_fresh，确保两个合约价格同步
    if (_leg1_price > 0 && _leg1_fresh) {
        _current_spread = calculateSpread(_leg1_price, _leg2_price);
        _spread_history.push(_current_spread);
        _leg1_history.push(_leg1_price);
        _leg2_history.push(_leg2_price);
        _last_update = timestamp;
        _leg1_fresh = false; // 消费leg1的新鲜标记

        updateStatistics();
    }
}

double SpreadCalculator::calculateSpread(double price1, double price2) const
{
    if (price2 <= 0)
        return 0;
    if (price1 <= 0)
        return 0; // LOG_DIFF/RATIO模式下price1<=0也会产生NaN/Inf

    switch (_spread_type) {
    case SpreadType::SIMPLE_DIFF:
        return price1 - price2;

    case SpreadType::RATIO:
        return price1 / price2;

    case SpreadType::LOG_DIFF:
        return std::log(price1) - std::log(price2);

    case SpreadType::WEIGHTED:
        // Adjust for contract multipliers and hedge ratios
        return _leg1_ratio * price1 * _leg1_multiplier - _leg2_ratio * price2 * _leg2_multiplier;

    case SpreadType::BASIS:
        return price1 - price2; // Futures - Spot

    default:
        return price1 - price2;
    }
}

void SpreadCalculator::updateStatistics()
{
    // EWMA decay statistics — replaces cumulative Welford.
    // Adapts to regime shifts: alpha controls half-life (~138 ticks at alpha=0.005).
    // For stationary spreads (same-variant arb), converges similarly to Welford.
    // For non-stationary spreads (cross-variant), tracks local statistics better.

    if (std::isnan(_current_spread)) {
        return;
    }

    _welford_n++;

    constexpr double alpha = 0.005; // EWMA decay factor (half-life ~138 ticks)

    if (!_initialized) {
        _spread_mean = _current_spread;
        _ewma_var = 0;
        _ema_spread = _current_spread;
        _initialized = true;
    } else {
        double prev_mean = _spread_mean;
        _spread_mean = alpha * _current_spread + (1.0 - alpha) * prev_mean;
        double diff = _current_spread - _spread_mean;
        _ewma_var = alpha * diff * diff + (1.0 - alpha) * _ewma_var;

        _ema_spread = _config.ema_alpha * _current_spread + (1.0 - _config.ema_alpha) * _ema_spread;
    }

    _spread_std = std::sqrt(_ewma_var);

    if (_spread_std > 1e-10) {
        _zscore = (_current_spread - _spread_mean) / _spread_std;
    } else {
        _zscore = 0;
    }

    // Update correlation and beta periodically (单次扫描 log-return 同算两者, 消除重复 std::log)
    if (_welford_n % 10 == 0 && _welford_n >= _config.min_samples) {
        computeCorrelationAndBeta();

        _smoothed_beta = _config.ema_alpha * _beta + (1 - _config.ema_alpha) * _smoothed_beta;

        // beta 截断范围可配 (默认 0.7/1.5 适合同品种跨期; 跨品种 pair 真实 beta 可能
        // 远低于 0.7, 由 CorrelationManager 按 expectedBeta 设带宽, 避免错误截断).
        if (_smoothed_beta < _config.beta_min)
            _smoothed_beta = _config.beta_min;
        if (_smoothed_beta > _config.beta_max)
            _smoothed_beta = _config.beta_max;
    }

    // Estimate half-life periodically
    if (_welford_n % 50 == 0 && _welford_n >= 50) {
        _half_life = estimateHalfLife();
    }
}

double SpreadCalculator::calculateCorrelation() const
{
    size_t n = std::min(_leg1_history.size(), _leg2_history.size());
    if (n < _config.min_samples + 1) // 需要 n+1 个价格计算 n 个 return
        return 0;

    // 使用 log return 计算相关性（解决非平稳序列问题）
    // log_return = ln(P_t / P_{t-1}) = ln(P_t) - ln(P_{t-1})
    // 优势：平稳性、消除价格水平影响、统计稳定性好

    double mean1 = 0, mean2 = 0;
    size_t valid_count = 0;

    for (size_t i = 1; i < n; ++i) {
        double prev1 = _leg1_history[i - 1];
        double prev2 = _leg2_history[i - 1];
        double curr1 = _leg1_history[i];
        double curr2 = _leg2_history[i];

        // 跳过无效价格
        if (prev1 <= 0 || prev2 <= 0 || curr1 <= 0 || curr2 <= 0)
            continue;

        double ret1 = std::log(curr1 / prev1);
        double ret2 = std::log(curr2 / prev2);

        mean1 += ret1;
        mean2 += ret2;
        valid_count++;
    }

    if (valid_count < _config.min_samples)
        return 0;

    mean1 /= valid_count;
    mean2 /= valid_count;

    double cov = 0, var1 = 0, var2 = 0;

    for (size_t i = 1; i < n; ++i) {
        double prev1 = _leg1_history[i - 1];
        double prev2 = _leg2_history[i - 1];
        double curr1 = _leg1_history[i];
        double curr2 = _leg2_history[i];

        if (prev1 <= 0 || prev2 <= 0 || curr1 <= 0 || curr2 <= 0)
            continue;

        double ret1 = std::log(curr1 / prev1);
        double ret2 = std::log(curr2 / prev2);

        double d1 = ret1 - mean1;
        double d2 = ret2 - mean2;
        cov += d1 * d2;
        var1 += d1 * d1;
        var2 += d2 * d2;
    }

    if (var1 < 1e-10 || var2 < 1e-10)
        return 0;

    return cov / std::sqrt(var1 * var2);
}

double SpreadCalculator::calculateBeta() const
{
    size_t n = std::min(_leg1_history.size(), _leg2_history.size());
    if (n < _config.min_samples + 1) // 需要 n+1 个价格计算 n 个 return
        return 1.0;

    // OLS regression on log returns: ret_Y = alpha + beta * ret_X + epsilon
    // 其中 ret_Y = ln(leg1_t / leg1_{t-1}), ret_X = ln(leg2_t / leg2_{t-1})
    //
    // 优势：
    // 1. 使用平稳序列，避免伪回归（spurious regression）
    // 2. beta 代表收益率敏感度，而非价格水平敏感度
    // 3. 消除价格水平差异的影响
    // 4. 统计稳定性更好

    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
    size_t valid_count = 0;

    for (size_t i = 1; i < n; ++i) {
        double prev1 = _leg1_history[i - 1];
        double prev2 = _leg2_history[i - 1];
        double curr1 = _leg1_history[i];
        double curr2 = _leg2_history[i];

        // 跳过无效价格
        if (prev1 <= 0 || prev2 <= 0 || curr1 <= 0 || curr2 <= 0)
            continue;

        // Log return
        double ret_x = std::log(curr2 / prev2); // leg2 return
        double ret_y = std::log(curr1 / prev1); // leg1 return

        sum_x += ret_x;
        sum_y += ret_y;
        sum_xy += ret_x * ret_y;
        sum_xx += ret_x * ret_x;
        valid_count++;
    }

    if (valid_count < _config.min_samples)
        return 1.0;

    double denom = valid_count * sum_xx - sum_x * sum_x;
    if (std::abs(denom) < 1e-10)
        return 1.0;

    double beta = (valid_count * sum_xy - sum_x * sum_y) / denom;

    // Beta 解释：
    // beta ≈ 1.0 表示两合约收益率高度同步（同品种跨期）
    // beta > 1.0 表示 leg1 收益率波动更大
    // beta < 1.0 表示 leg2 收益率波动更大

    return beta;
}

void SpreadCalculator::computeCorrelationAndBeta()
{
    // 单次扫描计算 log-return 到复用缓冲, correlation+beta 共享,
    // 消除原 calculateCorrelation(2 pass) + calculateBeta(1 pass) 共 6 次/元素 的重复 std::log.
    // 数学公式与原两方法完全一致 (valid 过滤/skip 逻辑相同).
    size_t n = std::min(_leg1_history.size(), _leg2_history.size());
    if (n < _config.min_samples + 1) // 需要 n+1 个价格计算 n 个 return
    {
        _correlation = 0;
        _beta = 1.0;
        return;
    }

    _ret1_buf.clear();
    _ret2_buf.clear();
    _ret1_buf.reserve(n - 1);
    _ret2_buf.reserve(n - 1);
    for (size_t i = 1; i < n; ++i) {
        double prev1 = _leg1_history[i - 1];
        double prev2 = _leg2_history[i - 1];
        double curr1 = _leg1_history[i];
        double curr2 = _leg2_history[i];
        if (prev1 <= 0 || prev2 <= 0 || curr1 <= 0 || curr2 <= 0)
            continue;
        _ret1_buf.push_back(std::log(curr1 / prev1)); // leg1 return
        _ret2_buf.push_back(std::log(curr2 / prev2)); // leg2 return
    }

    size_t valid_count = _ret1_buf.size();
    if (valid_count < _config.min_samples) {
        _correlation = 0;
        _beta = 1.0;
        return;
    }

    // Correlation (mean of returns) + Beta 累加项, 单次循环完成
    double mean1 = 0, mean2 = 0;
    for (size_t i = 0; i < valid_count; ++i) {
        mean1 += _ret1_buf[i];
        mean2 += _ret2_buf[i];
    }
    mean1 /= valid_count;
    mean2 /= valid_count;

    double cov = 0, var1 = 0, var2 = 0;
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
    for (size_t i = 0; i < valid_count; ++i) {
        double r1 = _ret1_buf[i]; // leg1 return
        double r2 = _ret2_buf[i]; // leg2 return
        double d1 = r1 - mean1;
        double d2 = r2 - mean2;
        cov += d1 * d2;
        var1 += d1 * d1;
        var2 += d2 * d2;
        // Beta: x=leg2 return, y=leg1 return (与原 calculateBeta 相同)
        sum_x += r2;
        sum_y += r1;
        sum_xy += r2 * r1;
        sum_xx += r2 * r2;
    }

    _correlation = (var1 < 1e-10 || var2 < 1e-10) ? 0.0 : cov / std::sqrt(var1 * var2);

    double denom = valid_count * sum_xx - sum_x * sum_x;
    if (std::abs(denom) < 1e-10) {
        _beta = 1.0;
    } else {
        _beta = (valid_count * sum_xy - sum_x * sum_y) / denom;
    }
}

double SpreadCalculator::estimateHalfLife() const
{
    // Estimate half-life using Ornstein-Uhlenbeck process
    // dX = theta * (mu - X) * dt + sigma * dW
    // half_life = ln(2) / theta

    size_t n = _spread_history.size();
    if (n < 30)
        return 0;

    // Regression: X_t - X_{t-1} = alpha + theta * X_{t-1} + epsilon
    double sum_x = 0, sum_dx = 0, sum_xx = 0, sum_x_dx = 0;
    size_t count = 0;

    for (size_t i = 1; i < n; ++i) {
        double x = _spread_history[i - 1];
        double dx = _spread_history[i] - _spread_history[i - 1];
        sum_x += x;
        sum_dx += dx;
        sum_xx += x * x;
        sum_x_dx += x * dx;
        ++count;
    }

    if (count == 0)
        return 0;

    double mean_x = sum_x / count;
    double mean_dx = sum_dx / count;

    // V8-A11: 零方差 (价差恒定) 时分母为 0 -> theta=0/0=NaN, `NaN >= 0` 恒 false
    // 滑过下方守卫 -> half_life=NaN 绕过 MeanReversion 半衰期过滤。
    double denom = sum_xx - count * mean_x * mean_x;
    if (denom < 1e-12)
        return 0;

    // Calculate theta (slope)
    double theta = (sum_x_dx - count * mean_x * mean_dx) / denom;

    // theta should be negative for mean-reverting process
    if (theta >= 0)
        return 0; // Not mean-reverting

    // Half-life = ln(2) / |theta|
    double half_life = std::log(2.0) / std::abs(theta);

    return half_life;
}

bool SpreadCalculator::isMeanReverting() const
{
    // Simple check: negative theta (from half-life calculation) indicates mean reversion
    return _half_life > 0 && _half_life < 1000; // Reasonable half-life range
}

SpreadState SpreadCalculator::getState() const
{
    SpreadState state;
    state.current_spread = _current_spread;
    // V8-A2: current_price 此前全链路从未赋值 -> TrendFollowing 止损分支
    // (entry_price>0 && current_price>0 恒 false) 是死代码, 趋势价差仓无价格止损。
    // 价差"价格"即价差值本身 (pnl_pct 以 spread 为分母, 接近 0 的价差该策略本身不应持仓)
    state.current_price = _current_spread;
    state.spread_mean = _spread_mean;
    state.spread_std = _spread_std;
    state.zscore = _zscore;
    state.correlation = _correlation;
    state.beta = _beta;
    state.half_life = _half_life;
    state.leg1_price = _leg1_price;
    state.leg2_price = _leg2_price;
    state.last_update = _last_update;
    state.is_active = hasEnoughSamples();
    state.is_converging = isMeanReverting();
    return state;
}

void SpreadCalculator::reset()
{
    _spread_history.clear();
    _leg1_history.clear();
    _leg2_history.clear();
    _leg1_price = 0;
    _leg2_price = 0;
    _current_spread = 0;
    _spread_mean = 0;
    _spread_std = 0;
    _zscore = 0;
    _ema_spread = 0;
    _correlation = 0;
    _beta = 1.0;
    _smoothed_beta = 1.0;
    _half_life = 0;
    _initialized = false;

    // Reset EWMA state
    _ewma_var = 0;
    _welford_n = 0;
}

//==============================================================================
// SpreadCalculatorManager Implementation
//==============================================================================

SpreadCalculatorManager::SpreadCalculatorManager() {}

void SpreadCalculatorManager::addSpreadPair(const SpreadPairConfig& pair_config)
{
    auto calc = std::make_unique<SpreadCalculator>();
    calc->setConfig(_config);
    calc->setSpreadType(pair_config.spread_type);
    calc->setLegRatios(pair_config.leg1_ratio, pair_config.leg2_ratio);

    _calculators[pair_config.pair_id] = std::move(calc);
    _pair_configs[pair_config.pair_id] = pair_config;

    // Update contract to pairs mapping
    _contract_to_pairs[pair_config.leg1_code].push_back(pair_config.pair_id);
    _contract_to_pairs[pair_config.leg2_code].push_back(pair_config.pair_id);
}

void SpreadCalculatorManager::removeSpreadPair(const std::string& pair_id)
{
    auto it = _calculators.find(pair_id);
    if (it == _calculators.end())
        return;

    // Get config for this pair
    auto config_it = _pair_configs.find(pair_id);
    if (config_it != _pair_configs.end()) {
        const auto& config = config_it->second;

        // Remove from contract mappings
        auto& leg1_pairs = _contract_to_pairs[config.leg1_code];
        leg1_pairs.erase(std::remove(leg1_pairs.begin(), leg1_pairs.end(), pair_id), leg1_pairs.end());

        auto& leg2_pairs = _contract_to_pairs[config.leg2_code];
        leg2_pairs.erase(std::remove(leg2_pairs.begin(), leg2_pairs.end(), pair_id), leg2_pairs.end());

        _pair_configs.erase(config_it);
    }

    _calculators.erase(it);
}

void SpreadCalculatorManager::onTick(const std::string& code, double price, double multiplier, uint64_t timestamp)
{
    auto it = _contract_to_pairs.find(code);
    if (it == _contract_to_pairs.end())
        return;

    for (const auto& pair_id : it->second) {
        auto calc_it = _calculators.find(pair_id);
        if (calc_it == _calculators.end())
            continue;

        auto config_it = _pair_configs.find(pair_id);
        if (config_it == _pair_configs.end())
            continue;

        const auto& config = config_it->second;

        if (code == config.leg1_code) {
            calc_it->second->onLeg1Tick(price, timestamp);
        } else if (code == config.leg2_code) {
            calc_it->second->onLeg2Tick(price, timestamp);
        }
    }
}

SpreadState SpreadCalculatorManager::getSpreadState(const std::string& pair_id) const
{
    auto it = _calculators.find(pair_id);
    if (it == _calculators.end())
        return SpreadState();

    return it->second->getState();
}

std::vector<SpreadState> SpreadCalculatorManager::getAllStates() const
{
    std::vector<SpreadState> states;
    states.reserve(_calculators.size());

    for (const auto& kv : _calculators) {
        states.push_back(kv.second->getState());
    }

    return states;
}

SpreadCalculator* SpreadCalculatorManager::getCalculator(const std::string& pair_id)
{
    auto it = _calculators.find(pair_id);
    if (it == _calculators.end())
        return nullptr;
    return it->second.get();
}

bool SpreadCalculatorManager::isSpreadContract(const std::string& code) const
{
    return _contract_to_pairs.find(code) != _contract_to_pairs.end();
}

const std::vector<std::string>& SpreadCalculatorManager::getPairsForContract(const std::string& code) const
{
    // 返回 const 引用避免每 tick 按值拷贝 vector<string> (pair 注册后 map 只读, 引用安全).
    static const std::vector<std::string> empty;
    auto it = _contract_to_pairs.find(code);
    if (it == _contract_to_pairs.end())
        return empty;
    return it->second;
}

void SpreadCalculatorManager::reset()
{
    for (auto& kv : _calculators) {
        kv.second->reset();
    }
}

} // namespace futu
