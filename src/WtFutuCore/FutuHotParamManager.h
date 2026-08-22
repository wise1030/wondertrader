/*!
 * \file FutuHotParamManager.h
 * \brief 热更新参数管理器 (从 UftFutuMmStrategy 拆分的热参数职责)
 *
 * 设计目的:
 *   集中管理 26 个热更新参数的注册(共享内存同步)与应用分发。
 *   策略类只持有一个实例, on_params_updated 委托给 applyAll。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <memory>
#include <unordered_map>
#include "../Includes/FasterDefs.h"

// 前向声明, 减少头文件依赖
namespace wtp
{
class IUftStraCtx;
}

namespace futu
{

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
enum HotParamIndex : uint32_t
{
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

struct HotParamEntry
{
    const char* name;
    double default_val;
    double* ptr;
};

class FutuHotParamManager
{
public:
    FutuHotParamManager() : _hot_params{} {}

    /// 应用热更新所需的模块引用集合 (由策略类在 on_params_updated 时组装)
    struct Targets
    {
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
    void registerParams(wtp::IUftStraCtx* ctx,
                        const FutuMmConfig& config,
                        const GLFTParams& glft_defaults,
                        const SignalAggregatorConfig& sig_defaults,
                        double alpha_sensitivity);

    /// 应用全部热参数到各模块 (on_params_updated 委托)
    void applyAll(const Targets& t, const char* strategy_id);

    /// V8-P0-1: 从文件同步热参数值到共享内存 -- **只写共享内存 + 置 pending 标志**,
    /// 不再直接 applyAll (watcher 线程裸写主链路状态是 P0 数据竞争);
    /// 应用由 UftFutuMmStrategy::on_tick 在 _cb_mtx 内 drain (on_params_updated)
    /// @return -1=文件解析失败; >=0=实际发生值变更的参数个数 (值比对去重)
    int32_t syncFromFile(const char* filepath);

    /// V8-P0-1: tick 线程消费 pending 标志 (true=有变更待应用)
    bool consumePendingApply() { return _pending_apply.exchange(false, std::memory_order_acq_rel); }

    /// V8-P0-1: 解析 + 校验 hotparams 文件 (纯函数, 供单测)
    /// 越界/NaN/非数值类型 -> 跳过该键 (warn); 值合法但与共享内存相同 -> 由
    /// syncFromFile 值比对去重
    /// @return false=文件解析失败; true=out 收录全部合法 (idx,value) (可为空)
    static bool parseHotParamFile(const char* filepath, std::vector<std::pair<uint32_t, double>>& out);

    double hotVal(HotParamIndex idx) const
    {
        return _hot_params[idx].ptr ? *_hot_params[idx].ptr : _hot_params[idx].default_val;
    }
    bool isHotChanged(HotParamIndex idx) const { return _hot_params[idx].ptr != nullptr; }

    /// 获取已注册热参数名列表 (供 FutuHotParamWatcher 同步文件用)
    static const char* const* paramNames()
    {
        static const char* names[HP_COUNT] = {
            "base_spread",
            "base_qty",
            "level_qty_multiplier",
            "level_step",
            "max_delta",
            "alpha_sensitivity",
            "ofi_weight",
            "trade_weight",
            "book_imbalance_weight",
            "momentum_weight",
            "lead_lag_weight",
            "strong_threshold",
            "confidence_weight_min",
            "confidence_weight_max",
            "phi",
            "delta_skew_threshold",
            "delta_skew_factor",
            "max_spread_mult",
            "min_spread_mult",
            "depth_sensitivity",
            "toxicity_spread_factor",
            "low_confidence_spread_factor",
            "sticky_threshold",
            "improve_retreat_ratio",
            "protect_ticks",
            "max_price_deviation",
        };
        return names;
    }

protected:
    HotParamEntry _hot_params[HP_COUNT];
    std::atomic<bool> _pending_apply{false}; ///< V8-P0-1: watcher 置位, tick 线程 drain
};

} // namespace futu
