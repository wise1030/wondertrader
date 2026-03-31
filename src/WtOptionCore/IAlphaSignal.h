/*!
 * \file IAlphaSignal.h
 * \brief Interface for Alpha Signals (Price/Vol Prediction)
 */
#pragma once

#include <string>
#include <memory>
#include <vector>
#include "../Includes/WTSDataDef.hpp"

namespace wt_option {

class OptionData;

class IAlphaSignal
{
public:
    virtual ~IAlphaSignal() {}
    
    // Initialize with config
    virtual bool init(const std::string& config) = 0;
    
    // Update on tick
    virtual void onTick(OptionData* option, const wtp::WTSTickData* tick) = 0;
    
    // Get Signal Value (scaled -1.0 to 1.0 usually)
    virtual double getValue() const = 0;
    
    // Get Signal Name
    virtual const std::string& getName() const = 0;
};

using IAlphaSignalPtr = std::shared_ptr<IAlphaSignal>;

} // namespace wt_option
