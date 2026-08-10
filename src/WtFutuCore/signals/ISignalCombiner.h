#pragma once

#include <string>
#include <functional>
#include <memory>
#include <unordered_map>

namespace futu {

//==============================================================================
// B9: ISignalCombiner - pluggable signal fusion model interface
//
// Current "linear" combiner is implemented inline in SignalAggregator::computeAlpha
// (weighted sum + EWMA fallback + confidence). This interface reserves the
// extension point for future non-linear fusion models (e.g. neural, tree-based).
//
// To add a new combiner:
//   1. Implement ISignalCombiner
//   2. Register with SignalCombinerRegistry::instance().registerCombiner("name", ...)
//   3. Set model.type in coordinator.yaml
//   4. SignalAggregator delegates to the combiner (future refactor)
//==============================================================================
class ISignalCombiner
{
public:
    virtual ~ISignalCombiner() = default;
    virtual const char* typeName() const = 0;
    // Future: virtual CombinerOutput combine(const CombinerInput&) = 0;
};

//==============================================================================
// LinearCombiner - default weighted-sum fusion (current computeAlpha logic)
//==============================================================================
class LinearCombiner : public ISignalCombiner
{
public:
    const char* typeName() const override { return "linear"; }
};

//==============================================================================
// SignalCombinerRegistry - string-keyed factory (mirrors SpreadStrategyRegistry)
//==============================================================================
class SignalCombinerRegistry
{
public:
    using Factory = std::function<std::unique_ptr<ISignalCombiner>()>;

    static SignalCombinerRegistry& instance()
    {
        static SignalCombinerRegistry inst;
        return inst;
    }

    void registerCombiner(const std::string& name, Factory factory)
    {
        _factories[name] = std::move(factory);
    }

    std::unique_ptr<ISignalCombiner> create(const std::string& name) const
    {
        auto it = _factories.find(name);
        return (it != _factories.end()) ? it->second() : nullptr;
    }

    bool has(const std::string& name) const { return _factories.find(name) != _factories.end(); }

private:
    SignalCombinerRegistry()
    {
        // Register built-in combiners
        registerCombiner("linear", [] { return std::make_unique<LinearCombiner>(); });
    }
    std::unordered_map<std::string, Factory> _factories;
};

} // namespace futu
