#pragma once

#include "../OptionValues.h"
#include "../Includes/WTSMarcos.h"
#include <string>
#include <memory>
#include <cstdint>

NS_WTP_BEGIN
class WTSVariant;
NS_WTP_END

namespace wt_option {

class OptionData;
class OptionGrid;

struct SignalContext {
    double time = 0;
    double underlyingPrice = 0;
    double frontForward = NAN;
    double frontAtmVol = 0;
    OptionGrid* grid = nullptr;
};

class ISignal {
public:
    virtual ~ISignal() = default;
    virtual const std::string& getName() const = 0;
    virtual bool init(wtp::WTSVariant* cfg) = 0;
    virtual bool isEnabled() const { return m_enabled; }
    virtual void setEnabled(bool b) { m_enabled = b; }

    virtual void onTradeTick(const std::string& code, double price, double size,
                             const OptionData* od) { (void)code; (void)price; (void)size; (void)od; }
    virtual void onFill(const std::string& code, bool isBuy, double qty, double price) {
        (void)code; (void)isBuy; (void)qty; (void)price; }
    virtual void onBatchStart(SignalContext& ctx) { (void)ctx; }
    virtual void onBatchEnd() {}

    // Host-driven clock (seconds-of-day). Must be refreshed by the strategy
    // before each batch / before dispatching fills, so that time-based logic
    // inside signals (EMA decay, window expiry, recovery timers) works.
    // B05 fix: previously signals had no time source and degenerated.
    void setSignalTime(double t) { m_signalTime = t; }
    double getSignalTime() const { return m_signalTime; }

protected:
    bool m_enabled = true;
    double m_signalTime = 0;
};

class IAlphaSignal : public ISignal {
public:
    using Ptr = std::shared_ptr<IAlphaSignal>;
    virtual double getWeight() const = 0;
    virtual void setWeight(double w) = 0;
    virtual double getVegaAdjust(const OptionData* od, const SignalContext& ctx) const { (void)od; (void)ctx; return 0; }
    virtual double getDeltaAdjust(const OptionData* od, const SignalContext& ctx) const { (void)od; (void)ctx; return 0; }
};

enum class RiskAction { None, Widen, StopQuoting, Panic };

class IRiskSignal : public ISignal {
public:
    using Ptr = std::shared_ptr<IRiskSignal>;
    virtual RiskAction getAction() const { return RiskAction::None; }
    virtual double getWidenFactor() const { return 1.0; }
    virtual RiskAction getActionByCode(const std::string& code) const { (void)code; return RiskAction::None; }
    virtual double getWidenFactorByCode(const std::string& code) const { (void)code; return 1.0; }
    /// B20: clear latched state (session begin)
    virtual void reset() {}
};

} // namespace wt_option
