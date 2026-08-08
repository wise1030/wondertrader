// src/WtFutuCore/CorrelationManager.h
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include "../Includes/FasterDefs.h"
#include "FutuConfig.h"
#include "SpreadCalculator.h"

NS_WTP_BEGIN
class WTSTickData;
NS_WTP_END

namespace futu {

enum class RelationType
{
    CROSS_TERM,
    CROSS_PRODUCT,
    SPREAD_CONTRACT,
    CUSTOM
};

struct CorrelationStats
{
    double correlation;
    double beta;
    double alpha;
    double spread_mean;
    double spread_std;
    double zscore;
    uint64_t sample_count;
    uint64_t last_update;

    CorrelationStats() : correlation(0), beta(1), alpha(0), spread_mean(0), spread_std(0), zscore(0), sample_count(0), last_update(0) {}
    inline bool isSpreadHigh(double threshold = 2.0) const { return zscore > threshold; }
    inline bool isSpreadLow(double threshold = -2.0) const { return zscore < threshold; }
};

struct CorrelationConfig
{
    uint32_t window_size;
    double min_correlation;
    double spread_z_threshold;
    bool auto_calculate_beta;

    CorrelationConfig() : window_size(100), min_correlation(0.5), spread_z_threshold(2.0), auto_calculate_beta(true) {}

    static CorrelationConfig fromVariant(wtp::WTSVariant* v) {
        CorrelationConfig c;
        c.window_size = FutuConfig::readUInt32(v, "windowSize", 100);
        c.min_correlation = FutuConfig::readDouble(v, "minCorrelation", 0.5);
        c.spread_z_threshold = FutuConfig::readDouble(v, "spreadZThreshold", 2.0);
        return c;
    }
};

class CorrelationManager
{
public:
    CorrelationManager();
    ~CorrelationManager() = default;

    void setConfig(const CorrelationConfig& config) { _config = config; }
    const CorrelationConfig& getConfig() const { return _config; }

    void addContract(const std::string& code, double multiplier = 1.0);
    void addRelation(const std::string& code1, const std::string& code2, RelationType type = RelationType::CROSS_TERM, double expectedBeta = 1.0);
    void removeContract(const std::string& code);

    void onTick(const std::string& code, double price, uint64_t timestamp);
    void onTick(wtp::WTSTickData* tick);

    CorrelationStats getCorrelation(const std::string& code1, const std::string& code2) const;
    std::vector<std::pair<std::string, CorrelationStats>> getCorrelationsFor(const std::string& code) const;

    double getSpreadZScore(const std::string& code1, const std::string& code2) const;
    double getAggregateDelta(const std::map<std::string, double>& positions) const;
    double getHedgeRatio(const std::string& code1, const std::string& code2) const;

    struct SpreadTradeSignal {
        std::string long_code;
        std::string short_code;
        double ratio;
        double zscore;
        double expected_return;
        double confidence;
    };
    bool hasSpreadOpportunity(const std::string& code1, const std::string& code2, double& spreadRatio) const;
    std::vector<SpreadTradeSignal> getSpreadSignals() const;

    size_t getContractCount() const { return _contracts.size(); }
    size_t getCorrelationCount() const { return _calculators.size(); }
    void reset();

private:
    CorrelationConfig _config;

    struct ContractInfo {
        double multiplier;
        double last_price;
    };
    wtp::wt_hashmap<std::string, ContractInfo> _contracts;

    // Shared SpreadCalculator logic reduces code duplication massively!
    wtp::wt_hashmap<std::string, std::shared_ptr<SpreadCalculator>> _calculators;

    // F1: code → (calculator*, isLeg1) 预建索引 — onTick 零字符串操作
    //   (旧实现每 tick × 每 pair: find('/') + 2×substr + 2×string 比较)
    //   注意: leg1 恒为字典序较小代码 (与原 getPairKey 解析语义一致)
    wtp::wt_hashmap<std::string, std::vector<std::pair<SpreadCalculator*, bool>>> _code_calcs;
    // F2: 双向 pair 索引 — getCalculator/getHedgeRatio 零 pair_key 堆分配
    //   (旧 getPairKey "code1/code2" ≈25字符 > SSO, 每查询 2-3 次 malloc)
    struct PairEntry {
        std::shared_ptr<SpreadCalculator> calc;
        RelationType type = RelationType::CROSS_TERM;
        double expected_beta = 1.0;
    };
    wtp::wt_hashmap<std::string, wtp::wt_hashmap<std::string, PairEntry>> _pair_index;

    static std::string getPairKey(const std::string& code1, const std::string& code2);
    std::shared_ptr<SpreadCalculator> getCalculator(const std::string& code1, const std::string& code2) const;
};

} // namespace futu
