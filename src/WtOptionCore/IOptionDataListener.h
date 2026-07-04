/*!
 * \file IOptionDataListener.h
 * \brief Option data listener interface (migrated from quantbox, no longbeach dependency)
 *
 * Original: longbeach::optioncore::IOptionDataListener (pure virtual callback)
 * Migration:
 *  - namespace longbeach::optioncore -> wt_option
 *  - Method signature unchanged
 */
#pragma once

#include <stddef.h>

namespace wt_option {

class OptionData;

class IOptionDataListener
{
public:
    virtual ~IOptionDataListener() {}

    virtual void onMarketsPriced(const OptionData& od, size_t index) = 0;
    // virtual void onFill( const OrderPtr &order, double fill_px, uint32_t fill_qty ) = 0;
};

} // namespace wt_option
