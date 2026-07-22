/*!
 * \file FutuHotParamManager.h
 * \brief 热更新参数管理器 (从 UftFutuMmStrategy 拆分的热参数职责)
 *
 * 设计目的:
 *   集中管理 26 个热更新参数的注册(共享内存同步)与应用分发。
 *   策略类只持有一个实例, on_params_updated 委托给 applyAll。
 */
#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <unordered_map>
#include "../Includes/FasterDefs.h"

// 前向声明, 减少头文件依赖
namespace wtp { class IUftStraCtx; }

namespace futu {

struct FutuMmConfig;
class FutuQuoter;
class SpreadOptimizer;
class SignalAggregator;
class StrategyCoordinator;
class FutuPortfolio;
struct GLFTParams;
struct SignalAggregatorConfig;

//==========================================================================
// 热参数索引 (与原 UftFutuMmStrategy::HotParamIndex 一致)
//==========================================================================
enum HotParamIndex : uint32_t {
    HP_BASE_SPREAD = 0,
    HP_BASE_QTY,
    HP_LEVEL_QTY_MULTIPLIER,
    HP_LEVEL_STEP,
    HP_MAX_DELTA,
    HP_ALPHA_SENSITIVITY,
    HP_OFI_WEIGHT,
    HP_TRADE_WEIGHT,
    HP_BOOK_IMBALANCE_WEIGHT,
    HP_MOMENTUM_WEIGHT,
    HP_LEAD_LAG_WEIGHT,
    HP_STRONG_THRESHOLD,
    HP_CONFIDENCE_WEIGHT_MIN,
    HP_CONFIDENCE_WEIGHT_MAX,
    HP_PHI,
    HP_DELTA_SKEW_THRESHOLD,
    HP_DELTA_SKEW_FACTOR,
    HP_MAX_SPREAD_MULT,
    HP_MIN_SPREAD_MULT,
    HP_DEPTH_SENSITIVITY,
    HP_TOXICITY_SPREAD_FACTOR,
    HP_LOW_CONFIDENCE_SPREAD_FACTOR,
    HP_STICKY_THRESHOLD,
    HP_IMPROVE_RETREAT_RATIO,
    HP_PROTECT_TICKS,
    HP_MAX_PRICE_DEVIATION,
    HP_COUNT
};

struct HotParamEntry {
    const char* name;
    double default_val;
    double* ptr;
};

class FutuHotParamManager
{
public:
    FutuHotParamManager() : _hot_params{} {}

    /// 应用热更新所需的模块引用集合 (由策略类在 on_params_updated 时组装)
    struct Targets {
        FutuMmConfig* config;
        wtp::wt_hashmap<std::string, std::unique_ptr<FutuQuoter>>* quoters;
        wtp::wt_hashmap<std::string, std::unique_ptr<SpreadOptimizer>>* spread_opts;
        std::unordered_map<std::string, std::unique_ptr<SignalAggregator>>* aggregators;
        StrategyCoordinator* coordinator;
        FutuPortfolio* portfolio;
    };

    /// 注册热参数到共享内存 (init 阶段调用一次)
    /// @param glft_defaults 从首个 SpreadOptimizer 读取的默认 GLFT 参数
    /// @param sig_defaults  从首个 SignalAggregator 读取的默认权重
    /// @param alpha_sensitivity coordinator 模块的 alpha 灵敏度
    void registerParams(wtp::IUftStraCtx* ctx, const FutuMmConfig& config,
                        const GLFTParams& glft_defaults,
                        const SignalAggregatorConfig& sig_defaults,
                        double alpha_sensitivity);

    /// 应用全部热参数到各模块 (on_params_updated 委托)
    void applyAll(const Targets& t, const char* strategy_id);

    double hotVal(HotParamIndex idx) const {
        return _hot_params[idx].ptr ? *_hot_params[idx].ptr : _hot_params[idx].default_val;
    }
    bool isHotChanged(HotParamIndex idx) const { return _hot_params[idx].ptr != nullptr; }

private:
    HotParamEntry _hot_params[HP_COUNT];
};

} // namespace futu
