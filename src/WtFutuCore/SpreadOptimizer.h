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
    //   库存厌恶自 R14 起由整档量化 skew(skew_tick_max 等, 见下方 Delta skew 注释)承担。键名保留兼容。
    double phi; ///< Volatility markup coefficient in base spread (legacy name from GLFT inventory penalty)

    // Delta skew parameters (R14-2026-09-07: tick 量化驱动)
    //   旧 delta_skew_threshold/factor 与 inventory_skew_gain 已删除——三者与 power 在调同一条
    //   平滑曲线（过参数化且 sub-tick 死区间, 09-07 夜盘实证: util=1.0 撞限总 skew 仅 0.67t 无效）。
    //   新语义: driver = w_p·sign(r_p)·|r_p|^γ + w_c·sign(r_c)·|r_c|^γ, skew_ticks=round(k_max×driver)+滞环
    double portfolio_max_delta;  ///< Portfolio-level Delta soft limit

    // Spread bounds
    double max_spread_mult; ///< Maximum spread multiplier
    double min_spread_mult; ///< Minimum spread multiplier

    // ========== GLFT+Alpha 增强参数 ==========
    // V10-R10: 毒性加宽链路已删除 — 毒性响应统一为"检测即双边暂停"
    // (StrategyCoordinator TOXICITY 相位), 优化器不再消费毒性信号

    // 置信度加权
    double confidence_weight_min; ///< 最小置信度权重 (default 0.2)
    double confidence_weight_max; ///< 最大置信度权重 (default 1.0)

    // Book imbalance - removed from skew, now only in alpha

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
    double delta_skew_power;        ///< R14 γ: driver 幂次 (default 1.0; 线性+取整=隐式分档, 可配置)
    double vol_percentile_scale; ///< 波动率百分位归一化分母 (default 50.0)

    // R14(2026-09-07) tick 量化 skew 档位参数
    double skew_tick_max;    ///< k_max: skew 最大档位数 (ticks, default 4.0)
    double skew_hysteresis;  ///< 滞环宽度 (raw ticks 单位, 档边界 ±h 内不切换, default 0.05)
    double skew_cross_max_ticks; ///< delta util≥1.0 时授权减仓侧穿越 mid 的最大 tick 数 (default 3.0)

    // v3 双维 skew 权重（>0 启用加权模式，=0/未设则保留旧 max 模式）
    double portfolio_skew_weight; ///< portfolio delta skew 权重 (default 0.5)
    double contract_skew_weight;  ///< contract delta skew 权重 (default 1.0)

    GLFTParams()
        : base_spread(2.0), tick_size(0.2), depth_sensitivity(0.5), phi(0.20), portfolio_max_delta(0),
          max_spread_mult(3.0), min_spread_mult(1.0),
          confidence_weight_min(0.2), confidence_weight_max(1.0),
          low_confidence_spread_factor(2.0) // M8: 统一为 2.0 (低置信度应扩大 spread; 此前构造 0.8 vs yaml 2.0 行为相反)
          ,
          low_confidence_threshold(0.3), vol_scale(5.0), depth_normalization(100.0), no_depth_spread_mult(1.5),
          depth_sensitivity_scale(0.2), pause_spread_mult_ratio(0.9), delta_skew_power(1.0),
          vol_percentile_scale(50.0), skew_tick_max(4.0), skew_hysteresis(0.05), skew_cross_max_ticks(3.0),
          portfolio_skew_weight(0.5),
          contract_skew_weight(1.0)
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
        p.delta_skew_power = FutuConfig::readDouble(v, "deltaSkewPower", 1.0); // R14: γ 默认 1.0
        p.vol_percentile_scale = FutuConfig::readDouble(v, "volPercentileScale", 50.0);
        p.skew_tick_max = FutuConfig::readDouble(v, "skewTickMax", 4.0);      // R14
        p.skew_hysteresis = FutuConfig::readDouble(v, "skewHysteresis", 0.05); // R14
        p.skew_cross_max_ticks = FutuConfig::readDouble(v, "skewCrossMaxTicks", 3.0);
        p.portfolio_skew_weight = FutuConfig::readDouble(v, "portfolioSkewWeight", 0.5);
        p.contract_skew_weight = FutuConfig::readDouble(v, "contractSkewWeight", 1.0);
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
    double confidence_weight;   ///< 置信度权重
    double glft_vol_contrib;    ///< GLFT 波动率贡献

    GLFTResult()
        : fair_value(0), bid_price(0), ask_price(0), base_spread(0), inventory_skew(0), alpha_adjustment(0),
          spread_mult(1.0), pause_quoting(false), confidence_weight(1.0), glft_vol_contrib(0)
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
    // F-core S1(2026-09-15): skew driver 纯化口径 —— hedge_ratio 逐 tick 价格重算
    //   (CorrelationManager (p1*m1*beta)/(p2*m2)) 使 delta() 随行情抖动, 翻转-成交
    //   相关仅 0-9%(2026-09-15 实测)。skew 驱动改用:
    //   contract_position_util = position手数 / contract_max_delta  (纯手数, 仅成交驱动)
    //   total_delta_skew       = Σ position_c × hr_slow_c           (hr 5min EMA, 慢变)
    double contract_position_util;
    bool contract_position_util_valid;
    double total_delta_skew;
    std::vector<RelatedInventory> related;

    PortfolioContext()
        : total_delta(0), total_exposure(0), current_multiplier(1), current_hedge_ratio(1), current_price(1),
          contract_max_delta(0), contract_delta_util(0), contract_delta_util_valid(false),
          contract_realized_delta_util(0), contract_realized_delta_util_valid(false),
          contract_position_util(0), contract_position_util_valid(false), total_delta_skew(0)
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
        contract_position_util = 0;
        contract_position_util_valid = false;
        total_delta_skew = 0;
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
    /// @param ctx 信号上下文（含 alpha, volatility, book_imbalance; V10-R10 起不再含毒性）
    /// @param alphaSensitivity Alpha 敏感度（ticks per alpha unit）
    /// @param pCtx 组合上下文（可选，用于组合级 skew）
    GLFTResult computeOptimalQuote(double midPrice,
                                   double contractDelta,
                                   const SignalContext& ctx,
                                   double alphaSensitivity,
                                   const PortfolioContext* pCtx) const;

    // Internal Logic (exposed for testing/secondary use)
    double computeBaseSpread(const SignalContext& ctx) const;
    // R14(2026-09-07): tick 量化 skew —— 加权符号驱动(纯函数, 无纲) + 滞环整档量化
    /// driver = w_p·sign(r_p)·|r_p|^γ + w_c·sign(r_c)·|r_c|^γ  (对冲腿符号相抵语义保留)
    double computeDeltaSkewDriver(double portfolio_delta_signed, double contract_util_signed) const;
    /// raw ticks → 滞环量化档位 k (clamp ±round(skew_tick_max)); 档边界 ±skew_hysteresis 内不切换
    int quantizeSkewTicks(double raw_ticks) const;

private:
    mutable std::atomic<uint64_t> _params_seq{0}; // F20: seqlock 版本号 (奇=写进行中)
    GLFTParams _params;
    std::string _code;
    mutable double _smoothed_spread_mult = 1.0;
    mutable double _last_output_spread_mult = 0.0; // B-1 fix: 上tick最终输出值，用于变化率限制
    mutable bool _mult_initialized = false;        // A2(2026-08-24②): 首 tick 跳过速率限幅（替代原魔数判定）
    mutable int _last_skew_k = 0;                  // R14: skew 滞环当前档位 (Md 线程单写单读)
};

} // namespace futu
