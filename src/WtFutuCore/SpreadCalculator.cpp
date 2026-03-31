/*!
 * \file SpreadCalculator.cpp
 * \brief Spread Calculator Implementation
 * 
 * Performance Optimizations:
 *   - Welford's online algorithm for O(1) mean/variance updates
 *   - SIMD vectorization for correlation/beta calculations
 */
#include "SpreadCalculator.h"
#include <algorithm>
#include <cmath>
#include <numeric>

#ifdef __AVX2__
#include <immintrin.h>
#define SIMD_AVAILABLE 1
#elif defined(__SSE2__)
#include <emmintrin.h>
#define SIMD_AVAILABLE 1
#else
#define SIMD_AVAILABLE 0
#endif

namespace futu {

//==============================================================================
// SIMD Helper Functions for Vectorized Statistics
//==============================================================================

#if SIMD_AVAILABLE && defined(__AVX2__)
/// SIMD-optimized horizontal sum of 4 doubles from __m256d
static inline double hsum_avx(__m256d v)
{
    __m128d vlow = _mm256_castpd256_pd128(v);
    __m128d vhigh = _mm256_extractf128_pd(v, 1);
    vlow = _mm_add_pd(vlow, vhigh);
    __m128d vtmp = _mm_unpackhi_pd(vlow, vlow);
    vlow = _mm_add_pd(vlow, vtmp);
    return _mm_cvtsd_f64(vlow);
}
#endif

//==============================================================================
// SpreadCalculator Implementation
//==============================================================================

SpreadCalculator::SpreadCalculator()
    : _spread_type(SpreadType::WEIGHTED)
    , _leg1_ratio(1.0)
    , _leg2_ratio(1.0)
    , _leg1_price(0)
    , _leg2_price(0)
    , _leg1_multiplier(1.0)
    , _leg2_multiplier(1.0)
    , _last_leg1_update(0)
    , _last_leg2_update(0)
    , _current_spread(0)
    , _spread_mean(0)
    , _spread_std(0)
    , _zscore(0)
    , _ema_spread(0)
    , _correlation(0)
    , _beta(1.0)
    , _alpha(0)
    , _half_life(0)
    , _last_update(0)
    , _initialized(false)
    , _welford_m(0)      // Welford mean
    , _welford_s(0)      // Welford sum of squared deviations
    , _welford_n(0)      // Welford count
{
}

void SpreadCalculator::setLegRatios(double leg1_ratio, double leg2_ratio)
{
    _leg1_ratio = leg1_ratio;
    _leg2_ratio = leg2_ratio;
}

void SpreadCalculator::onLeg1Tick(double price, uint64_t timestamp)
{
    _leg1_price = price;
    _last_leg1_update = timestamp;
    
    if (_leg2_price > 0)
    {
        _current_spread = calculateSpread(_leg1_price, _leg2_price);
        _spread_history.push(_current_spread);
        _leg1_history.push(_leg1_price);
        _leg2_history.push(_leg2_price);
        _last_update = timestamp;
        
        updateStatistics();
    }
}

void SpreadCalculator::onLeg2Tick(double price, uint64_t timestamp)
{
    _leg2_price = price;
    _last_leg2_update = timestamp;
    
    if (_leg1_price > 0)
    {
        _current_spread = calculateSpread(_leg1_price, _leg2_price);
        _spread_history.push(_current_spread);
        _leg1_history.push(_leg1_price);
        _leg2_history.push(_leg2_price);
        _last_update = timestamp;
        
        updateStatistics();
    }
}

double SpreadCalculator::calculateSpread(double price1, double price2) const
{
    if (price2 <= 0) return 0;
    
    switch (_spread_type)
    {
    case SpreadType::SIMPLE_DIFF:
        return price1 - price2;
        
    case SpreadType::RATIO:
        return price1 / price2;
        
    case SpreadType::LOG_DIFF:
        return std::log(price1) - std::log(price2);
        
    case SpreadType::WEIGHTED:
        // Adjust for contract multipliers and hedge ratios
        return _leg1_ratio * price1 * _leg1_multiplier 
             - _leg2_ratio * price2 * _leg2_multiplier;
        
    case SpreadType::BASIS:
        return price1 - price2;  // Futures - Spot
        
    default:
        return price1 - price2;
    }
}

void SpreadCalculator::updateStatistics()
{
    // Welford's online algorithm for O(1) mean/variance updates
    // This is critical for low-latency HFT systems
    
    _welford_n++;
    double delta = _current_spread - _welford_m;
    _welford_m += delta / _welford_n;
    double delta2 = _current_spread - _welford_m;
    _welford_s += delta * delta2;
    
    // Update mean and std using Welford results
    _spread_mean = _welford_m;
    
    if (_welford_n > 1)
    {
        _spread_std = std::sqrt(_welford_s / (_welford_n - 1));
    }
    else
    {
        _spread_std = 0;
    }
    
    // Calculate Z-Score
    if (_spread_std > 1e-10)
    {
        _zscore = (_current_spread - _spread_mean) / _spread_std;
    }
    else
    {
        _zscore = 0;
    }
    
    // Update EMA
    if (!_initialized)
    {
        _ema_spread = _current_spread;
        _initialized = true;
    }
    else
    {
        _ema_spread = _config.ema_alpha * _current_spread + 
                       (1 - _config.ema_alpha) * _ema_spread;
    }
    
    // Update correlation and beta periodically (still needs full scan, but less frequent)
    if (_welford_n % 10 == 0 && _welford_n >= _config.min_samples)
    {
        _correlation = calculateCorrelation();
        _beta = calculateBeta();
    }
    
    // Estimate half-life periodically
    if (_welford_n % 50 == 0 && _welford_n >= 50)
    {
        _half_life = estimateHalfLife();
    }
}

double SpreadCalculator::calculateCorrelation() const
{
    size_t n = std::min(_leg1_history.size(), _leg2_history.size());
    if (n < _config.min_samples)
        return 0;
    
    // SIMD-optimized correlation calculation
    // Uses AVX2 for processing 4 doubles at once
    
#if SIMD_AVAILABLE && defined(__AVX2__)
    if (n >= 8)  // Use SIMD only for sufficient data
    {
        __m256d sum1_vec = _mm256_setzero_pd();
        __m256d sum2_vec = _mm256_setzero_pd();
        
        // Calculate means using SIMD
        size_t i = 0;
        for (; i + 4 <= n; i += 4)
        {
            __m256d v1 = _mm256_loadu_pd(&_leg1_history[i]);
            __m256d v2 = _mm256_loadu_pd(&_leg2_history[i]);
            sum1_vec = _mm256_add_pd(sum1_vec, v1);
            sum2_vec = _mm256_add_pd(sum2_vec, v2);
        }
        
        double mean1 = hsum_avx(sum1_vec) / n;
        double mean2 = hsum_avx(sum2_vec) / n;
        
        // Handle remaining elements
        for (; i < n; ++i)
        {
            mean1 += _leg1_history[i];
            mean2 += _leg2_history[i];
        }
        
        // Calculate covariance and variances using SIMD
        __m256d cov_vec = _mm256_setzero_pd();
        __m256d var1_vec = _mm256_setzero_pd();
        __m256d var2_vec = _mm256_setzero_pd();
        __m256d mean1_vec = _mm256_set1_pd(mean1);
        __m256d mean2_vec = _mm256_set1_pd(mean2);
        
        i = 0;
        for (; i + 4 <= n; i += 4)
        {
            __m256d v1 = _mm256_loadu_pd(&_leg1_history[i]);
            __m256d v2 = _mm256_loadu_pd(&_leg2_history[i]);
            
            __m256d d1 = _mm256_sub_pd(v1, mean1_vec);
            __m256d d2 = _mm256_sub_pd(v2, mean2_vec);
            
            cov_vec = _mm256_fmadd_pd(d1, d2, cov_vec);
            var1_vec = _mm256_fmadd_pd(d1, d1, var1_vec);
            var2_vec = _mm256_fmadd_pd(d2, d2, var2_vec);
        }
        
        double cov = hsum_avx(cov_vec);
        double var1 = hsum_avx(var1_vec);
        double var2 = hsum_avx(var2_vec);
        
        // Handle remaining elements
        for (; i < n; ++i)
        {
            double d1 = _leg1_history[i] - mean1;
            double d2 = _leg2_history[i] - mean2;
            cov += d1 * d2;
            var1 += d1 * d1;
            var2 += d2 * d2;
        }
        
        if (var1 < 1e-10 || var2 < 1e-10)
            return 0;
        
        return cov / std::sqrt(var1 * var2);
    }
#endif
    
    // Scalar fallback for small data or no SIMD
    double mean1 = 0, mean2 = 0;
    for (size_t i = 0; i < n; ++i)
    {
        mean1 += _leg1_history[i];
        mean2 += _leg2_history[i];
    }
    mean1 /= n;
    mean2 /= n;
    
    double cov = 0, var1 = 0, var2 = 0;
    for (size_t i = 0; i < n; ++i)
    {
        double d1 = _leg1_history[i] - mean1;
        double d2 = _leg2_history[i] - mean2;
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
    if (n < _config.min_samples)
        return 1.0;
    
    // OLS regression: Y = alpha + beta * X + epsilon
    // where Y = leg1, X = leg2
    
#if SIMD_AVAILABLE && defined(__AVX2__)
    if (n >= 8)
    {
        __m256d sum_x_vec = _mm256_setzero_pd();
        __m256d sum_y_vec = _mm256_setzero_pd();
        __m256d sum_xy_vec = _mm256_setzero_pd();
        __m256d sum_xx_vec = _mm256_setzero_pd();
        
        size_t i = 0;
        for (; i + 4 <= n; i += 4)
        {
            __m256d x = _mm256_loadu_pd(&_leg2_history[i]);
            __m256d y = _mm256_loadu_pd(&_leg1_history[i]);
            
            sum_x_vec = _mm256_add_pd(sum_x_vec, x);
            sum_y_vec = _mm256_add_pd(sum_y_vec, y);
            sum_xy_vec = _mm256_fmadd_pd(x, y, sum_xy_vec);
            sum_xx_vec = _mm256_fmadd_pd(x, x, sum_xx_vec);
        }
        
        double sum_x = hsum_avx(sum_x_vec);
        double sum_y = hsum_avx(sum_y_vec);
        double sum_xy = hsum_avx(sum_xy_vec);
        double sum_xx = hsum_avx(sum_xx_vec);
        
        // Handle remaining elements
        for (; i < n; ++i)
        {
            double x = _leg2_history[i];
            double y = _leg1_history[i];
            sum_x += x;
            sum_y += y;
            sum_xy += x * y;
            sum_xx += x * x;
        }
        
        double denom = n * sum_xx - sum_x * sum_x;
        if (std::abs(denom) < 1e-10)
            return 1.0;
        
        double beta = (n * sum_xy - sum_x * sum_y) / denom;
        _alpha = (sum_y - beta * sum_x) / n;
        
        return beta;
    }
#endif
    
    // Scalar fallback
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
    
    for (size_t i = 0; i < n; ++i)
    {
        double x = _leg2_history[i];
        double y = _leg1_history[i];
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }
    
    double denom = n * sum_xx - sum_x * sum_x;
    if (std::abs(denom) < 1e-10)
        return 1.0;
    
    double beta = (n * sum_xy - sum_x * sum_y) / denom;
    _alpha = (sum_y - beta * sum_x) / n;
    
    return beta;
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
    
    for (size_t i = 1; i < n; ++i)
    {
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
    
    // Calculate theta (slope)
    double theta = (sum_x_dx - count * mean_x * mean_dx) / 
                   (sum_xx - count * mean_x * mean_x);
    
    // theta should be negative for mean-reverting process
    if (theta >= 0)
        return 0;  // Not mean-reverting
    
    // Half-life = ln(2) / |theta|
    double half_life = std::log(2.0) / std::abs(theta);
    
    return half_life;
}

bool SpreadCalculator::isMeanReverting() const
{
    // Simple check: negative theta (from half-life calculation) indicates mean reversion
    return _half_life > 0 && _half_life < 1000;  // Reasonable half-life range
}

SpreadState SpreadCalculator::getState() const
{
    SpreadState state;
    state.current_spread = _current_spread;
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
    _alpha = 0;
    _half_life = 0;
    _initialized = false;
    
    // Reset Welford state
    _welford_m = 0;
    _welford_s = 0;
    _welford_n = 0;
}

//==============================================================================
// SpreadCalculatorManager Implementation
//==============================================================================

SpreadCalculatorManager::SpreadCalculatorManager()
{
}

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
    if (config_it != _pair_configs.end())
    {
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

void SpreadCalculatorManager::onTick(const std::string& code, double price, 
                                      double multiplier, uint64_t timestamp)
{
    auto it = _contract_to_pairs.find(code);
    if (it == _contract_to_pairs.end())
        return;
    
    for (const auto& pair_id : it->second)
    {
        auto calc_it = _calculators.find(pair_id);
        if (calc_it == _calculators.end())
            continue;
        
        auto config_it = _pair_configs.find(pair_id);
        if (config_it == _pair_configs.end())
            continue;
        
        const auto& config = config_it->second;
        
        if (code == config.leg1_code)
        {
            calc_it->second->onLeg1Tick(price, timestamp);
        }
        else if (code == config.leg2_code)
        {
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
    
    for (const auto& kv : _calculators)
    {
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

std::vector<std::string> SpreadCalculatorManager::getPairsForContract(const std::string& code) const
{
    auto it = _contract_to_pairs.find(code);
    if (it == _contract_to_pairs.end())
        return {};
    return it->second;
}

void SpreadCalculatorManager::reset()
{
    for (auto& kv : _calculators)
    {
        kv.second->reset();
    }
}

} // namespace futu
