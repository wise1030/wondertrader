/*!
 * \file IScanModule.h
 * \brief Scanner module base class (migrated from quantbox IScanModule.h)
 *
 * Original: longbeach::optiontrader::IScanModule inherited Configurable<IScanModule>
 *   and IScanModuleBase (which derived from CommandServicesHelper). It carried
 *   luabind/ScriptHelper/ConfigBaseT plumbing, DECL_EVENT(OptionHitEvent),
 *   notifiable<bool> m_enable, and registerScripting.
 *
 * Migration:
 *  - Configurable / ScriptHelper / ConfigBaseT / luabind → deleted entirely.
 *  - CommandServicesHelper → deleted; no command/property framework.
 *  - OptionTraderContext → simplified to a plain struct { bool enabled; bool panicked; }
 *    defined here (the original full context is not yet migrated and these are
 *    the only two fields scanners actually query).
 *  - DECL_EVENT(OptionHitEvent) → virtual void onOptionHit(OptionData*, int32_t) = 0
 *    (the event-sink pattern is replaced by a direct virtual callback).
 *  - IScanModuleBase::onRefresh(refresher&) → deleted (no notifier).
 *  - IScanModuleBase::getTime() → deleted (scanners get time from elsewhere).
 *  - boost::shared_ptr → std::shared_ptr.
 *
 * The interface is intentionally minimal so concrete scanners can be migrated
 * against it immediately.
 */
#pragma once

#include "optioncoretypes.h"

#include <memory>
#include <string>
#include <cstdint>

namespace wt_option {

class OptionData;

// ---------------------------------------------------------------------------
// OptionTraderContext — simplified context for scanner modules.
// The original longbeach::optiontrading::OptionTraderContext was a heavy
// object wired to notifiable properties, TradingContext, command services,
// etc. Scanners only read two bits of state from it: enabled() and
// isPanicked(). We collapse it to this struct.
// ---------------------------------------------------------------------------
// OptionTraderContext is defined in ControllableTradingGrid.h — forward declare here
struct OptionTraderContext;
using OptionTraderContextPtr = std::shared_ptr<OptionTraderContext>;

// ---------------------------------------------------------------------------
// IScanModule — base class for all scanner / signal modules.
// ---------------------------------------------------------------------------
class IScanModule
{
public:
    IScanModule() = default;
    explicit IScanModule(const OptionTraderContextPtr& ctx) : m_spContext(ctx) {}
    virtual ~IScanModule() = default;

    /// Human-readable name of this scanner (e.g. "MMScanner", "SpreadScanner").
    virtual std::string getName() const = 0;

    /// Called when the scanner is started (trading session begins).
    virtual void onStart() {}

    /// Called when the scanner is stopped (trading session ends).
    virtual void onStop() {}

    /// Called when a panic signal fires — scanner should stop generating hits.
    virtual void onPanic() {}

    /// Called when a scanner detects a hit on an option.
    /// \param od        The option that triggered the hit.
    /// \param valueIndex Which value set triggered (0-4, mirrors original index).
    virtual void onOptionHit(OptionData* od, int32_t valueIndex) = 0;

    // Context access (replaces IScanModuleBase::context())
    const OptionTraderContextPtr& context() const { return m_spContext; }
    void setContext(const OptionTraderContextPtr& ctx) { m_spContext = ctx; }

    bool isEnabled() const { return m_enable; }
    void setEnable(bool b) { m_enable = b; }

protected:
    OptionTraderContextPtr m_spContext;
    bool m_enable = true;
};

using IScanModulePtr = std::shared_ptr<IScanModule>;

} // namespace wt_option
