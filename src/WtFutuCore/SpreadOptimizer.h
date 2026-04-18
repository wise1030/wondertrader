// src/WtFutuCore/SpreadOptimizer.h
#pragma once

#include <string>
#include <cmath>
#include <cstdint>
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"
#include "ISignalSource.h"
#include "SelfTradeCalibrator.h"  // 引入 CalibrationResult

namespace futu {

/// GLFT model configuration
struct GLFTParams
{
    // Base spread parameters
    double      base_spread;        ///< Base spread in ticks (minimum spread)
    double      tick_size;          ///< Minimum price increment
    double      vol_sensitivity;    ///< How volatility affects spread
    double      depth_sensitivity;  ///< How order book depth affects spread
    
    // Inventory skew parameters
    double      phi;                ///< Inventory penalty coefficient (Skew = φ * q)
    double      max_skew;           ///< Maximum skew in ticks
    
    // Portfolio-level inventory skew
    double      portfolio_skew_weight;  ///< Weight for portfolio-level skew (0-1)
    double      min_correlation;        ///< Minimum correlation to include in portfolio skew
    
    // Delta 敏感 skew 参数
    double      delta_skew_threshold;   ///< Delta skew 触发阈值（利用率，默认 0.3）
    double      delta_skew_factor;      ///< Delta skew 强度系数（默认 2.0）
    double      portfolio_max_delta;    ///< 组合级 Delta 软限制
    
    // Spread bounds
    double      max_spread_mult;    ///< Maximum spread multiplier
    double      min_spread_mult;    ///< Minimum spread multiplier
    
    // ========== GLFT+Alpha 增强参数 ==========
    // 毒性影响
    double      toxicity_spread_factor;  ///< 毒性对价差的扩大系数 (default 0.5)
    double      toxicity_skew_factor;    ///< 毒性对 skew 的影响系数 (default 0.3)
    
    // 置信度加权
    double      confidence_weight_min;   ///< 最小置信度权重 (default 0.5)
    double      confidence_weight_max;   ///< 最大置信度权重 (default 1.0)
    
    // Book imbalance 影响
    double      book_imbalance_skew_factor;  ///< 盘口不平衡对 skew 的影响 (default 0.5)
    double      book_imbalance_threshold;    ///< 盘口不平衡触发阈值 (default 0.2)
    
    // 自成交校准影响
    double      calibration_skew_factor;  ///< 自成交校准对 skew 的影响 (default 0.3)
    double      calibration_min_samples;  ///< 最小样本数 (default 5)
    
    GLFTParams()
        : base_spread(2.0), tick_size(0.2), vol_sensitivity(1.0), depth_sensitivity(0.5)
        , phi(2.0), max_skew(5.0), portfolio_skew_weight(0.5), min_correlation(0.5)
        , delta_skew_threshold(0.1), delta_skew_factor(3.0), portfolio_max_delta(0)
        , max_spread_mult(5.0), min_spread_mult(0.5)
        // GLFT+Alpha 增强参数默认值
        , toxicity_spread_factor(0.5)
        , toxicity_skew_factor(0.3)
        , confidence_weight_min(0.5)
        , confidence_weight_max(1.0)
        , book_imbalance_skew_factor(0.5)
        , book_imbalance_threshold(0.2)
        , calibration_skew_factor(0.3)
        , calibration_min_samples(5)
    {}
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
    bool        widen_spread;
    bool        pause_quoting;
    
    // ========== 分解字段（调试和分析用）==========
    double      toxicity_adjustment;    ///< 毒性对价差的调整
    double      confidence_weight;      ///< 置信度权重
    double      book_imbalance_skew;    ///< 盘口不平衡 skew
    double      calibration_skew;       ///< 自成交校准 skew
    double      glft_vol_contrib;       ///< GLFT 波动率贡献
    
    GLFTResult() : fair_value(0), bid_price(0), ask_price(0), base_spread(0)
                 , inventory_skew(0), alpha_adjustment(0), spread_mult(1.0)
                 , widen_spread(false), pause_quoting(false)
                 , toxicity_adjustment(0), confidence_weight(1.0)
                 , book_imbalance_skew(0), calibration_skew(0), glft_vol_contrib(0) {}
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
    SpreadOptimizer();
    ~SpreadOptimizer() = default;
    
    void setParams(const GLFTParams& params) { _params = params; }
    const GLFTParams& getParams() const { return _params; }
    
    void setSkewEnhancement(double sensitivity, double aggressiveThreshold) {
        _skew_sensitivity = sensitivity;
        _aggressive_threshold = aggressiveThreshold;
    }

    //==========================================================================
    // Core Functional API
    //==========================================================================
    
    /// 计算最优报价（完整版，含自成交校准）
    /// @param midPrice 中间价
    /// @param inventory 当前库存
    /// @param ctx 信号上下文（含 alpha, volatility, toxicity, book_imbalance）
    /// @param alphaSensitivity Alpha 敏感度（ticks per alpha unit）
    /// @param pCtx 组合上下文（可选，用于组合级 skew）
    /// @param calib 自成交校准结果（可选，用于毒性校正）
    GLFTResult computeOptimalQuote(
        double midPrice,
        double inventory,
        const SignalContext& ctx,
        double alphaSensitivity,
        const PortfolioContext* pCtx,
        const CalibrationResult* calib
    ) const;
    
    /// 计算最优报价（简化版，无校准）
    GLFTResult computeOptimalQuote(
        double midPrice,
        double inventory,
        const SignalContext& ctx,
        double alphaSensitivity = 0.05,
        const PortfolioContext* pCtx = nullptr
    ) const {
        return computeOptimalQuote(midPrice, inventory, ctx, alphaSensitivity, pCtx, nullptr);
    }

    // Internal Logic (exposed for testing/secondary use)
    double computeBaseSpread(const SignalContext& ctx) const;
    double computeInventorySkew(double inventory, const SignalContext& ctx, double delta = 0, double contract_max_delta = 0) const;
    double computePortfolioSkew(double singleInventory, const PortfolioContext& ctx, VolTier tier) const;
    double computeDeltaAwareSkew(double delta) const;

private:
    GLFTParams _params;
    double _skew_sensitivity;
    double _aggressive_threshold;
};

} // namespace futu
