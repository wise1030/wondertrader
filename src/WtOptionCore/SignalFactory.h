/*!
 * \file SignalFactory.h
 * \brief Factory for creating Alpha Signals
 */
#pragma once

#include "IAlphaSignal.h"
#include "ForecastSignal.h"
#include <functional>
#include <map>
#include <string>

namespace wt_option {

class SignalFactory
{
public:
    using Creator = std::function<IAlphaSignalPtr()>;
    
    static void registerSignal(const std::string& name, Creator creator) {
        getRegistry()[name] = creator;
    }
    
    static IAlphaSignalPtr createSignal(const std::string& name) {
        auto& reg = getRegistry();
        auto it = reg.find(name);
        if (it != reg.end()) {
            return it->second();
        }
        return nullptr;
    }
    
    // Helper to register built-ins
    static void initBuiltins() {
        registerSignal("ForecastSignal", []() { return std::make_shared<ForecastSignal>(); });
        registerSignal("Forecast", []() { return std::make_shared<ForecastSignal>(); });
    }
    
private:
    static std::map<std::string, Creator>& getRegistry() {
        static std::map<std::string, Creator> registry;
        return registry;
    }
};

} // namespace wt_option
