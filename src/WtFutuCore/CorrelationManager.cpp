// src/WtFutuCore/CorrelationManager.cpp
#include "CorrelationManager.h"
#include "../Includes/WTSDataDef.hpp"
#include "../WTSTools/WTSLogger.h"

namespace futu {

CorrelationManager::CorrelationManager() {}

void CorrelationManager::addContract(const std::string& code, double multiplier) {
    if (_contracts.find(code) == _contracts.end()) {
        _contracts[code] = {multiplier, 0.0};
    }
}

void CorrelationManager::addRelation(const std::string& code1, const std::string& code2, RelationType type, double expectedBeta) {
    if (code1 == code2) return;
    
    std::string key = getPairKey(code1, code2);
    if (_calculators.find(key) == _calculators.end()) {
        auto calc = std::make_shared<SpreadCalculator>();
        SpreadCalculatorConfig cfg;
        cfg.window_size = _config.window_size > 0 ? _config.window_size : 100;
        cfg.min_samples = 10;
        calc->setConfig(cfg);
        calc->setSpreadType(SpreadType::SIMPLE_DIFF);
        _calculators[key] = calc;
        _relation_types[key] = type;
        _expected_betas[key] = expectedBeta;
    }
}

void CorrelationManager::removeContract(const std::string& code) {
    _contracts.erase(code);
    for (auto it = _calculators.begin(); it != _calculators.end();) {
        if (it->first.find(code) != std::string::npos) {
            _relation_types.erase(it->first);
            _expected_betas.erase(it->first);
            it = _calculators.erase(it);
        } else {
            ++it;
        }
    }
}

void CorrelationManager::onTick(const std::string& code, double price, uint64_t timestamp) {
    auto it = _contracts.find(code);
    if (it != _contracts.end()) {
        it->second.last_price = price;
    }

    for (auto& pair : _calculators) {
        size_t slash_pos = pair.first.find('/');
        if (slash_pos == std::string::npos) continue;
        
        std::string leg1 = pair.first.substr(0, slash_pos);
        std::string leg2 = pair.first.substr(slash_pos + 1);
        
        if (code == leg1) {
            pair.second->onLeg1Tick(price, timestamp);
        } else if (code == leg2) {
            pair.second->onLeg2Tick(price, timestamp);
        }
    }
}

void CorrelationManager::onTick(wtp::WTSTickData* tick) {
    if (tick) {
        onTick(tick->code(), tick->price(), tick->actiontime());
    }
}

std::string CorrelationManager::getPairKey(const std::string& code1, const std::string& code2) {
    return (code1 < code2) ? (code1 + "/" + code2) : (code2 + "/" + code1);
}

std::shared_ptr<SpreadCalculator> CorrelationManager::getCalculator(const std::string& code1, const std::string& code2) const {
    auto it = _calculators.find(getPairKey(code1, code2));
    if (it != _calculators.end()) {
        return it->second;
    }
    return nullptr;
}

CorrelationStats CorrelationManager::getCorrelation(const std::string& code1, const std::string& code2) const {
    CorrelationStats stats;
    auto calc = getCalculator(code1, code2);
    if (calc) {
        stats.correlation = calc->getCorrelation();
        stats.beta = calc->getBeta();
        stats.spread_mean = calc->getMean();
        stats.spread_std = calc->getStdDev();
        stats.zscore = calc->getZScore();
        stats.sample_count = calc->getSampleCount();
        stats.last_update = 0;
    }
    return stats;
}

std::vector<std::pair<std::string, CorrelationStats>> CorrelationManager::getCorrelationsFor(const std::string& code) const {
    std::vector<std::pair<std::string, CorrelationStats>> result;
    for (const auto& pair : _calculators) {
        size_t slash_pos = pair.first.find('/');
        if (slash_pos != std::string::npos) {
            std::string leg1 = pair.first.substr(0, slash_pos);
            std::string leg2 = pair.first.substr(slash_pos + 1);
            if (leg1 == code) {
                result.push_back({leg2, getCorrelation(code, leg2)});
            } else if (leg2 == code) {
                result.push_back({leg1, getCorrelation(code, leg1)});
            }
        }
    }
    return result;
}

double CorrelationManager::getSpreadZScore(const std::string& code1, const std::string& code2) const {
    auto calc = getCalculator(code1, code2);
    return calc ? calc->getZScore() : 0.0;
}

double CorrelationManager::getAggregateDelta(const std::map<std::string, double>& positions) const {
    double total_delta = 0.0;
    for (const auto& pos : positions) {
        auto it = _contracts.find(pos.first);
        if (it != _contracts.end()) {
            double multiplier = it->second.multiplier;
            total_delta += pos.second * multiplier * it->second.last_price;
        }
    }
    return total_delta;
}

double CorrelationManager::getHedgeRatio(const std::string& code1, const std::string& code2) const {
    auto calc = getCalculator(code1, code2);
    if (!calc) return 1.0;
    
    std::string key = getPairKey(code1, code2);
    
    // 根据关系类型决定 hedge_ratio
    // 使用 EMA 平滑后的 beta，减少价格跳跃和短期波动影响
    auto rel_it = _relation_types.find(key);
    if (rel_it != _relation_types.end()) {
        RelationType rel_type = rel_it->second;
        
        // 获取平滑后的 beta（log return 回归 + EMA 平滑）
        double smoothed_beta = calc->getBeta();  // 返回 EMA 平滑后的 beta
        
        switch (rel_type) {
            case RelationType::CROSS_TERM:
                // 同品种跨期合约：使用平滑后的 beta
                // 理论上 beta ≈ 1.0，但实际可能因流动性差异、基差变化而偏离
                // 平滑后的 beta 能更好地反映稳定的对冲比例
                return smoothed_beta;
                
            case RelationType::CROSS_PRODUCT:
                // 跨品种：使用平滑后的 beta
                // beta 反映收益率敏感度关系
                return smoothed_beta;
                
            case RelationType::SPREAD_CONTRACT:
            case RelationType::CUSTOM:
            default:
                return smoothed_beta;
        }
    }
    
    // 未知关系类型：返回 1.0
    return 1.0;
}

bool CorrelationManager::hasSpreadOpportunity(const std::string& code1, const std::string& code2, double& spreadRatio) const {
    auto calc = getCalculator(code1, code2);
    if (calc && std::abs(calc->getZScore()) > _config.spread_z_threshold) {
        spreadRatio = 1.0;
        return true;
    }
    return false;
}

std::vector<CorrelationManager::SpreadTradeSignal> CorrelationManager::getSpreadSignals() const {
    return {};
}

void CorrelationManager::reset() {
    _contracts.clear();
    _calculators.clear();
    _relation_types.clear();
    _expected_betas.clear();
}

} // namespace futu
