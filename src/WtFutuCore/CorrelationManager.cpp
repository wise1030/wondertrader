// src/WtFutuCore/CorrelationManager.cpp
#include "CorrelationManager.h"
#include "../Includes/WTSDataDef.hpp"
#include "../WTSTools/WTSLogger.h"
#include <cmath>

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
        size_t slash_pos = it->first.find('/');
        if (slash_pos != std::string::npos) {
            std::string leg1 = it->first.substr(0, slash_pos);
            std::string leg2 = it->first.substr(slash_pos + 1);
            if (leg1 == code || leg2 == code) {
                _relation_types.erase(it->first);
                _expected_betas.erase(it->first);
                it = _calculators.erase(it);
                continue;
            }
        }
        ++it;
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
        double price = tick->price();
        // nan/inf 防御 — 防止脏 tick 污染 SpreadCalculator 的 leg_history → calculateBeta=nan
        if (!std::isfinite(price)) {
            static thread_local uint64_t cm_nan_cnt = 0;
            if ((++cm_nan_cnt & 0xFFF) == 1) {
                WTSLogger::warn("CorrelationManager: {} non-finite price={} ts={} (cnt={}), dropping",
                    tick->code(), price, tick->actiontime(), cm_nan_cnt);
            }
            return;
        }
        onTick(tick->code(), price, tick->actiontime());
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
    // ============================================================
    // hedge_ratio 语义统一定义:
    //   "持有 1 手 code1,等价于多少手 code2 的价格变动美元敞口"
    //
    // 公式:
    //   hedge_ratio(c1→c2) = (p_c1 × m_c1 × price_beta_user) / (p_c2 × m_c2)
    //
    //   其中:
    //   - p_ci × m_ci = 1 手 ci 的价格变动美元敞口(价格涨 1 元的 PnL)
    //   - price_beta_user = "c1 价格涨 1 单位 → c2 价格涨多少单位"(收益率敏感度)
    //
    // 与 FutuPortfolio 的契约:
    //   - delta() = position × hedge_ratio  (累加成可比 delta)
    //   - getNetDelta() = Σ excess_pos × hedge_ratio
    //   - hedgeQty = -netDelta / anchor->hedge_ratio  (反算成 anchor 手数)
    //
    // 历史 bug:之前直接 return calc->getBeta(),把"收益率 beta"误用作
    //   "手数对冲权重",在陡峭期限结构(EC 2607=3700/2609=1996)下,
    //   ec2609 持仓的实际美元敞口被严重低估,skew 触发不及时导致 Delta 失控。
    // ============================================================

    if (code1 == code2) return 1.0;

    auto calc = getCalculator(code1, code2);
    if (!calc) return 1.0;

    // 1. 拿两腿合约信息(multiplier + last_price)
    auto it1 = _contracts.find(code1);
    auto it2 = _contracts.find(code2);
    if (it1 == _contracts.end() || it2 == _contracts.end()) return 1.0;

    double m1 = it1->second.multiplier;
    double m2 = it2->second.multiplier;
    double p1 = it1->second.last_price;
    double p2 = it2->second.last_price;

    // 数据未就绪(冷启动 last_price=0):退化到 1.0,等待 onTick 填充。
    // 调用侧(UftFutuMmStrategy)有"sample_count<100 不更新"保护,初始 cs->hedge_ratio
    // 也由 on_tick 路径用纯货值比初始化(见 ContractState::hedge_ratio_initialized)。
    if (!std::isfinite(p1) || !std::isfinite(p2) || p1 <= 0 || p2 <= 0) return 1.0;
    if (!std::isfinite(m1) || !std::isfinite(m2) || m1 <= 0 || m2 <= 0) return 1.0;

    // 2. 价格 beta(SpreadCalculator 字典序方向)
    //    SpreadCalculator 内部:lex_leg1 = min(c1,c2), lex_leg2 = max(c1,c2)
    //    回归方向: Δlex_leg2 ≈ beta × Δlex_leg1
    double price_beta_lex = calc->getBeta();

    double price_beta_user;
    if (!std::isfinite(price_beta_lex) || price_beta_lex <= 0) {
        // beta 异常(冷启动/数据缺失):退化到 beta=1 假设,只用货值比
        price_beta_user = 1.0;
    } else {
        // 字典序方向 → 用户传入方向修正
        // - 若 code1 < code2(用户与字典序对齐):price_beta_user = beta_lex
        //   含义:c1=lex_leg1,c2=lex_leg2,Δc2 ≈ beta × Δc1,即 c1 涨 1 → c2 涨 beta
        // - 否则(用户与字典序相反):price_beta_user = 1/beta_lex
        //
        // EC 案例 (c1=ec2609, c2=ec2607):
        //   字典序 lex_leg1=ec2607, lex_leg2=ec2609 (因 ec2607 < ec2609)
        //   beta_lex ≈ 0.92(ec2607 涨 1 → ec2609 涨 0.92)
        //   user 与 lex 相反 → price_beta_user = 1/0.92 ≈ 1.087
        //   含义:ec2609 涨 1 → ec2607 涨 1.087(数学上 1/β 近似 OLS 反向 β)
        bool user_aligned_with_lex = (code1 < code2);
        price_beta_user = user_aligned_with_lex ? price_beta_lex : (1.0 / price_beta_lex);
    }

    // 3. 货值等价手数计算
    //    hedge_ratio = (1 手 c1 美元敞口 × c1→c2 收益率敏感度) / (1 手 c2 美元敞口)
    double hedge_ratio = (p1 * m1 * price_beta_user) / (p2 * m2);

    // 4. 物理合理区间钳制 [0.05, 20]
    //    覆盖跨期(同 multiplier,价差几倍内)和跨品种(multiplier 不同,价格量级不同)实际场景
    //    异常值(beta 失稳 / 价格闪崩 / 数据污染)钳到边界,等三层保护处理
    if (!std::isfinite(hedge_ratio) || hedge_ratio <= 0) return 1.0;
    if (hedge_ratio < 0.05) hedge_ratio = 0.05;
    if (hedge_ratio > 20.0) hedge_ratio = 20.0;

    return hedge_ratio;
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
