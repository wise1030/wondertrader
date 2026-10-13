/*!
 * \file DceMOStrikeFinder.h
 * \brief DCE MO strike intervals — absorbed from quantbox optioncore.
 *
 * Exchange rule: price-tiered intervals — <2000: 25, <5000: 50, else 100.
 */
#pragma once

#include "IStrikeFinder.h"

namespace wt_option {

class DceMOStrikeFinder : public IStrikeFinder
{
public:
    explicit DceMOStrikeFinder(ExpiryType type) : m_type(type) {}

    double getStrike(double price) const override;
    uint32_t getStrikeInterval(double price) const override;

private:
    ExpiryType m_type;
};

} // namespace wt_option
