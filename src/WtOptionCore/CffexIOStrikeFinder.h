/*!
 * \file CffexIOStrikeFinder.h
 * \brief CFFEX IO/MO/HO strike intervals — absorbed from quantbox optioncore.
 *
 * Exchange rule: monthly series quote 50-point intervals, quarterly series
 * (nearest two quarters beyond month+2) quote 100-point intervals.
 */
#pragma once

#include "IStrikeFinder.h"

namespace wt_option {

class CffexIOStrikeFinder : public IStrikeFinder
{
public:
    explicit CffexIOStrikeFinder(ExpiryType type) : m_type(type) {}

    double getStrike(double price) const override;
    uint32_t getStrikeInterval(double /*price*/) const override
    { return getStrikeInterval(); }

private:
    uint32_t getStrikeInterval() const;

    ExpiryType m_type;
};

} // namespace wt_option
