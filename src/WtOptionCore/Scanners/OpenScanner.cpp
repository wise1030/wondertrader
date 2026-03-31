/*!
 * \file OpenScanner.cpp
 * \brief Open interest scanner implementation
 */

#include "OpenScanner.h"
#include <cmath>

namespace wt_option {

OpenScanner::OpenScanner(const OpenScannerConfig& config)
    : IScanModule("OpenScanner")
    , m_config(config)
{
}

void OpenScanner::onStart() {
    setEnabled(true);
}

void OpenScanner::onStop() {
    setEnabled(false);
}

void OpenScanner::onSessionStart() {
    // Capture start of day snapshots
    m_startOfDay = m_current;
}

void OpenScanner::onTick(const OptionGrid* grid) {
    if (!isEnabled() || !grid) return;
    
    const_cast<OptionGrid*>(grid)->forEachOption([this](OptionData* option) {
        evalOption(option);
    });
}

void OpenScanner::onOptionUpdate(OptionData* option) {
    if (!isEnabled()) return;
    
    updateSnapshot(option);
    evalOption(option);
}

void OpenScanner::updateSnapshot(OptionData* option) {
    if (!option) return;
    
    OISnapshot snapshot;
    snapshot.timestamp = option->getMarket().updateTime;
    snapshot.openInterest = option->getMarket().openInterest;
    snapshot.volume = option->getMarket().volume;
    snapshot.price = option->getMid();
    
    m_current[option->getCode()] = snapshot;
}

void OpenScanner::evalOption(OptionData* option) {
    if (!option) return;
    
    const std::string& code = option->getCode();
    
    // Check delta bounds
    double delta = std::abs(option->greeks().delta());
    if (delta < m_config.minDelta || delta > m_config.maxDelta) {
        return;
    }
    
    auto currentIt = m_current.find(code);
    auto startIt = m_startOfDay.find(code);
    
    if (currentIt == m_current.end()) return;
    
    const OISnapshot& current = currentIt->second;
    
    // Check volume threshold
    if (current.volume >= m_config.volumeThreshold) {
        // High volume signal
        ScannerHitEvent event;
        event.option = option;
        event.signal = current.volume;
        event.reason = getName() + ": High volume: " + std::to_string(current.volume);
        notifyHit(event);
    }
    
    // Check OI change from start of day
    if (startIt != m_startOfDay.end()) {
        const OISnapshot& start = startIt->second;
        int32_t oiChange = current.openInterest - start.openInterest;
        
        if (std::abs(oiChange) >= m_config.openInterestThreshold) {
            ScannerHitEvent event;
            event.option = option;
            event.signal = oiChange;
            event.reason = getName() + ": OI change: " + std::to_string(oiChange);
            notifyHit(event);
        }
    }
    
    // Check volume/OI ratio
    if (current.openInterest > 0) {
        double ratio = static_cast<double>(current.volume) / current.openInterest;
        if (ratio >= m_config.volumeOiRatio) {
            ScannerHitEvent event;
            event.option = option;
            event.signal = ratio;
            event.reason = getName() + ": High Volume/OI ratio: " + std::to_string(ratio);
            notifyHit(event);
        }
    }
}

} // namespace wt_option
