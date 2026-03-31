/*!
 * \file CorrelationManager.h
 * \brief Cross-Contract Correlation Management for Multi-Contract Market Making
 * 
 * Provides real-time correlation tracking for:
 *   - Cross-term arbitrage (same underlying, different expirations)
 *   - Cross-product arbitrage (related products, e.g., IF/IC/IH)
 *   - Portfolio delta aggregation across correlated contracts
 *   - Spread trading signals
 * 
 * Uses efficient rolling window for correlation calculation.
 */
#pragma once

#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <cstdint>
#include "../Share/RingBuffer.hpp"
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

namespace futu {

/// Correlation pair identifier
struct CorrelationPair
{
    std::string code1;
    std::string code2;
    
    bool operator<(const CorrelationPair& other) const
    {
        if (code1 != other.code1) return code1 < other.code1;
        return code2 < other.code2;
    }
    
    std::string toString() const
    {
        return code1 + "/" + code2;
    }
};

/// Correlation statistics
struct CorrelationStats
{
    double correlation;         ///< Pearson correlation coefficient (-1 to 1)
    double beta;                ///< Linear regression beta (code2 = alpha + beta * code1)
    double alpha;               ///< Linear regression alpha
    double spread_mean;         ///< Mean spread between contracts
    double spread_std;          ///< Standard deviation of spread
    double zscore;              ///< Current spread z-score
    uint64_t sample_count;      ///< Number of samples used
    uint64_t last_update;       ///< Last update timestamp
    
    CorrelationStats()
        : correlation(0), beta(1), alpha(0)
        , spread_mean(0), spread_std(0), zscore(0)
        , sample_count(0), last_update(0)
    {}
    
    /// Check if spread is above threshold (mean-reversion signal)
    inline bool isSpreadHigh(double threshold = 2.0) const
    {
        return zscore > threshold;
    }
    
    /// Check if spread is below threshold (mean-reversion signal)
    inline bool isSpreadLow(double threshold = -2.0) const
    {
        return zscore < threshold;
    }
};

/// Contract relationship type
enum class RelationType
{
    CROSS_TERM,         ///< Same underlying, different expiration (e.g., IF2503/IF2506)
    CROSS_PRODUCT,      ///< Related products (e.g., IF/IC, Cu/Al)
    SPREAD_CONTRACT,    ///< Official spread contract
    CUSTOM              ///< User-defined relationship
};

/// Correlation Manager Configuration
struct CorrelationConfig
{
    uint32_t window_size;       ///< Rolling window for correlation calculation
    double min_correlation;     ///< Minimum correlation to consider for hedging
    double spread_z_threshold;  ///< Z-score threshold for spread trading
    bool auto_calculate_beta;   ///< Auto-calculate beta for delta adjustment
    
    CorrelationConfig()
        : window_size(100)
        , min_correlation(0.5)
        , spread_z_threshold(2.0)
        , auto_calculate_beta(true)
    {}
};

/// Correlation Manager for Multi-Contract Market Making
class CorrelationManager
{
public:
    CorrelationManager();
    ~CorrelationManager() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const CorrelationConfig& config) { _config = config; }
    const CorrelationConfig& getConfig() const { return _config; }
    
    //==========================================================================
    // Contract Management
    //==========================================================================
    
    /// Add a contract to track
    void addContract(const std::string& code, double multiplier = 1.0);
    
    /// Define a correlation relationship between two contracts
    void addRelation(const std::string& code1, const std::string& code2,
                     RelationType type = RelationType::CROSS_TERM,
                     double expectedBeta = 1.0);
    
    /// Remove a contract
    void removeContract(const std::string& code);
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// Update with tick data
    void onTick(const std::string& code, double price, uint64_t timestamp);
    void onTick(wtp::WTSTickData* tick);
    
    //==========================================================================
    // Correlation Analysis
    //==========================================================================
    
    /// Get correlation between two contracts
    CorrelationStats getCorrelation(const std::string& code1, 
                                    const std::string& code2) const;
    
    /// Get all correlations for a contract
    std::vector<std::pair<std::string, CorrelationStats>> 
    getCorrelationsFor(const std::string& code) const;
    
    /// Get spread z-score for a pair
    double getSpreadZScore(const std::string& code1, const std::string& code2) const;
    
    //==========================================================================
    // Delta Aggregation
    //==========================================================================
    
    /// Calculate aggregate delta across correlated contracts
    /// Uses beta-adjusted delta for cross-term/cross-product hedging
    double getAggregateDelta(const std::map<std::string, double>& positions) const;
    
    /// Get hedge ratio between two contracts
    double getHedgeRatio(const std::string& code1, const std::string& code2) const;
    
    //==========================================================================
    // Spread Trading Signals
    //==========================================================================
    
    /// Check if spread trading opportunity exists
    bool hasSpreadOpportunity(const std::string& code1, const std::string& code2,
                              double& spreadRatio) const;
    
    /// Get recommended spread trades
    struct SpreadTradeSignal {
        std::string long_code;
        std::string short_code;
        double ratio;           ///< Position ratio
        double zscore;          ///< Current z-score
        double expected_return; ///< Expected return from mean reversion
        double confidence;      ///< Signal confidence (0-1)
    };
    std::vector<SpreadTradeSignal> getSpreadSignals() const;
    
    //==========================================================================
    // Statistics
    //==========================================================================
    
    /// Get number of tracked contracts
    size_t getContractCount() const { return _price_histories.size(); }
    
    /// Get number of correlation pairs
    size_t getCorrelationCount() const { return _correlations.size(); }
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void reset();
    
private:
    CorrelationConfig _config;
    
    /// Price history for each contract (using RingBuffer for efficiency)
    struct ContractData
    {
        RingBuffer<double, 256> prices;  // capacity must be power of 2
        double multiplier;
        double last_price;
        uint64_t last_update;
        
        ContractData() : multiplier(1.0), last_price(0), last_update(0) {}
    };
    wtp::wt_hashmap<std::string, ContractData> _price_histories;
    
    /// Correlation statistics for each pair
    wtp::wt_hashmap<std::string, CorrelationStats> _correlations;
    
    /// Relation types for each pair
    wtp::wt_hashmap<std::string, RelationType> _relation_types;
    
    /// Expected betas (from config or auto-calculated)
    wtp::wt_hashmap<std::string, double> _expected_betas;
    
    /// Update correlation for a pair
    void updateCorrelation(const std::string& code1, const std::string& code2);
    
    /// Calculate Pearson correlation
    static double calculateCorrelation(const std::vector<double>& x, 
                                       const std::vector<double>& y);
    
    /// Calculate linear regression (beta, alpha)
    static void calculateRegression(const std::vector<double>& x, 
                                    const std::vector<double>& y,
                                    double& beta, double& alpha);
    
    /// Get pair key for storage
    static std::string getPairKey(const std::string& code1, const std::string& code2);
};

} // namespace futu
