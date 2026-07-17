#pragma once

#include "ISignal.h"
#include <functional>
#include <map>

namespace wt_option {

class SignalFactory {
public:
    using AlphaCreator = std::function<IAlphaSignal::Ptr()>;
    using RiskCreator  = std::function<IRiskSignal::Ptr()>;

    static SignalFactory& instance();

    void registerAlpha(const std::string& type, AlphaCreator c);
    void registerRisk(const std::string& type, RiskCreator c);

    IAlphaSignal::Ptr createAlpha(const std::string& type);
    IRiskSignal::Ptr  createRisk(const std::string& type);

private:
    std::map<std::string, AlphaCreator> m_alphaCreators;
    std::map<std::string, RiskCreator>  m_riskCreators;
};

#define REGISTER_ALPHA_SIGNAL(type, classname) \
    static bool _reg_alpha_##classname = []() { \
        SignalFactory::instance().registerAlpha(type, \
            []() { return std::make_shared<classname>(); }); \
        return true; \
    }();

#define REGISTER_RISK_SIGNAL(type, classname) \
    static bool _reg_risk_##classname = []() { \
        SignalFactory::instance().registerRisk(type, \
            []() { return std::make_shared<classname>(); }); \
        return true; \
    }();

} // namespace wt_option
