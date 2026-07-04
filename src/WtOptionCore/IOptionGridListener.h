/*!
 * \file IOptionGridListener.h
 * \brief Option grid listener interface (migrated from quantbox, no longbeach dependency)
 *
 * Original: longbeach::optioncore::IOptionGridListener (pure virtual callbacks)
 * Migration:
 *  - namespace longbeach::optioncore -> wt_option
 *  - boost::shared_ptr / LONGBEACH_*_SHARED_PTR -> std::shared_ptr
 *  - All pure virtual methods preserved as-is
 */
#pragma once

#include "optioncoretypes.h"

namespace wt_option {

class IOptionGrid;

class IOptionGridListener
{
public:
    virtual ~IOptionGridListener() {}

    virtual void onAddOption(const OptionDataPtr& /*od*/) {}
    virtual void onAddExpiry(const ExpiryDataPtr& /*ed*/) {}
    virtual void onComputeValuesCompleted(const IOptionGrid* /*grid*/) {}
};

// Convenience empty subclass (mirrors original OptionGridListener)
class OptionGridListener : public IOptionGridListener {};

} // namespace wt_option
