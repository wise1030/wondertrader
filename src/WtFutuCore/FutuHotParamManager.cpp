/*!
 * \file FutuHotParamManager.cpp
 * \brief 热更新参数管理器实现 (自 UftFutuMmStrategy 搬移, 逻辑零修改)
 */
#include "FutuHotParamManager.h"
#include "UftFutuMmStrategy.h"
#include "FutuQuoter.h"
#include "SpreadOptimizer.h"
#include "SignalAggregator.h"
#include "StrategyCoordinator.h"
#include "FutuPortfolio.h"
#include "../Includes/IUftStraCtx.h"
#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../Includes/WTSVariant.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace futu
{

void FutuHotParamManager::registerParams(wtp::IUftStraCtx* ctx,
                                         const FutuMmConfig& _config,
                                         const GLFTParams& glft_defaults,
                                         const SignalAggregatorConfig& sig_defaults,
                                         double alpha_sensitivity)
{
    // 报价基础参数
    HotParamEntry hot_defaults[] = {
        {"base_spread", _config.quoting.base_spread, nullptr},
        {"base_qty", _config.quoting.base_qty, nullptr},
        {"level_qty_multiplier", _config.quoting.level_qty_multiplier, nullptr},
        {"level_step", _config.quoting.level_step, nullptr},
        {"max_delta", _config.portfolio.max_delta, nullptr},
        {"alpha_sensitivity", alpha_sensitivity, nullptr},
        {"ofi_weight", sig_defaults.ofi_weight, nullptr},
        {"trade_weight", sig_defaults.trade_weight, nullptr},
        {"book_imbalance_weight", sig_defaults.book_imbalance_weight, nullptr},
        {"momentum_weight", sig_defaults.momentum_weight, nullptr},
        {"lead_lag_weight", sig_defaults.lead_lag_weight, nullptr},
        {"strong_threshold", sig_defaults.strong_threshold, nullptr},
        {"confidence_weight_min", glft_defaults.confidence_weight_min, nullptr},
        {"confidence_weight_max", glft_defaults.confidence_weight_max, nullptr},
        {"phi", glft_defaults.phi, nullptr},
        {"delta_skew_threshold", glft_defaults.delta_skew_threshold, nullptr},
        {"delta_skew_factor", glft_defaults.delta_skew_factor, nullptr},
        {"max_spread_mult", glft_defaults.max_spread_mult, nullptr},
        {"min_spread_mult", glft_defaults.min_spread_mult, nullptr},
        {"depth_sensitivity", glft_defaults.depth_sensitivity, nullptr},
        {"toxicity_spread_factor", glft_defaults.toxicity_spread_factor, nullptr},
        {"low_confidence_spread_factor", glft_defaults.low_confidence_spread_factor, nullptr},
        {"sticky_threshold", _config.quoting.sticky_threshold, nullptr},
        {"improve_retreat_ratio", _config.quoting.improve_retreat_ratio, nullptr},
        {"protect_ticks", _config.quoting.protect_ticks, nullptr},
        {"max_price_deviation", _config.quoting.max_price_deviation, nullptr},
    };
    static_assert(sizeof(hot_defaults) / sizeof(hot_defaults[0]) == HP_COUNT, "hot_defaults size mismatch");

    for (uint32_t i = 0; i < HP_COUNT; i++) {
        _hot_params[i].name = hot_defaults[i].name;
        _hot_params[i].default_val = hot_defaults[i].default_val;
        _hot_params[i].ptr = ctx->sync_param(hot_defaults[i].name, hot_defaults[i].default_val);
    }

    // 注册参数监控（启用热更新检测）
    ctx->commit_param_watcher();
}

void FutuHotParamManager::applyAll(const Targets& t, const char* strategy_id)
{
    //============================================================
    // 从共享内存读取更新后的参数值，同步到各模块
    //============================================================

    // 报价参数 → SpreadOptimizer
    for (auto& [code, optimizer] : *t.spread_opts) {
        if (!optimizer)
            continue;
        GLFTParams p = optimizer->getParams(); // Copy current params
        p.base_spread = hotVal(HP_BASE_SPREAD);
        p.confidence_weight_min = hotVal(HP_CONFIDENCE_WEIGHT_MIN);
        p.confidence_weight_max = hotVal(HP_CONFIDENCE_WEIGHT_MAX);
        p.phi = hotVal(HP_PHI);
        p.delta_skew_threshold = hotVal(HP_DELTA_SKEW_THRESHOLD);
        p.delta_skew_factor = hotVal(HP_DELTA_SKEW_FACTOR);
        p.max_spread_mult = hotVal(HP_MAX_SPREAD_MULT);
        p.min_spread_mult = hotVal(HP_MIN_SPREAD_MULT);
        p.depth_sensitivity = hotVal(HP_DEPTH_SENSITIVITY);
        p.toxicity_spread_factor = hotVal(HP_TOXICITY_SPREAD_FACTOR);
        p.low_confidence_spread_factor = hotVal(HP_LOW_CONFIDENCE_SPREAD_FACTOR);
        optimizer->updateParams(p); // Thread-safe update (replaces const_cast)
    }

    // 报价数量参数 → FutuQuoter
    t.config->quoting.base_spread = hotVal(HP_BASE_SPREAD);
    t.config->quoting.base_qty = hotVal(HP_BASE_QTY);
    t.config->quoting.level_qty_multiplier = hotVal(HP_LEVEL_QTY_MULTIPLIER);
    t.config->quoting.level_step = hotVal(HP_LEVEL_STEP);

    for (auto& [code, quoter] : *t.quoters) {
        if (!quoter)
            continue;
        quoter->updateQuotingParams(t.config->quoting.base_spread,
                                    t.config->quoting.base_qty,
                                    t.config->quoting.level_qty_multiplier,
                                    t.config->quoting.level_step);
    }

    // Alpha权重 → SignalAggregator
    SignalAggregatorConfig sig_weights;
    sig_weights.ofi_weight = hotVal(HP_OFI_WEIGHT);
    sig_weights.trade_weight = hotVal(HP_TRADE_WEIGHT);
    sig_weights.book_imbalance_weight = hotVal(HP_BOOK_IMBALANCE_WEIGHT);
    sig_weights.momentum_weight = hotVal(HP_MOMENTUM_WEIGHT);
    sig_weights.lead_lag_weight = hotVal(HP_LEAD_LAG_WEIGHT);
    sig_weights.strong_threshold = hotVal(HP_STRONG_THRESHOLD);

    for (auto& [code, aggregator] : *t.aggregators) {
        if (aggregator)
            aggregator->updateWeights(sig_weights);
    }

    // Alpha灵敏度 → Coordinator
    if (t.coordinator) {
        t.coordinator->setAlphaSensitivity(hotVal(HP_ALPHA_SENSITIVITY));
    }

    // Delta软指标 → Portfolio
    if (t.portfolio) {
        PortfolioParams pp = t.portfolio->getParams(); // 拷贝
        pp.portfolio_max_delta = hotVal(HP_MAX_DELTA);
        t.portfolio->setParams(pp); // 通过非const方法写回
    }
    // 同步到 Coordinator (软风控 WIDEN_SPREAD 的 portfolio delta util 基准)
    if (t.coordinator) {
        t.coordinator->setPortfolioMaxDelta(hotVal(HP_MAX_DELTA));
    }

    // 报价粘性/保护参数 → FutuQuoter
    double new_sticky_threshold = hotVal(HP_STICKY_THRESHOLD);
    double new_improve_retreat_ratio = hotVal(HP_IMPROVE_RETREAT_RATIO);
    double new_protect_ticks = hotVal(HP_PROTECT_TICKS);
    double new_max_price_deviation = hotVal(HP_MAX_PRICE_DEVIATION);

    t.config->quoting.sticky_threshold = new_sticky_threshold;
    t.config->quoting.improve_retreat_ratio = new_improve_retreat_ratio;
    t.config->quoting.protect_ticks = new_protect_ticks;
    t.config->quoting.max_price_deviation = new_max_price_deviation;

    for (auto& [code, quoter] : *t.quoters) {
        if (!quoter)
            continue;
        quoter->updateStickyParams(new_sticky_threshold, new_improve_retreat_ratio);
        quoter->updateProtectionParams(t.config->quoting.price_protection, new_protect_ticks);
        quoter->updateMaxPriceDeviation(new_max_price_deviation);
    }

    //==========================================================================
    // 加固(2026-08-24): 交叉复查 (warn 级, 不阻断)。热参单键边界表只防单键越界,
    // 参数间关系(权重和/深度预估/软限vs硬顶/GLFT区间)此前仅在启动期校验一次,
    // 热更路径零复查。applyAll 触发频率极低(watcher diff 变更/on_params_updated),
    // portfolio 快照拷贝开销可接受。
    //==========================================================================
    {
        HotCrossCheckInput cc{};
        cc.ofi_weight = hotVal(HP_OFI_WEIGHT);
        cc.trade_weight = hotVal(HP_TRADE_WEIGHT);
        cc.book_imbalance_weight = hotVal(HP_BOOK_IMBALANCE_WEIGHT);
        cc.momentum_weight = hotVal(HP_MOMENTUM_WEIGHT);
        cc.lead_lag_weight = hotVal(HP_LEAD_LAG_WEIGHT);
        cc.base_qty = hotVal(HP_BASE_QTY);
        cc.level_qty_multiplier = hotVal(HP_LEVEL_QTY_MULTIPLIER);
        cc.num_levels = t.config->quoting.num_levels;
        cc.obligation_level = t.config->quoting.obligation_level;
        cc.scout_qty = t.config->quoting.scout_qty;
        cc.obligation_min_qty = t.config->quoting.obligation_min_qty;
        cc.portfolio_max_delta = hotVal(HP_MAX_DELTA);
        cc.max_spread_mult = hotVal(HP_MAX_SPREAD_MULT);
        cc.min_spread_mult = hotVal(HP_MIN_SPREAD_MULT);
        cc.confidence_weight_min = hotVal(HP_CONFIDENCE_WEIGHT_MIN);
        cc.confidence_weight_max = hotVal(HP_CONFIDENCE_WEIGHT_MAX);

        std::vector<double> max_positions;
        if (t.portfolio) {
            for (const auto& cs : t.portfolio->getAllContractsSnapshot()) {
                if (cs.max_position > 0)
                    max_positions.push_back(cs.max_position);
            }
            cc.contract_max_positions = &max_positions;
        }

        for (const auto& msg : crossCheckIssues(cc))
            WTSLogger::warn("UftFutuMmStrategy[{}] [HOTPARAM-CHECK] {}", strategy_id, msg);
    }
}

} // namespace futu

namespace futu
{

int32_t FutuHotParamManager::syncFromFile(const char* filepath)
{
    std::vector<std::pair<uint32_t, double>> parsed;
    if (!parseHotParamFile(filepath, parsed))
        return -1;

    uint32_t updated = 0;
    for (const auto& kv : parsed)
    {
        double* ptr = _hot_params[kv.first].ptr;
        if (!ptr)
            continue;
        // 值比对: 无变化不写不计数 (mtime 秒粒度导致的重复 sync / 同秒二次修改
        // 天然去重, watcher 每轮全量 parse+diff 也因此零写入)
        if (*ptr != kv.second)
        {
            WTSLogger::info("FutuHotParamManager: hot param '{}' {} -> {}",
                            _hot_params[kv.first].name,
                            *ptr,
                            kv.second);
            *ptr = kv.second;
            updated++;
        }
    }

    // V8-P0-1: 只置脏标志, applyAll 由 on_tick 在 _cb_mtx 内执行
    if (updated > 0)
        _pending_apply.store(true, std::memory_order_release);

    return static_cast<int32_t>(updated);
}

bool FutuHotParamManager::parseHotParamFile(const char* filepath, std::vector<std::pair<uint32_t, double>>& out)
{
    out.clear();
    WTSVariant* cfg = WTSCfgLoader::load_from_file(filepath);
    if (!cfg)
    {
        WTSLogger::error("FutuHotParamManager: failed to load {}", filepath);
        return false;
    }

    // V8-P0-1: 26 参数边界表 -- 越界/NaN 拒收 (此前 "abc"->0.0、负值照单全收)
    // 加固(2026-08-24): 收紧至与 FutuConfigLoader error 级 / FutuConfigValidator 同口径 --
    //   此前 base_spread{0,1000}/base_qty{0,1e5}/level_step{0,1000}/max_delta{0,1e6} 均宽于
    //   loader 拒启范围, protect_ticks/sticky_threshold 允许 0 (静默关软保护/churn 风暴)。
    //   原则: loader error 级 -> 热路径拒收; warn 口径保持宽松, 交给 applyAll 后交叉复查。
    struct Bounds
    {
        double lo;
        double hi;
    };
    static const Bounds bounds[HP_COUNT] = {
        /* base_spread */ {0.5, 20.0},          // validator checkRange [0.5,20] / loader error (0,20]
        /* base_qty */ {1e-6, 100.0},           // loader error (0,100]
        /* level_qty_multiplier */ {0.0, 1000.0}, // 宽松: loader warn [0.1,1.0] 走交叉复查
        /* level_step */ {1e-6, 100.0},         // loader error (0,100]
        /* max_delta */ {1e-6, 1e8},            // loader error (0,1e8]; 0=静默关组合 skew+WIDEN 分母
        /* alpha_sensitivity */ {0.0, 1000.0},
        /* ofi_weight */ {0.0, 10.0},           // 和≈1.0 由交叉复查 warn
        /* trade_weight */ {0.0, 10.0},
        /* book_imbalance_weight */ {0.0, 10.0},
        /* momentum_weight */ {0.0, 10.0},
        /* lead_lag_weight */ {0.0, 10.0},
        /* strong_threshold */ {0.0, 1.0},
        /* confidence_weight_min */ {0.0, 1.0},
        /* confidence_weight_max */ {0.0, 1.0},
        /* phi */ {0.01, 2.0},                  // validator checkRange [0.01,2] (原上界 1.0 过紧)
        /* delta_skew_threshold */ {0.0, 0.9},  // validator checkRange [0,0.9]
        /* delta_skew_factor */ {0.0, 100.0},
        /* max_spread_mult */ {0.0, 100.0},
        /* min_spread_mult */ {0.0, 100.0},
        /* depth_sensitivity */ {0.0, 1000.0},
        /* toxicity_spread_factor */ {0.0, 100.0},
        /* low_confidence_spread_factor */ {0.0, 100.0},
        /* sticky_threshold */ {0.01, 10.0},    // loader warn (0,10]; 0=每 tick 重挂 churn 风暴防呆
        /* improve_retreat_ratio */ {0.0, 1000.0},
        /* protect_ticks */ {0.5, 10000.0},     // <半 tick 无意义; 0=静默关价格软保护(关闭走 price_protection 开关)
        /* max_price_deviation */ {0.0, 1000000.0},
    };
    static_assert(sizeof(bounds) / sizeof(bounds[0]) == HP_COUNT, "bounds table size mismatch");

    const char* const* names = paramNames();
    for (uint32_t i = 0; i < HP_COUNT; i++)
    {
        const char* key = names[i];
        if (!cfg->has(key))
            continue;

        // V8-R5 语义修正: WTSVariant 内部标量统一字符串存储 (yaml_to_variant
        // 对所有标量 as<std::string>, asDouble=atof), "abc"/"true"/空串静默
        // 变 0.0 -- strtod 全串校验拒收非数值内容
        const char* raw = cfg->getCString(key);
        double val = 0;
        bool numeric = false;
        if (raw && *raw)
        {
            char* end = nullptr;
            errno = 0;
            double v = std::strtod(raw, &end);
            if (end != raw && *end == '\0' && errno != ERANGE)
            {
                val = v;
                numeric = true;
            }
        }
        if (!numeric)
        {
            WTSLogger::warn("FutuHotParamManager: '{}' non-numeric value '{}', skipped", key, raw ? raw : "");
            continue;
        }
        const Bounds& b = bounds[i];
        // NaN: 两个比较均为 false -> !true 拒收; inf: 超上界拒收
        if (!(val >= b.lo && val <= b.hi))
        {
            WTSLogger::warn("FutuHotParamManager: '{}'={} out of range [{},{}], skipped",
                            key,
                            val,
                            b.lo,
                            b.hi);
            continue;
        }
        out.emplace_back(i, val);
    }

    cfg->release();
    return true;
}

} // namespace futu

namespace futu
{

//==========================================================================
// 加固 (2026-08-24): 交叉复查 + 启动漂移摘要
//==========================================================================

std::vector<std::string> FutuHotParamManager::crossCheckIssues(const HotCrossCheckInput& in)
{
    std::vector<std::string> issues;
    char buf[256];

    // 1. 五路权重和 (validateSignalWeights 口径: 偏离 1.0 超 ±0.1)
    const double wsum = in.ofi_weight + in.trade_weight + in.book_imbalance_weight +
                        in.momentum_weight + in.lead_lag_weight;
    if (!(wsum >= 0.9 && wsum <= 1.1))
    {
        snprintf(buf, sizeof(buf), "signal weights sum to %.3f, expected ~1.0 (alpha 刻度与一致性口径偏移)", wsum);
        issues.emplace_back(buf);
    }

    // 2. 全侧满挂总深度 (与 FutuConfigLoader 同公式): base_qty/level_qty_multiplier 可热变,
    //    层数结构不变但深度可能跌破义务阈值
    double full_side_depth = 0;
    for (uint32_t l = 0; l < in.num_levels; ++l) {
        if (l < in.obligation_level) {
            full_side_depth += in.scout_qty;
        } else if (l == in.obligation_level) {
            full_side_depth += in.base_qty;
        } else {
            double qty = in.base_qty;
            for (uint32_t k = 0; k < l; ++k)
                qty *= in.level_qty_multiplier;
            if (qty < 1.0)
                qty = 1.0;
            full_side_depth += qty;
        }
    }
    if (full_side_depth + 1e-9 < in.obligation_min_qty) {
        snprintf(buf,
                 sizeof(buf),
                 "full-side depth %.2f < obligationMinQty %.2f: 当前挂单结构可能无法满足交易所总深度义务",
                 full_side_depth,
                 in.obligation_min_qty);
        issues.emplace_back(buf);
    }

    // 3. 组合软限 vs 合约硬顶 (2026-08-19 delta/position 语义边界口径)
    if (in.contract_max_positions) {
        for (size_t i = 0; i < in.contract_max_positions->size(); ++i) {
            const double mp = (*in.contract_max_positions)[i];
            if (mp > 0 && in.portfolio_max_delta > mp) {
                snprintf(buf,
                         sizeof(buf),
                         "portfolio_max_delta(%.0f) > contract maxPosition(%.0f): 策略软限高于风控硬顶, "
                         "调节到达前即被硬停",
                         in.portfolio_max_delta,
                         mp);
                issues.emplace_back(buf);
                break; // 同一问题一次告警即可
            }
        }
    }

    // 4. GLFT spread clamp 区间倒置
    if (in.min_spread_mult > in.max_spread_mult) {
        snprintf(buf,
                 sizeof(buf),
                 "min_spread_mult(%.2f) > max_spread_mult(%.2f): GLFT spread clamp 区间倒置",
                 in.min_spread_mult,
                 in.max_spread_mult);
        issues.emplace_back(buf);
    }

    // 5. 公允价置信度权重插值反向
    if (in.confidence_weight_min > in.confidence_weight_max) {
        snprintf(buf,
                 sizeof(buf),
                 "confidence_weight_min(%.2f) > confidence_weight_max(%.2f): 公允价置信度权重插值反向",
                 in.confidence_weight_min,
                 in.confidence_weight_max);
        issues.emplace_back(buf);
    }

    return issues;
}

int32_t FutuHotParamManager::collectDriftLines(
    const char* filepath, const double* default_vals, std::vector<std::string>& out_lines)
{
    out_lines.clear();
    std::vector<std::pair<uint32_t, double>> parsed;
    if (!parseHotParamFile(filepath, parsed))
        return -1;

    const char* const* names = paramNames();
    uint32_t drifted = 0;
    for (const auto& kv : parsed) {
        if (kv.first >= HP_COUNT)
            continue;
        // 与 syncFromFile 值比对同精度口径: 完全相等视为无漂移
        if (default_vals[kv.first] == kv.second)
            continue;
        char buf[256];
        snprintf(buf, sizeof(buf), "'%s' config_default=%.4f hotparams=%.4f", names[kv.first], default_vals[kv.first], kv.second);
        out_lines.emplace_back(buf);
        drifted++;
    }
    return static_cast<int32_t>(drifted);
}

void FutuHotParamManager::logDriftSummary(const char* filepath, const char* strategy_id) const
{
    double defaults[HP_COUNT];
    for (uint32_t i = 0; i < HP_COUNT; i++)
        defaults[i] = _hot_params[i].default_val;

    std::vector<std::string> lines;
    const int32_t n = collectDriftLines(filepath, defaults, lines);
    if (n < 0) {
        WTSLogger::info("UftFutuMmStrategy[{}] hot params drift check: '{}' not loadable, hot params keep config/coordinator values",
                        strategy_id,
                        filepath);
        return;
    }
    if (n == 0) {
        WTSLogger::info("UftFutuMmStrategy[{}] hot params drift check passed (0/{} drifted from config/coordinator)",
                        strategy_id,
                        static_cast<uint32_t>(HP_COUNT));
        return;
    }
    for (const auto& line : lines)
        WTSLogger::warn("UftFutuMmStrategy[{}] [HOTPARAM-DRIFT] {}", strategy_id, line);
    WTSLogger::warn("UftFutuMmStrategy[{}] {} hot param(s) differ from config/coordinator -- hot file wins on live after first sync "
                    "(回测不加载热参, 差异键=回测/实盘行为分叉点)",
                    strategy_id,
                    n);
}

} // namespace futu
