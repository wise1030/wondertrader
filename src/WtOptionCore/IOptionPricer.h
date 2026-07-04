/*!
 * \file IOptionPricer.h
 * \brief Option pricer interface (migrated from quantbox)
 *
 * Original: longbeach::optioncore::IOptionPricer inherited OPConfigurable
 * and wired up luabind scripting (LuaState, LuabindScripting, LuaStream),
 * CommandServices, ClientContext, ListenerList, and DECL_EVENT macros.
 *
 * Migration:
 *  - namespace -> wt_option
 *  - All luabind / LuaStream / LuaState scripting removed
 *  - OPConfigurable template removed (it only drove luabind config factory)
 *  - ConfigBase retained as a plain base for concrete Config structs, but the
 *    virtual-instance factory is gone; construct concrete pricers directly
 *  - DECL_EVENT(SingleOptionChangedEvent,...) -> std::function callback vector
 *  - ListenerList<pricingChangedEventSink> -> std::vector<std::function>
 *  - CommandServicesPtr / ClientContextPtr -> removed from instance(); the
 *    concrete pricers take only what they need (OptionRisk, grid, etc.)
 *  - OptionGrid forward-declared; OptionData included via optioncoretypes.h
 *  - instrument_t/symbol_t -> std::string; expiry_t -> uint32_t
 */
#pragma once

#include "optioncoretypes.h"
#include "OptionGreeks.h"
#include "IVolCurve.h"

#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include <vector>

namespace wt_option {

class OptionGrid;
class OptionData;

class IOptionPricer;
using IOptionPricerPtr = std::shared_ptr<IOptionPricer>;

class IOptionPricer
{
public:
    typedef std::function<void(IOptionPricer*)> pricingChangedEventSink;
    typedef std::function<void(IOptionPricer*, OptionData*)> singleOptionChangedEventSink;

public:
    virtual ~IOptionPricer() {}

    virtual bool computeValues(OptionGrid* grid) = 0;
    virtual bool computeImpliedValues(OptionGrid* grid) { return false; }

    virtual bool initValuesCompute(OptionGrid* grid) = 0;
    virtual void computeValue(OptionData* option) = 0;
    virtual void finalizeCompute(OptionGrid* grid) = 0;

    virtual bool isPanicked() const = 0;

    // GVV curve
    virtual IVolCurvePtr getVolCurve(uint32_t expiry) const = 0;
    // fit to market
    virtual IVolCurvePtr getVolCurve2(uint32_t expiry) const = 0;
    // sprd fwd vs atmfwd
    virtual IVolCurvePtr getFwdCurve(uint32_t expiry) const = 0;

    virtual double getMaturity(uint32_t expiry) const = 0;
    virtual double getATMForward(uint32_t expiry) const = 0;

    virtual void   setATMVol(uint32_t expiry, double atmvol) {}
    virtual double getATMVol(uint32_t expiry) const = 0;

    virtual double getFutSprd(uint32_t expiry) const { return 0; }
    virtual double getATMVolSprd(uint32_t expiry) const { return 0; }

    virtual void setReprice(bool bReprice) = 0;

    virtual void    setTraceLevel(int32_t i) = 0;
    virtual int32_t getTraceLevel() const = 0;

    // Simple callback registration (replaces ListenerList / DECL_EVENT)
    void addPricingChangedCallback(pricingChangedEventSink cb)
    { m_pricingChangedCallbacks.push_back(std::move(cb)); }
    void addSingleOptionChangedCallback(singleOptionChangedEventSink cb)
    { m_singleOptionChangedCallbacks.push_back(std::move(cb)); }

protected:
    void firePricingChanged()
    {
        for (auto& cb : m_pricingChangedCallbacks) cb(this);
    }
    void fireSingleOptionChanged(OptionData* od)
    {
        for (auto& cb : m_singleOptionChangedCallbacks) cb(this, od);
    }

private:
    std::vector<pricingChangedEventSink>       m_pricingChangedCallbacks;
    std::vector<singleOptionChangedEventSink>  m_singleOptionChangedCallbacks;

public:
    // ConfigBase retained as a base for concrete Config structs.
    class ConfigBase
    {
    public:
        virtual ~ConfigBase() {}
    };
    using ConfigBasePtr = std::shared_ptr<ConfigBase>;
};

} // namespace wt_option
