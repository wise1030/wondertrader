// src/WtFutuCore/SpreadOptimizer.h
#pragma once

#include <string>
#include <cmath>
#include <cstdint>
#include <mutex>
#include "../Includes/FasterDefs.h"
#include "FutuConfig.h"
#include "ISignalSource.h"

namespace futu {

/// GLFT model configuration
struct GLFTParams
{
    // Base spread parameters
    double      base_spread;        ///< Base spread in ticks (minimum spread)
    double      tick_size;          ///< Minimum price increment
    double      depth_sensitivity;  ///< How order book depth affects spread
    
    // Inventory skew parameters (unified with delta)
    double      phi;                ///< Inventory penalty coefficient (used in base spread vol contribution)
    
    // Delta skew parameters
    double      delta_skew_threshold;   ///< Portfolio delta skew trigger threshold (utilization, default 0.3)
    double      delta_skew_factor;      ///< Portfolio delta skew intensity (default 1.5)
    double      portfolio_max_delta;    ///< Portfolio-level Delta soft limit
    
    // Spread bounds
    double      max_spread_mult;    ///< Maximum spread multiplier
    double      min_spread_mult;    ///< Minimum spread multiplier
    
    // ========== GLFT+Alpha 增强参数 ==========
    // 毒性影响（仅通过 toxicity_spread_factor 影响 spread，不影响 skew）
    
    // 置信度加权
    double      confidence_weight_min;   ///< 最小置信度权重 (default 0.2)
    double      confidence_weight_max;   ///< 最大置信度权重 (default 1.0)
    
    // Book imbalance - removed from skew, now only in alpha
    
    // 毒性对 spread 的影响
    double      toxicity_spread_factor;   ///< 毒性对 spread 的扩大系数 (default 1.0)
    
    // 低置信度保护
    double      low_confidence_spread_factor; ///< 低置信度时 spread 扩大系数 (default 0.8)
    double      low_confidence_threshold;     ///< 低置信度阈值 (default 0.3)
    
    // GLFT 波动率贡献缩放
    double      vol_scale;                    ///< phi*sigma_sq 的缩放因子 (default 5.0)
    
    // 深度调整参数
    double      depth_normalization;          ///< 深度归一化常量 (default 100.0)
    double      no_depth_spread_mult;         ///< 无深度数据时 spread 倍数 (default 1.5)
    double      depth_sensitivity_scale;     ///< depth_sensitivity 缩放因子 (default 0.2)
    
    double      pause_spread_mult_ratio;    ///< spread_mult 暂停阈值比例 (default 0.9)
    double      delta_skew_power;           ///< delta skew 非线性幂次 (default 1.5)
    double      inventory_skew_scale;      ///< 库存skew放大系数 (default 2.0, 使中持仓时ask接近贴mid)
    double      vol_percentile_scale;       ///< 波动率百分位归一化分母 (default 50.0)
    
    GLFTParams()
        : base_spread(2.0), tick_size(0.2), depth_sensitivity(0.5)
        , phi(0.20)
        , delta_skew_threshold(0.3), delta_skew_factor(1.5), portfolio_max_delta(0)
        , max_spread_mult(3.0), min_spread_mult(1.0)
        , confidence_weight_min(0.2)
        , confidence_weight_max(1.0)
        , toxicity_spread_factor(1.0)
        , low_confidence_spread_factor(0.8)
        , low_confidence_threshold(0.3)
        , vol_scale(5.0)
        , depth_normalization(100.0)
        , no_depth_spread_mult(1.5)
        , depth_sensitivity_scale(0.2)
        , pause_spread_mult_ratio(0.9)
        , delta_skew_power(1.5)
        , inventory_skew_scale(2.0)
        , vol_percentile_scale(50.0)
    {}
    
    static GLFTParams fromVariant(wtp::WTSVariant* v, double base_spread, double tick_size, double portfolio_max_delta) {
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
        p.low_confidence_spread_factor = FutuConfig::readDouble(v, "lowConfidenceSpreadFactor", 0.8);
        p.low_confidence_threshold = FutuConfig::readDouble(v, "lowConfidenceThreshold", 0.3);
        p.vol_scale = FutuConfig::readDouble(v, "volScale", 5.0);
        p.depth_normalization = FutuConfig::readDouble(v, "depthNormalization", 100.0);
        p.no_depth_spread_mult = FutuConfig::readDouble(v, "noDepthSpreadMult", 1.5);
        p.depth_sensitivity_scale = FutuConfig::readDouble(v, "depthSensitivityScale", 0.2);
        p.pause_spread_mult_ratio = FutuConfig::readDouble(v, "pauseSpreadMultRatio", 0.9);
        p.delta_skew_power = FutuConfig::readDouble(v, "deltaSkewPower", 1.5);
        p.inventory_skew_scale = FutuConfig::readDouble(v, "inventorySkewScale", 2.0);
        p.vol_percentile_scale = FutuConfig::readDouble(v, "volPercentileScale", 50.0);
        return p;
    }
};

/// Result from GLFT spread calculation
struct GLFTResult
{
    double      fair_value;         ///< ŝ = mid + alpha_adj
    double      bid_price;
    double      ask_price;
    double      base_spread;
    double      inventory_skew;
    double      alpha_adjustment;
    double      spread_mult;
    bool        pause_quoting;
    
    // ========== 分解字段（调试和分析用）==========
    double      toxicity_adjustment;    ///< 毒性对价差的调整
    double      confidence_weight;      ///< 置信度权重
    double      glft_vol_contrib;       ///< GLFT 波动率贡献
    
    GLFTResult() : fair_value(0), bid_price(0), ask_price(0), base_spread(0)
                 , inventory_skew(0), alpha_adjustment(0), spread_mult(1.0)
                 , pause_quoting(false)
, toxicity_adjustment(0), confidence_weight(1.0)
                  , glft_vol_contrib(0) {}
};

/// Related contract inventory context
struct RelatedInventory
{
    std::string code;
    double      inventory;
    double      correlation;
    double      hedge_ratio;
    double      multiplier;
    double      last_price;
    
    RelatedInventory(const std::string& c, double inv, double corr, double hr, double mult, double px)
        : code(c), inventory(inv), correlation(corr), hedge_ratio(hr), multiplier(mult), last_price(px) {}
};

/// Portfolio context for multi-contract skew
struct PortfolioContext
{
    double      total_delta;
    double      total_exposure;
    double      current_multiplier;
    double      current_hedge_ratio;
    double      current_price;
    double      contract_max_delta;  ///< 单合约 Delta 软限制（用于归一化 inventory skew）
    std::vector<RelatedInventory> related;
    
    PortfolioContext() : total_delta(0), total_exposure(0), current_multiplier(1), current_hedge_ratio(1), current_price(1), contract_max_delta(0) {}
    void clear() { related.clear(); total_delta = total_exposure = 0; contract_max_delta = 0; }
    void addRelated(const std::string& c, double inv, double corr, double hr, double mult, double px) {
        related.emplace_back(c, inv, corr, hr, mult, px);
    }
};

/// GLFT-based Spread Optimizer (Functional Engine)
class SpreadOptimizer
{
public:
    SpreadOptimizer(const std::string& code = "");
    ~SpreadOptimizer() = default;
    
    void setParams(const GLFTParams& params) { _params = params; }
    const GLFTParams& getParams() const { return _params; }
    
    /// Thread-safe parameter update (replaces const_cast usage in hot-update)
    void updateParams(const GLFTParams& new_params)
    {
        std::lock_guard<std::mutex> guard(_params_mutex);
        _params = new_params;
    }
    
    /// Thread-safe parameter read (snapshot for tick processing)
    GLFTParams snapshotParams() const
    {
        std::lock_guard<std::mutex> guard(_params_mutex);
        return _params;
    }

    //==========================================================================
    // Core Functional API
    //==========================================================================
    
    /// 计算最优报价
    /// @param midPrice 中间价
    /// @param contractDelta 当前合约 delta (= position * hedge_ratio, 有正负号)
    /// @param ctx 信号上下文（含 alpha, volatility, toxicity, book_imbalance）
    /// @param alphaSensitivity Alpha 敏感度（ticks per alpha unit）
    /// @param pCtx 组合上下文（可选，用于组合级 skew）
    GLFTResult computeOptimalQuote(
        double midPrice,
        double contractDelta,
        const SignalContext& ctx,
        double alphaSensitivity,
        const PortfolioContext* pCtx
    ) const;
    
    // Internal Logic (exposed for testing/secondary use)
    double computeBaseSpread(const SignalContext& ctx) const;
    double computeContractDeltaSkew(double contractDelta, double contractMaxDelta) const;
    double computePortfolioDeltaSkew(double totalDelta) const;

private:
    mutable std::mutex _params_mutex;  // Protects _params for hot-update
    GLFTParams _params;
    std::string _code;
    mutable double _smoothed_spread_mult = 1.0;
    mutable double _last_output_spread_mult = 0.0;  // B-1 fix: 上tick最终输出值，用于变化率限制
};

} // namespace futu
