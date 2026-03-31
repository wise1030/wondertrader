/*!
 * \file IScanModule.cpp
 * \brief Scanner module base implementation
 */

#include "IScanModule.h"
#include <algorithm>
#include <chrono>
#include <iostream>

namespace wt_option {

//=============================================================================
// IScanModule implementation
//=============================================================================

IScanModule::IScanModule(const std::string& name)
    : m_name(name)
    , m_enabled(false)
{
}

void IScanModule::setEnabled(bool enabled) {
    if (m_enabled != enabled) {
        m_enabled = enabled;
        if (enabled) {
            onEnable();
        } else {
            onDisable();
        }
    }
}

void IScanModule::addListener(IScannerListener* listener) {
    if (listener) {
        m_listeners.push_back(listener);
    }
}

void IScanModule::removeListener(IScannerListener* listener) {
    m_listeners.erase(
        std::remove(m_listeners.begin(), m_listeners.end(), listener),
        m_listeners.end()
    );
}

void IScanModule::notifyHit(const ScannerHitEvent& event) {
    for (auto* listener : m_listeners) {
        listener->onScannerHit(event);
    }
}

uint64_t IScanModule::getTimestamp() const {
    using namespace std::chrono;
    return duration_cast<microseconds>(
        system_clock::now().time_since_epoch()
    ).count();
}

void IScanModule::log(const std::string& message) {
    std::cout << "[" << m_name << "] " << message << std::endl;
}

//=============================================================================
// ScannerFactory implementation
//=============================================================================

ScannerFactory& ScannerFactory::instance() {
    static ScannerFactory factory;
    return factory;
}

void ScannerFactory::registerScanner(const std::string& type, CreatorFunc creator) {
    m_creators[type] = creator;
}

IScanModulePtr ScannerFactory::createScanner(const std::string& type, const ScannerConfig& config) {
    auto it = m_creators.find(type);
    if (it != m_creators.end()) {
        return it->second(config);
    }
    return nullptr;
}

} // namespace wt_option
