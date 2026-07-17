#include "SignalFactory.h"

namespace wt_option {

SignalFactory& SignalFactory::instance() {
    static SignalFactory factory;
    return factory;
}

void SignalFactory::registerAlpha(const std::string& type, AlphaCreator c) {
    m_alphaCreators[type] = std::move(c);
}

void SignalFactory::registerRisk(const std::string& type, RiskCreator c) {
    m_riskCreators[type] = std::move(c);
}

IAlphaSignal::Ptr SignalFactory::createAlpha(const std::string& type) {
    auto it = m_alphaCreators.find(type);
    return (it != m_alphaCreators.end()) ? it->second() : nullptr;
}

IRiskSignal::Ptr SignalFactory::createRisk(const std::string& type) {
    auto it = m_riskCreators.find(type);
    return (it != m_riskCreators.end()) ? it->second() : nullptr;
}

} // namespace wt_option
