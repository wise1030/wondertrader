// src/WtFutuCore/SpreadOptimizer.h
#pragma once

#include <string>
#include <cmath>
#include <cstdint>
#include <atomic>
#include "../../Includes/FasterDefs.h"
#include "FutuConfig.h"
#include "ISignalSource.h"

namespace futu
{

/// GLFT model configuration
struct GLFTParams
{
    // Base spread parameters
    double base_spread;       ///< Base spread in ticks (minimum spread)
    double tick_size;         ///< Minimum price increment
    double depth_sensitivity; ///< How order book depth affects spread

    // Inventory skew parameters (unified with delta)
    // A4(2026-08-24②) 角色澄清: phi 名义源自经典 GLFT 的库存厌恶项, 在本实现中
    //   实际充当【波动率加价系数】(computeBaseSpread: spread += phi × percentile/scale × vol_scale);
    //   库存厌恶的真实承担者是 delta_skew_factor / inventory_skew_gain。键名保留兼容。
    double phi; ///< Volatility markup coefficient in base spread (legacy name from GLFT inventory penalty)

    // Delta skew parameters
    double delta_skew_threshold; ///< Portfolio delta skew trigger threshold (utilization, default 0.3)
    double delta_skew_factor;    ///< Portfolio delta skew intensity (default 1.5)
    double portfolio_max_delta;  ///< Portfolio-level Delta soft limit

    // Spread bounds
    double max_spread_mult; ///< Maximum spread multiplier
    double min_spread_mult; ///< Minimum spread multiplier

    // ========== GLFT+Alpha 增强参数 ==========
    // 毒性影响（仅通过 toxicity_spread_factor 影响 spread，不影响 skew）

    // 置信度加权
    double confidence_weight_min; ///< 最小置信度权重 (default 0.2)
    double confidence_weight_max; ///< 最大置信度权重 (default 1.0)

    // Book imbalance - removed from skew, now only in alpha

    // 毒性对 spread 的影响
    double toxicity_spread_factor; ///< 毒性对 spread 的扩大系数 (default 1.0)

    // 低置信度保护
    double low_confidence_spread_factor; ///< 低置信度时 spread 扩大系数 (default 0.8)
    double low_confidence_threshold;     ///< 低置信度阈值 (default 0.3)

    // GLFT 波动率贡献缩放
    double vol_scale; ///< phi*sigma_sq 的缩放因子 (default 5.0)

    // 深度调整参数
    double depth_normalization;     ///< 深度归一化常量 (default 100.0)
    double no_depth_spread_mult;    ///< 无深度数据时 spread 倍数 (default 1.5)
    double depth_sensitivity_scale; ///< depth_sensitivity 缩放因子 (default 0.2)

    double pause_spread_mult_ratio; ///< spread_mult 暂停阈值比例 (default 0.9)
    double delta_skew_power;        ///< delta skew 非线性幂次 (default 1.5)
    double vol_percentile_scale; ///< 波动率百分位归一化分母 (default 50.0)

    // v7.1 连续控制重设计: 归一化库存 skew (delta_util 口径, 2026-08-19 起统一 delta)
    double inventory_skew_gain;  ///< 归一化库存skew增益 (default 1.0; skew_norm=util^power×gain, 1.0=贴mid)
    double skew_cross_max_ticks; ///< delta util≥1.0 时授权减仓侧穿越 mid 的最大 tick 数 (default 3.0)

    // v3 双维 skew 权重（>0 启用加权模式，=0/未设则保留旧 max 模式）
    double portfolio_skew_weight; ///< portfolio delta skew 权重 (default 0.5)
    double contract_skew_weight;  ///< contract delta skew 权重 (default 1.0)

    // B4(2026-08-24②): 毒性加宽最小门槛 (原硬编码 0.05)
    double toxicity_min_score; ///< toxicity_score 低于此值不加宽 spread (default 0.05, 过滤噪声)

    GLFTParams()
        : base_spread(2.0), tick_size(0.2), depth_sensitivity(0.5), phi(0.20), delta_skew_threshold(0.3),
          delta_skew_factor(1.5), portfolio_max_delta(0), max_spread_mult(3.0), min_spread_mult(1.0),
          confidence_weight_min(0.2), confidence_weight_max(1.0), toxicity_spread_factor(1.0),
          low_confidence_spread_factor(2.0) // M8: 统一为 2.0 (低置信度应扩大 spread; 此前构造 0.8 vs yaml 2.0 行为相反)
          ,
          low_confidence_threshold(0.3), vol_scale(5.0), depth_normalization(100.0), no_depth_spread_mult(1.5),
          depth_sensitivity_scale(0.2), pause_spread_mult_ratio(0.9), delta_skew_power(1.5),
          vol_percentile_scale(50.0), inventory_skew_gain(1.0), skew_cross_max_ticks(3.0), portfolio_skew_weight(0.5),
          contract_skew_weight(1.0), toxicity_min_score(0.05)
    {}

    static GLFTParams fromVariant(wtp::WTSVariant* v, double base_spread, double tick_size, double portfolio_max_delta)
    {
        GLFTParams p;
        p.base_spread = base_spread;
        p.tick_size = tick_size;
        p.depth_sensitivity = FutuConfig::readDouble(v, "depthSensitivity", 0.5);
        p.min_spread_mult = FutuConfig::readDouble(v, "minSpreadMult", 1.0);
        p.max_spread_mult = FutuConfig::readDouble(v, "maxSpreadMult", 3.0);
        p.phi = FutuConfig::readDouble(v, "phi", 0.20);
        p.portfolio_max_delta = portfolio_max_delta;
        p.delta_skew_threshold = FutuConfig::readDouble(v, "deltaSkewThreshold", 0.3);
        p.delta_skew_factor = FutuConfig::readDouble(v, "deltaSkewFactor", 1.5);
        p.toxicity_spread_factor = FutuConfig::readDouble(v, "toxicitySpreadFactor", 1.0);
        p.confidence_weight_min = FutuConfig::readDouble(v, "confidenceWeightMin", 0.2);
        p.confidence_weight_max = FutuConfig::readDouble(v, "confidenceWeightMax", 1.0);
        p.low_confidence_spread_factor =
            FutuConfig::readDouble(v, "lowConfidenceSpreadFactor", 2.0); // M8: 与构造默认统一
        p.low_confidence_threshold = FutuConfig::readDouble(v, "lowConfidenceThreshold", 0.3);
        p.vol_scale = FutuConfig::readDouble(v, "volScale", 5.0);
        p.depth_normalization = FutuConfig::readDouble(v, "depthNormalization", 100.0);
        p.no_depth_spread_mult = FutuConfig::readDouble(v, "noDepthSpreadMult", 1.5);
        p.depth_sensitivity_scale = FutuConfig::readDouble(v, "depthSensitivityScale", 0.2);
        p.pause_spread_mult_ratio = FutuConfig::readDouble(v, "pauseSpreadMultRatio", 0.9);
        p.delta_skew_power = FutuConfig::readDouble(v, "deltaSkewPower", 1.5);
        p.vol_percentile_scale = FutuConfig::readDouble(v, "volPercentileScale", 50.0);
        p.inventory_skew_gain = FutuConfig::readDouble(v, "inventorySkewGain", 1.0);
        p.skew_cross_max_ticks = FutuConfig::readDouble(v, "skewCrossMaxTicks", 3.0);
        p.portfolio_skew_weight = FutuConfig::readDouble(v, "portfolioSkewWeight", 0.5);
        p.contract_skew_weight = FutuConfig::readDouble(v, "contractSkewWeight", 1.0);
        p.toxicity_min_score = FutuConfig::readDouble(v, "toxicityMinScore", 0.05);
        return p;
    }
};

/// Result from GLFT spread calculation
struct GLFTResult
{
    double fair_value; ///< ŝ = mid + alpha_adj
    double bid_price;
    double ask_price;
    double base_spread;
    double inventory_skew;
    double alpha_adjustment;
    double spread_mult;
    bool pause_quoting;

    // ========== 分解字段（调试和分析用）==========
    double toxicity_adjustment; ///< 毒性对价差的调整
    double confidence_weight;   ///< 置信度权重
    double glft_vol_contrib;    ///< GLFT 波动率贡献

    GLFTResult()
        : fair_value(0), bid_price(0), ask_price(0), base_spread(0), inventory_skew(0), alpha_adjustment(0),
          spread_mult(1.0), pause_quoting(false), toxicity_adjustment(0), confidence_weight(1.0), glft_vol_contrib(0)
    {}
};

/// Related contract inventory context
struct RelatedInventory
{
    std::string code;
    double inventory;
    double correlation;
    double hedge_ratio;
    double multiplier;
    double last_price;

    RelatedInventory(const std::string& c, double inv, double corr, double hr, double mult, double px)
        : code(c), inventory(inv), correlation(corr), hedge_ratio(hr), multiplier(mult), last_price(px)
    {}
};

/// Portfolio context for multi-contract skew
struct PortfolioContext
{
    double total_delta;
    double total_exposure;
    double current_multiplier;
    double current_hedge_ratio;
    double current_price;
    double contract_max_delta;    ///< 单合约 Delta 软限制（用于归一化 inventory skew）
    double contract_delta_util;   ///< 带符号 delta 利用率 (delta+同向pending×hr)/contract_max_delta, 正=多 负=空
    bool contract_delta_util_valid; ///< contract_delta_util 是否有效 (统一口径 skew 开关)
    // C2(2026-08-24②): 已实现口径 position×hedge_ratio/contract_max_delta —— 消费方: skew/穿越授权。
    //   语义原则: skew 响应已实现库存, 义务/数量响应前瞻库存(含在途)。
    double contract_realized_delta_util;
    bool contract_realized_delta_util_valid;
    std::vector<RelatedInventory> related;

    PortfolioContext()
        : total_delta(0), total_exposure(0), current_multiplier(1), current_hedge_ratio(1), current_price(1),
          contract_max_delta(0), contract_delta_util(0), contract_delta_util_valid(false),
          contract_realized_delta_util(0), contract_realized_delta_util_valid(false)
    {}
    void clear()
    {
        related.clear();
        total_delta = total_exposure = 0;
        contract_max_delta = 0;
        contract_delta_util = 0;
        contract_delta_util_valid = false;
        contract_realized_delta_util = 0;
        contract_realized_delta_util_valid = false;
    }
    void addRelated(const std::string& c, double inv, double corr, double hr, double mult, double px)
    {
        related.emplace_back(c, inv, corr, hr, mult, px);
    }
};

/// GLFT-based Spread Optimizer (Functional Engine)
class SpreadOptimizer
{
public:
    SpreadOptimizer(const std::string& code = "");
    ~SpreadOptimizer() = default;

    void setParams(const GLFTParams& params)
    {
        // F20: seqlock 写协议 (奇=写进行中, 偶=稳定)
        _params_seq.fetch_add(1, std::memory_order_acq_rel);
        _params = params;
        _params_seq.fetch_add(1, std::memory_order_release);
    }

    /// F20: 按值返回一致性快照 (旧 const& 接口在热更新线程并发写时是 data race:
    ///   读侧无锁读 ~200B GLFTParams 可撕裂)。const auto& 调用点靠临时量
    ///   生命周期延长保持兼容。
    GLFTParams getParams() const { return snapshotParams(); }

    /// Thread-safe parameter update (replaces const_cast usage in hot-update)
    void updateParams(const GLFTParams& new_params) { setParams(new_params); }

    /// Thread-safe parameter read (snapshot for tick processing)
    GLFTParams snapshotParams() const
    {
        // F20: seqlock 读协议 — 读到奇数版本或版本变化则重试。
        // 热更新极罕见(GUI 手工), 稳态零重试; 读侧无锁 ~20ns, 替代旧 mutex。
        GLFTParams copy;
        uint64_t s0, s1;
        do {
            s0 = _params_seq.load(std::memory_order_acquire);
            copy = _params;
            s1 = _params_seq.load(std::memory_order_acquire);
        } while (s0 != s1 || (s0 & 1));
        return copy;
    }

    //==========================================================================
    // Core Functional API
    //==========================================================================

    /// 计算最优报价
    /// @param midPrice 中间价
    /// @param contractDelta 当前合约 delta (= position * hedge_ratio, 有正负号)
    ///        [2026-08-19 起不再参与 skew 计算 — 单合约 skew 输入改由
    ///         PortfolioContext.contract_delta_util 注入; 参数保留仅为 API 兼容]
    /// @param ctx 信号上下文（含 alpha, volatility, toxicity, book_imbalance）
    /// @param alphaSensitivity Alpha 敏感度（ticks per alpha unit）
    /// @param pCtx 组合上下文（可选，用于组合级 skew）
    GLFTResult computeOptimalQuote(double midPrice,
                                   double contractDelta,
                                   const SignalContext& ctx,
                                   double alphaSensitivity,
                                   const PortfolioContext* pCtx) const;

    // Internal Logic (exposed for testing/secondary use)
    double computeBaseSpread(const SignalContext& ctx) const;
    /// 归一化库存 skew (tick 单位, delta 口径). skew_norm=delta_util^power×gain, 1.0=贴mid;
    /// delta_util≥1.0 时授权穿越 mid, 上限 1+cross_max/half_spread.
    double computeContractDeltaSkew(double signed_delta_util, double half_spread_ticks, double cross_max_ticks) const;
    double computePortfolioDeltaSkew(double totalDelta, double half_spread_ticks) const; ///< C1: ×half_spread 归一到 ticks

private:
    mutable std::atomic<uint64_t> _params_seq{0}; // F20: seqlock 版本号 (奇=写进行中)
    GLFTParams _params;
    std::string _code;
    mutable double _smoothed_spread_mult = 1.0;
    mutable double _last_output_spread_mult = 0.0; // B-1 fix: 上tick最终输出值，用于变化率限制
    mutable bool _mult_initialized = false;        // A2(2026-08-24②): 首 tick 跳过速率限幅（替代原魔数判定）
};

} // namespace futu
