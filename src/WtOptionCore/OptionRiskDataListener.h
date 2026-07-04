/*!
 * \file OptionRiskDataListener.h
 * \brief Option risk data listener interface (migrated from quantbox, no longbeach dependency)
 *
 * Original: longbeach::optioncore::OptionRiskDataListener (pure virtual callback)
 * Migration:
 *  - namespace longbeach::optioncore -> wt_option
 *  - Method signature unchanged
 */
#pragma once

namespace wt_option {

class OptionRiskData;
class OptionGreeks;
class OptionExpiryGreeks;

/// Listener for OptionRiskData position/Greeks change events.
/// (Migrated from longbeach::optioncore::OptionRiskDataListener; pure virtual.)
class OptionRiskDataListener
{
public:
    virtual ~OptionRiskDataListener() {}

    /// Called when an option's position-weighted Greeks change.
    /// \param d     the OptionRiskData whose position changed
    /// \param prev  the previous position-weighted Greeks (before the change)
    virtual void onPositionGreekChanged(const OptionRiskData& d,
                                        const OptionGreeks& prev) = 0;
};

} // namespace wt_option
