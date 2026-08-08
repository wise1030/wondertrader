/*!
 * \file ISpreadStrategy.h
 * \brief 价差套利策略插件接口
 *
 * 设计目的:
 *   统一 4 个套利策略(MeanReversion/TrendFollowing/PairsTrading/StatisticalArb)
 *   的调用接口, 消除 SpreadArbitrageManager 中的 else-if 链。
 *   新增策略只需: 继承本接口 + 在 StrategyRegistry 注册一行。
 *
 * 线程契约:
 *   update/generateSignal 由套利执行线程(或同步模式下的主线程)调用,
 *   reset 由主线程 session 管理调用。实现类内部状态不需跨线程保护,
 *   调用方(SpreadArbitrageManager)负责外部串行化。
 */
#pragma once

#include "SpreadArbitrageTypes.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>

namespace futu
{

//==============================================================================
// Spread Strategy Plugin Interface
//==============================================================================

class ISpreadStrategy
{
public:
    virtual ~ISpreadStrategy() = default;

    /// 基于当前价差状态生成交易信号
    virtual SpreadSignal generateSignal(const SpreadState& state, uint64_t current_time) = 0;

    /// 每 tick 数据更新 (无内部状态的策略可实现为空操作)
    virtual void update(const SpreadState& state, uint64_t timestamp) = 0;

    /// 从 pair 配置提取本策略所需参数 (替代原先 Manager 中的类型分支)
    virtual void configure(const SpreadPairConfig& cfg) = 0;

    /// 重置内部状态 (session 切换)
    virtual void reset() = 0;

    /// 策略类型名 (与 yaml 中 primaryStrategy 字符串一致)
    virtual const char* typeName() const = 0;
};

//==============================================================================
// Strategy Registry (工厂注册表)
//
// 用法:
//   1. 内置策略在 SpreadArbitrageManager.cpp 中静态注册
//   2. 新增策略: 实现 ISpreadStrategy 后调用
//      SpreadStrategyRegistry::registerStrategy("my_strategy",
//          [] { return std::make_unique<MyStrategy>(); });
//==============================================================================

class SpreadStrategyRegistry
{
public:
    using Factory = std::function<std::unique_ptr<ISpreadStrategy>()>;

    static SpreadStrategyRegistry& instance()
    {
        static SpreadStrategyRegistry inst;
        return inst;
    }

    void registerStrategy(const std::string& name, Factory factory) { _factories[name] = std::move(factory); }

    std::unique_ptr<ISpreadStrategy> create(const std::string& name) const
    {
        auto it = _factories.find(name);
        return (it != _factories.end()) ? it->second() : nullptr;
    }

    bool has(const std::string& name) const { return _factories.find(name) != _factories.end(); }

private:
    SpreadStrategyRegistry() = default;
    std::unordered_map<std::string, Factory> _factories;
};

} // namespace futu
