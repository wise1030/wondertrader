/*!
 * \file CorrelationManager.cpp
 * \brief Cross-Contract Correlation Management Implementation
 */
#include "CorrelationManager.h"
#include "../Includes/WTSDataDef.hpp"
#include "../WTSTools/WTSLogger.h"
#include <algorithm>
#include <numeric>
#include <cmath>

namespace futu {

CorrelationManager::CorrelationManager()
{
}

void CorrelationManager::addContract(const std::string& code, double multiplier)
{
    if (_price_histories.find(code) == _price_histories.end())
    {
        ContractData data;
        data.multiplier = multiplier;
        _price_histories[code] = std::move(data);
    }
}

void CorrelationManager::addRelation(const std::string& code1, const std::string& code2,
                                     RelationType type, double expectedBeta)
{
    // Ensure both contracts are tracked
    addContract(code1);
    addContract(code2);
    
    std::string key = getPairKey(code1, code2);
    _relation_types[key] = type;
    _expected_betas[key] = expectedBeta;
    
    // Initialize correlation stats
    if (_correlations.find(key) == _correlations.end())
    {
        CorrelationStats stats;
        stats.beta = expectedBeta;
        _correlations[key] = stats;
    }
    
    WTSLogger::debug("[CORR] Added relation: {} with type={}, beta={}", 
                     key, static_cast<int>(type), expectedBeta);
}

void CorrelationManager::removeContract(const std::string& code)
{
    _price_histories.erase(code);
    
    // Remove all correlations involving this contract
    std::vector<std::string> to_remove;
    for (auto& kv : _correlations)
    {
        if (kv.first.find(code) != std::string::npos)
        {
            to_remove.push_back(kv.first);
        }
    }
    for (const auto& key : to_remove)
    {
        _correlations.erase(key);
        _relation_types.erase(key);
        _expected_betas.erase(key);
    }
}

void CorrelationManager::onTick(const std::string& code, double price, uint64_t timestamp)
{
    auto it = _price_histories.find(code);
    if (it == _price_histories.end())
        return;
    
    ContractData& data = it->second;
    
    // Calculate return (log return)
    if (data.last_price > 0 && price > 0)
    {
        double ret = std::log(price / data.last_price);
        data.prices.push(price);  // Store price for correlation
    }
    else
    {
        data.prices.push(price);
    }
    
    data.last_price = price;
    data.last_update = timestamp;
    
    // Update correlations with other tracked contracts
    for (auto& kv : _price_histories)
    {
        if (kv.first != code)
        {
            std::string pair_key = getPairKey(code, kv.first);
            
            // Only update if this pair has a defined relation
            if (_relation_types.find(pair_key) != _relation_types.end())
            {
                updateCorrelation(code, kv.first);
            }
        }
    }
}

void CorrelationManager::onTick(wtp::WTSTickData* tick)
{
    if (!tick) return;
    
    std::string code = tick->code();
    double price = tick->price();
    uint64_t timestamp = tick->actiontime();
    
    onTick(code, price, timestamp);
}

void CorrelationManager::updateCorrelation(const std::string& code1, const std::string& code2)
{
    auto it1 = _price_histories.find(code1);
    auto it2 = _price_histories.find(code2);
    
    if (it1 == _price_histories.end() || it2 == _price_histories.end())
        return;
    
    const ContractData& data1 = it1->second;
    const ContractData& data2 = it2->second;
    
    size_t size = std::min(data1.prices.size(), data2.prices.size());
    if (size < 10)  // Need minimum samples
        return;
    
    // Extract price series (use most recent window_size prices)
    size_t window = std::min(static_cast<size_t>(_config.window_size), size);
    
    std::vector<double> p1, p2;
    p1.reserve(window);
    p2.reserve(window);
    
    size_t start1 = data1.prices.size() - window;
    size_t start2 = data2.prices.size() - window;
    
    for (size_t i = 0; i < window; ++i)
    {
        p1.push_back(data1.prices[start1 + i]);
        p2.push_back(data2.prices[start2 + i]);
    }
    
    // Calculate correlation
    double correlation = calculateCorrelation(p1, p2);
    
    // Calculate regression for beta
    double beta, alpha;
    if (_config.auto_calculate_beta)
    {
        calculateRegression(p1, p2, beta, alpha);
    }
    else
    {
        std::string key = getPairKey(code1, code2);
        auto it = _expected_betas.find(key);
        beta = (it != _expected_betas.end()) ? it->second : 1.0;
        alpha = 0;
    }
    
    // Calculate spread statistics
    std::vector<double> spreads;
    spreads.reserve(window);
    for (size_t i = 0; i < window; ++i)
    {
        spreads.push_back(p2[i] - beta * p1[i]);  // Spread = price2 - beta * price1
    }
    
    double spread_mean = std::accumulate(spreads.begin(), spreads.end(), 0.0) / window;
    double spread_var = 0;
    for (double s : spreads)
    {
        spread_var += (s - spread_mean) * (s - spread_mean);
    }
    double spread_std = std::sqrt(spread_var / window);
    
    // Current z-score
    double current_spread = data2.last_price - beta * data1.last_price;
    double zscore = (spread_std > 0) ? (current_spread - spread_mean) / spread_std : 0;
    
    // Update stored stats
    std::string key = getPairKey(code1, code2);
    CorrelationStats& stats = _correlations[key];
    stats.correlation = correlation;
    stats.beta = beta;
    stats.alpha = alpha;
    stats.spread_mean = spread_mean;
    stats.spread_std = spread_std;
    stats.zscore = zscore;
    stats.sample_count = window;
    stats.last_update = std::max(data1.last_update, data2.last_update);
}

double CorrelationManager::calculateCorrelation(const std::vector<double>& x, 
                                                 const std::vector<double>& y)
{
    size_t n = x.size();
    if (n != y.size() || n < 2)
        return 0;
    
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0, sum_y2 = 0;
    
    for (size_t i = 0; i < n; ++i)
    {
        sum_x += x[i];
        sum_y += y[i];
        sum_xy += x[i] * y[i];
        sum_x2 += x[i] * x[i];
        sum_y2 += y[i] * y[i];
    }
    
    double numerator = n * sum_xy - sum_x * sum_y;
    double denominator = std::sqrt((n * sum_x2 - sum_x * sum_x) * 
                                    (n * sum_y2 - sum_y * sum_y));
    
    if (denominator == 0)
        return 0;
    
    return numerator / denominator;
}

void CorrelationManager::calculateRegression(const std::vector<double>& x, 
                                              const std::vector<double>& y,
                                              double& beta, double& alpha)
{
    size_t n = x.size();
    if (n < 2)
    {
        beta = 1;
        alpha = 0;
        return;
    }
    
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_x2 = 0;
    
    for (size_t i = 0; i < n; ++i)
    {
        sum_x += x[i];
        sum_y += y[i];
        sum_xy += x[i] * y[i];
        sum_x2 += x[i] * x[i];
    }
    
    double mean_x = sum_x / n;
    double mean_y = sum_y / n;
    
    // beta = Cov(x,y) / Var(x)
    double cov = sum_xy / n - mean_x * mean_y;
    double var_x = sum_x2 / n - mean_x * mean_x;
    
    if (var_x == 0)
    {
        beta = 1;
        alpha = mean_y - mean_x;
    }
    else
    {
        beta = cov / var_x;
        alpha = mean_y - beta * mean_x;
    }
}

CorrelationStats CorrelationManager::getCorrelation(const std::string& code1, 
                                                     const std::string& code2) const
{
    std::string key = getPairKey(code1, code2);
    auto it = _correlations.find(key);
    if (it != _correlations.end())
        return it->second;
    
    // Try reverse order
    key = getPairKey(code2, code1);
    it = _correlations.find(key);
    if (it != _correlations.end())
    {
        // Return with inverted beta (for reverse lookup)
        CorrelationStats stats = it->second;
        if (stats.beta != 0)
            stats.beta = 1.0 / stats.beta;
        return stats;
    }
    
    return CorrelationStats();
}

std::vector<std::pair<std::string, CorrelationStats>> 
CorrelationManager::getCorrelationsFor(const std::string& code) const
{
    std::vector<std::pair<std::string, CorrelationStats>> result;
    
    for (const auto& kv : _correlations)
    {
        // Check if this pair involves the code
        if (kv.first.find(code) != std::string::npos)
        {
            // Extract the other code
            size_t pos = kv.first.find('/');
            if (pos != std::string::npos)
            {
                std::string other;
                if (kv.first.substr(0, pos) == code)
                    other = kv.first.substr(pos + 1);
                else
                    other = kv.first.substr(0, pos);
                
                result.push_back({other, kv.second});
            }
        }
    }
    
    return result;
}

double CorrelationManager::getSpreadZScore(const std::string& code1, 
                                           const std::string& code2) const
{
    CorrelationStats stats = getCorrelation(code1, code2);
    return stats.zscore;
}

double CorrelationManager::getAggregateDelta(
    const std::map<std::string, double>& positions) const
{
    double total_delta = 0;
    
    // Simple sum of positions adjusted by multiplier
    // For more sophisticated aggregation, use beta-adjusted deltas
    
    for (const auto& kv : positions)
    {
        const std::string& code = kv.first;
        double position = kv.second;
        
        auto it = _price_histories.find(code);
        if (it != _price_histories.end())
        {
            total_delta += position * it->second.multiplier;
        }
        else
        {
            total_delta += position;  // Default multiplier = 1
        }
    }
    
    return total_delta;
}

double CorrelationManager::getHedgeRatio(const std::string& code1, 
                                         const std::string& code2) const
{
    CorrelationStats stats = getCorrelation(code1, code2);
    
    // Only use beta for hedging if correlation is strong enough
    if (std::abs(stats.correlation) >= _config.min_correlation)
    {
        return stats.beta;
    }
    
    return 1.0;  // Default 1:1 hedge
}

bool CorrelationManager::hasSpreadOpportunity(const std::string& code1, 
                                               const std::string& code2,
                                               double& spreadRatio) const
{
    CorrelationStats stats = getCorrelation(code1, code2);
    
    // Check correlation threshold
    if (std::abs(stats.correlation) < _config.min_correlation)
        return false;
    
    // Check z-score threshold
    if (std::abs(stats.zscore) < _config.spread_z_threshold)
        return false;
    
    spreadRatio = stats.zscore / _config.spread_z_threshold;
    return true;
}

std::vector<CorrelationManager::SpreadTradeSignal> CorrelationManager::getSpreadSignals() const
{
    std::vector<SpreadTradeSignal> signals;
    
    for (const auto& kv : _correlations)
    {
        const CorrelationStats& stats = kv.second;
        
        // Skip low correlation pairs
        if (std::abs(stats.correlation) < _config.min_correlation)
            continue;
        
        // Check for spread opportunity
        if (std::abs(stats.zscore) >= _config.spread_z_threshold)
        {
            SpreadTradeSignal signal;
            
            // Parse pair key to get codes
            size_t pos = kv.first.find('/');
            if (pos == std::string::npos)
                continue;
            
            std::string code1 = kv.first.substr(0, pos);
            std::string code2 = kv.first.substr(pos + 1);
            
            // Determine long/short based on z-score
            // Positive z-score = spread high = short spread
            // Negative z-score = spread low = long spread
            if (stats.zscore > 0)
            {
                // Spread is high, expect mean reversion down
                // Short code2, Long code1 (with hedge ratio)
                signal.long_code = code1;
                signal.short_code = code2;
                signal.ratio = stats.beta;
            }
            else
            {
                // Spread is low, expect mean reversion up
                // Long code2, Short code1 (with hedge ratio)
                signal.long_code = code2;
                signal.short_code = code1;
                signal.ratio = stats.beta;
            }
            
            signal.zscore = stats.zscore;
            signal.expected_return = std::abs(stats.zscore) * stats.spread_std;
            signal.confidence = std::min(1.0, std::abs(stats.correlation) * 
                                        std::abs(stats.zscore) / _config.spread_z_threshold);
            
            signals.push_back(signal);
        }
    }
    
    // Sort by confidence (highest first)
    std::sort(signals.begin(), signals.end(), 
              [](const SpreadTradeSignal& a, const SpreadTradeSignal& b) {
                  return a.confidence > b.confidence;
              });
    
    return signals;
}

std::string CorrelationManager::getPairKey(const std::string& code1, const std::string& code2)
{
    // Always use sorted order for consistent keys
    if (code1 < code2)
        return code1 + "/" + code2;
    return code2 + "/" + code1;
}

void CorrelationManager::reset()
{
    for (auto& kv : _price_histories)
    {
        kv.second.prices.clear();
        kv.second.last_price = 0;
        kv.second.last_update = 0;
    }
    
    for (auto& kv : _correlations)
    {
        kv.second = CorrelationStats();
    }
}

} // namespace futu
