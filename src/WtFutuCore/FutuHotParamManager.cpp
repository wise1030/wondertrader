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

namespace futu {

void FutuHotParamManager::registerParams(wtp::IUftStraCtx* ctx, const FutuMmConfig& _config,
                                          const GLFTParams& glft_defaults,
                                          const SignalAggregatorConfig& sig_defaults,
                                          double alpha_sensitivity)
{
    // 报价基础参数
    HotParamEntry hot_defaults[] = {
        {"base_spread",                _config.quoting.base_spread,           nullptr},
        {"base_qty",                   _config.quoting.base_qty,              nullptr},
        {"level_qty_multiplier",                  _config.quoting.level_qty_multiplier,             nullptr},
        {"level_step",                 _config.quoting.level_step,            nullptr},
        {"max_delta",                  _config.portfolio.max_delta,           nullptr},
        {"alpha_sensitivity",          alpha_sensitivity,                     nullptr},
        {"ofi_weight",                 sig_defaults.ofi_weight,               nullptr},
        {"trade_weight",               sig_defaults.trade_weight,             nullptr},
        {"book_imbalance_weight",      sig_defaults.book_imbalance_weight,    nullptr},
        {"momentum_weight",            sig_defaults.momentum_weight,          nullptr},
        {"lead_lag_weight",            sig_defaults.lead_lag_weight,          nullptr},
        {"strong_threshold",           sig_defaults.strong_threshold,         nullptr},
        {"confidence_weight_min",      glft_defaults.confidence_weight_min,   nullptr},
        {"confidence_weight_max",      glft_defaults.confidence_weight_max,   nullptr},
        {"phi",                        glft_defaults.phi,                     nullptr},
        {"delta_skew_threshold",       glft_defaults.delta_skew_threshold,    nullptr},
        {"delta_skew_factor",          glft_defaults.delta_skew_factor,       nullptr},
        {"max_spread_mult",            glft_defaults.max_spread_mult,         nullptr},
        {"min_spread_mult",            glft_defaults.min_spread_mult,         nullptr},
        {"depth_sensitivity",          glft_defaults.depth_sensitivity,       nullptr},
        {"toxicity_spread_factor",     glft_defaults.toxicity_spread_factor,  nullptr},
        {"low_confidence_spread_factor", glft_defaults.low_confidence_spread_factor, nullptr},
        {"sticky_threshold",           _config.quoting.sticky_threshold,      nullptr},
        {"improve_retreat_ratio",      _config.quoting.improve_retreat_ratio, nullptr},
        {"protect_ticks",              _config.quoting.protect_ticks,         nullptr},
        {"max_price_deviation",        _config.quoting.max_price_deviation,   nullptr},
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
    for (auto& [code, optimizer] : *t.spread_opts)
    {
        if (!optimizer) continue;
        GLFTParams p = optimizer->getParams();  // Copy current params
        p.base_spread           = hotVal(HP_BASE_SPREAD);
        p.confidence_weight_min = hotVal(HP_CONFIDENCE_WEIGHT_MIN);
        p.confidence_weight_max = hotVal(HP_CONFIDENCE_WEIGHT_MAX);
        p.phi                   = hotVal(HP_PHI);
        p.delta_skew_threshold  = hotVal(HP_DELTA_SKEW_THRESHOLD);
        p.delta_skew_factor     = hotVal(HP_DELTA_SKEW_FACTOR);
        p.max_spread_mult       = hotVal(HP_MAX_SPREAD_MULT);
        p.min_spread_mult       = hotVal(HP_MIN_SPREAD_MULT);
        p.depth_sensitivity     = hotVal(HP_DEPTH_SENSITIVITY);
        p.toxicity_spread_factor = hotVal(HP_TOXICITY_SPREAD_FACTOR);
        p.low_confidence_spread_factor = hotVal(HP_LOW_CONFIDENCE_SPREAD_FACTOR);
        optimizer->updateParams(p);  // Thread-safe update (replaces const_cast)
    }

    // 报价数量参数 → FutuQuoter
    t.config->quoting.base_spread = hotVal(HP_BASE_SPREAD);
    t.config->quoting.base_qty    = hotVal(HP_BASE_QTY);
    t.config->quoting.level_qty_multiplier   = hotVal(HP_LEVEL_QTY_MULTIPLIER);
    t.config->quoting.level_step  = hotVal(HP_LEVEL_STEP);

    for (auto& [code, quoter] : *t.quoters)
    {
        if (!quoter) continue;
        quoter->updateQuotingParams(t.config->quoting.base_spread,
                                    t.config->quoting.base_qty,
                                    t.config->quoting.level_qty_multiplier,
                                    t.config->quoting.level_step);
    }

    // Alpha权重 → SignalAggregator
    SignalAggregatorConfig sig_weights;
    sig_weights.ofi_weight              = hotVal(HP_OFI_WEIGHT);
    sig_weights.trade_weight            = hotVal(HP_TRADE_WEIGHT);
    sig_weights.book_imbalance_weight   = hotVal(HP_BOOK_IMBALANCE_WEIGHT);
    sig_weights.momentum_weight         = hotVal(HP_MOMENTUM_WEIGHT);
    sig_weights.lead_lag_weight         = hotVal(HP_LEAD_LAG_WEIGHT);
    sig_weights.strong_threshold        = hotVal(HP_STRONG_THRESHOLD);

    for (auto& [code, aggregator] : *t.aggregators)
    {
        if (aggregator) aggregator->updateWeights(sig_weights);
    }

    // Alpha灵敏度 → Coordinator
    if (t.coordinator)
    {
        t.coordinator->setAlphaSensitivity(hotVal(HP_ALPHA_SENSITIVITY));
    }

    // Delta软指标 → Portfolio
    if (t.portfolio)
    {
        PortfolioParams pp = t.portfolio->getParams();  // 拷贝
        pp.portfolio_max_delta = hotVal(HP_MAX_DELTA);
        t.portfolio->setParams(pp);  // 通过非const方法写回
    }
    // 同步到 Coordinator (checkAndHedge 防震荡阈值)
    if (t.coordinator)
    {
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

    for (auto& [code, quoter] : *t.quoters)
    {
        if (!quoter) continue;
        quoter->updateStickyParams(new_sticky_threshold, new_improve_retreat_ratio);
        quoter->updateProtectionParams(t.config->quoting.price_protection, new_protect_ticks);
        quoter->updateMaxPriceDeviation(new_max_price_deviation);
    }
}

} // namespace futu
