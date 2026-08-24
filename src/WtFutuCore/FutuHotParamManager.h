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
    HP_CONTRACT_MAX_DELTA, ///< B2(2026-08-24②): 单合约 delta 软限(应用于全部合约; 差异化配置需重启)
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
    /// @param contract_max_delta B2: 单合约 delta 软限默认值 (anchor 合约配置值)
    void registerParams(wtp::IUftStraCtx* ctx,
                        const FutuMmConfig& config,
                        const GLFTParams& glft_defaults,
                        const SignalAggregatorConfig& sig_defaults,
                        double alpha_sensitivity,
                        double contract_max_delta);

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

    //==========================================================================
    // 加固 (2026-08-24): 边界对齐后的交叉复查与启动漂移摘要
    //==========================================================================

    /// applyAll 后交叉复查的纯数值输入 (与模块解耦, 便于单测)
    struct HotCrossCheckInput
    {
        // 五路信号权重
        double ofi_weight;
        double trade_weight;
        double book_imbalance_weight;
        double momentum_weight;
        double lead_lag_weight;
        // 挂单结构 (base_qty/level_qty_multiplier 可热变; 其余为结构性配置不变)
        double base_qty;
        double level_qty_multiplier;
        uint32_t num_levels;
        uint32_t obligation_level;
        double scout_qty;
        double obligation_min_qty;
        // 组合软限 vs 合约硬顶 (语义边界复查)
        double portfolio_max_delta;
        double contract_max_delta; ///< B2: 单合约 delta 软限 (热参数)
        const std::vector<double>* contract_max_positions; ///< 各合约 maxPosition (>0 项), 可为 null
        // GLFT 区间一致性
        double max_spread_mult;
        double min_spread_mult;
        double confidence_weight_min;
        double confidence_weight_max;
    };

    /// 交叉复查 (warn 级, 不阻断): 返回问题描述列表, 空列表=通过。
    /// 口径与 FutuConfigLoader / FutuConfigValidator 启动期校验一致。
    static std::vector<std::string> crossCheckIssues(const HotCrossCheckInput& in);

    /// 对比 hotparams 文件与注册默认值(config/coordinator), 返回差异行 (供单测)。
    /// @return -1=文件不可加载; >=0=差异键数 (out_lines 每差异键一行)
    static int32_t collectDriftLines(
        const char* filepath, const double* default_vals, std::vector<std::string>& out_lines);

    /// 启动期漂移摘要: 打印 hotparams.yaml 与 config/coordinator 同名键的差异。
    /// 回测同样调用 —— watcher 不跑、热参不生效, 差异键即回测/实盘行为分叉点。
    void logDriftSummary(const char* filepath, const char* strategy_id) const;

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
            "contract_max_delta",
        };
        return names;
    }

protected:
    HotParamEntry _hot_params[HP_COUNT];
    std::atomic<bool> _pending_apply{false}; ///< V8-P0-1: watcher 置位, tick 线程 drain
};

} // namespace futu
