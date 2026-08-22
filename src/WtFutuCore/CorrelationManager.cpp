// src/WtFutuCore/CorrelationManager.cpp
#include "CorrelationManager.h"
#include "../Includes/WTSDataDef.hpp"
#include "../WTSTools/WTSLogger.h"
#include <cmath>
#include <algorithm>

namespace futu
{

CorrelationManager::CorrelationManager() {}

void CorrelationManager::addContract(const std::string& code, double multiplier)
{
    if (_contracts.find(code) == _contracts.end()) {
        _contracts[code] = {multiplier, 0.0};
    }
}

void CorrelationManager::addRelation(const std::string& code1,
                                     const std::string& code2,
                                     RelationType type,
                                     double expectedBeta)
{
    if (code1 == code2)
        return;

    std::string key = getPairKey(code1, code2);
    if (_calculators.find(key) == _calculators.end()) {
        auto calc = std::make_shared<SpreadCalculator>();
        SpreadCalculatorConfig cfg;
        cfg.window_size = _config.window_size > 0 ? _config.window_size : 100;
        cfg.min_samples = 10;
        // 跨品种 pair 的 beta 截断带围绕 expectedBeta 设置 (默认带宽 ±3x),
        // 避免真实 beta (如 0.3/0.05) 被默认下限 0.7 错误截断.
        if (expectedBeta > 0) {
            cfg.beta_min = expectedBeta * 0.3;
            cfg.beta_max = expectedBeta * 3.0;
        }
        calc->setConfig(cfg);
        calc->setSpreadType(SpreadType::SIMPLE_DIFF);
        _calculators[key] = calc;
        // F1/F2: 维护预建索引 (onTick/getCalculator/getHedgeRatio 零字符串操作)
        //   leg1 恒为字典序较小代码 (与原 getPairKey 解析语义一致, beta 方向不变)
        const bool c1_is_leg1 = (code1 < code2);
        _code_calcs[code1].emplace_back(calc.get(), c1_is_leg1);
        _code_calcs[code2].emplace_back(calc.get(), !c1_is_leg1);
        PairEntry entry{calc, type, expectedBeta};
        _pair_index[code1][code2] = entry;
        _pair_index[code2][code1] = entry;
    }
}


void CorrelationManager::onTick(const std::string& code, double price, uint64_t timestamp)
{
    auto it = _contracts.find(code);
    if (it != _contracts.end()) {
        it->second.last_price = price;
    }

    // F1: 预建索引直达, 消除每 tick × 每 pair 的字符串拆分/比较
    auto cc = _code_calcs.find(code);
    if (cc != _code_calcs.end()) {
        for (const auto& [calc, isLeg1] : cc->second) {
            if (isLeg1)
                calc->onLeg1Tick(price, timestamp);
            else
                calc->onLeg2Tick(price, timestamp);
        }
    }
}

void CorrelationManager::onTick(wtp::WTSTickData* tick)
{
    if (tick) {
        double price = tick->price();
        // nan/inf 防御 — 防止脏 tick 污染 SpreadCalculator 的 leg_history → calculateBeta=nan
        if (!std::isfinite(price)) {
            static thread_local uint64_t cm_nan_cnt = 0;
            if ((++cm_nan_cnt & 0xFFF) == 1) {
                WTSLogger::warn("CorrelationManager: {} non-finite price={} ts={} (cnt={}), dropping",
                                tick->code(),
                                price,
                                tick->actiontime(),
                                cm_nan_cnt);
            }
            return;
        }
        onTick(tick->code(), price, tick->actiontime());
    }
}

std::string CorrelationManager::getPairKey(const std::string& code1, const std::string& code2)
{
    return (code1 < code2) ? (code1 + "/" + code2) : (code2 + "/" + code1);
}

std::shared_ptr<SpreadCalculator> CorrelationManager::getCalculator(const std::string& code1,
                                                                    const std::string& code2) const
{
    // F2: 双向预建索引直达, 消除 getPairKey 堆分配 (热路径每非 anchor tick 调用)
    auto it1 = _pair_index.find(code1);
    if (it1 != _pair_index.end()) {
        auto it2 = it1->second.find(code2);
        if (it2 != it1->second.end()) {
            return it2->second.calc;
        }
    }
    return nullptr;
}

CorrelationStats CorrelationManager::getCorrelation(const std::string& code1, const std::string& code2) const
{
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

double CorrelationManager::getSpreadZScore(const std::string& code1, const std::string& code2) const
{
    auto calc = getCalculator(code1, code2);
    return calc ? calc->getZScore() : 0.0;
}


double CorrelationManager::getHedgeRatio(const std::string& code1, const std::string& code2) const
{
    // ============================================================
    // hedge_ratio 按关系类型分叉:
    //
    // 跨期 (CROSS_TERM, 同品种不同月份):
    //   return 1.0
    //   理由: 同品种 multiplier 相同,做市 max_position 按手数设限,
    //         Delta 直接等于净手数与 max_position/max_delta 语义对齐。
    //         ec2609 做空 2 手 = 2 手 ec 空头,不是 0.586 手。
    //
    // 跨品种/其他 (CROSS_PRODUCT/SPREAD_CONTRACT/CUSTOM):
    //   hedge_ratio = (p_c1 × m_c1 × price_beta) / (p_c2 × m_c2)
    //   即"持有 1 手 code1 等价于多少手 code2 的美元敞口"
    //
    // 未注册关系 (无 _relation_types 条目):
    //   按跨期处理 (return 1.0) — 保守默认
    // ============================================================

    if (code1 == code2)
        return 1.0;

    // 1. 查关系类型 (F2: 预建索引直达, 消除 getPairKey 堆分配)
    auto pi1 = _pair_index.find(code1);
    if (pi1 == _pair_index.end())
        return 1.0; // 未注册关系,保守默认
    auto pi2 = pi1->second.find(code2);
    if (pi2 == pi1->second.end())
        return 1.0; // 未注册关系,保守默认
    const PairEntry& entry = pi2->second;

    // 2. 跨期: 直接返回 1.0
    if (entry.type == RelationType::CROSS_TERM)
        return 1.0;

    // 3. 跨品种/其他: 货值等价计算
    const auto& calc = entry.calc;
    if (!calc)
        return 1.0;

    auto it1 = _contracts.find(code1);
    auto it2 = _contracts.find(code2);
    if (it1 == _contracts.end() || it2 == _contracts.end())
        return 1.0;

    double m1 = it1->second.multiplier;
    double m2 = it2->second.multiplier;
    double p1 = it1->second.last_price;
    double p2 = it2->second.last_price;

    if (!std::isfinite(p1) || !std::isfinite(p2) || p1 <= 0 || p2 <= 0)
        return 1.0;
    if (!std::isfinite(m1) || !std::isfinite(m2) || m1 <= 0 || m2 <= 0)
        return 1.0;

    double price_beta_lex = calc->getBeta();
    double price_beta_user;
    if (!std::isfinite(price_beta_lex) || price_beta_lex <= 0) {
        price_beta_user = 1.0;
    } else {
        bool user_aligned_with_lex = (code1 < code2);
        price_beta_user = user_aligned_with_lex ? price_beta_lex : (1.0 / price_beta_lex);
    }

    double hedge_ratio = (p1 * m1 * price_beta_user) / (p2 * m2);

    if (!std::isfinite(hedge_ratio) || hedge_ratio <= 0)
        return 1.0;
    if (hedge_ratio < 0.05)
        hedge_ratio = 0.05;
    if (hedge_ratio > 20.0)
        hedge_ratio = 20.0;

    return hedge_ratio;
}



void CorrelationManager::reset()
{
    _contracts.clear();
    _calculators.clear();
    _code_calcs.clear();
    _pair_index.clear();
}

} // namespace futu
