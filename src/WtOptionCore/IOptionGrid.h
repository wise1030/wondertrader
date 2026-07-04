/*!
 * \file IOptionGrid.h
 * \brief Option Grid Interface (simplified for WT — no multi_index)
 */
#pragma once

#include "optioncoretypes.h"
#include "OptionData.h"
#include "ExpiryData.h"
#include "StrikeData.h"
#include "IOptionGridListener.h"

#include <map>
#include <vector>
#include <string>
#include <functional>

namespace wt_option {

class IOptionPricer;

using ExpiryTable = std::map<uint32_t, ExpiryDataPtr>;
using StrikeDataList = std::vector<StrikeDataPtr>;

class IOptionGrid
{
public:
    virtual ~IOptionGrid() {}

    virtual const std::string& getSymbol() const = 0;
    virtual const std::string& getUnderlyingCode() const = 0;

    virtual OptionDataPtr get(uint32_t expiry, strike_t strike, OptionRight right) = 0;
    virtual OptionDataPtr get(const std::string& code) = 0;

    virtual void computeValues(IOptionPricer* pricer = nullptr) = 0;
    virtual double getUnderlyingPrice() const = 0;

    virtual const ExpiryTable& expiries() const = 0;
    virtual ExpiryDataPtr getExpiryData(uint32_t expiry) const = 0;
    virtual ExpiryDataPtr getFrontMonthExpiryData() = 0;

    virtual const std::vector<OptionDataPtr>& getAllOptions() const = 0;
    virtual const StrikeDataList& getAllStrikes() const = 0;
    virtual std::vector<StrikeDataPtr> getStrikesByExpiry(uint32_t expiry) const = 0;

    virtual size_t numStrikes() const = 0;
    virtual size_t numOptions() const = 0;

    virtual void addListener(IOptionGridListener* listener) = 0;
    virtual void removeListener(IOptionGridListener* listener) = 0;
};

using IOptionGridPtr = std::shared_ptr<IOptionGrid>;

} // namespace wt_option
