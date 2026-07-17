#pragma once

#include "ISignal.h"
#include "../OptionValues.h"
#include <string>
#include <cmath>

namespace wt_option {

class VegaFlowSignal : public IAlphaSignal {
public:
    VegaFlowSignal() : m_name("VegaFlow") {}
    const std::string& getName() const override { return m_name; }
    bool init(wtp::WTSVariant* cfg) override;
    double getWeight() const override { return m_weight; }
    void setWeight(double w) override { m_weight = w; }
    void onTradeTick(const std::string& code, double price, double size,
                     const OptionData* od) override;
    double getVegaAdjust(const OptionData* od, const SignalContext& ctx) const override;
private:
    std::string m_name;
    double m_weight = 0.0;
    EMAFilter m_ema;
    double m_window = 120.0;
};

class DeltaFlowSignal : public IAlphaSignal {
public:
    DeltaFlowSignal() : m_name("DeltaFlow") {}
    const std::string& getName() const override { return m_name; }
    bool init(wtp::WTSVariant* cfg) override;
    double getWeight() const override { return m_weight; }
    void setWeight(double w) override { m_weight = w; }
    void onTradeTick(const std::string& code, double price, double size,
                     const OptionData* od) override;
    double getDeltaAdjust(const OptionData* od, const SignalContext& ctx) const override;
private:
    std::string m_name;
    double m_weight = 0.0;
    EMAFilter m_ema;
    double m_window = 120.0;
};

class AtmSigSignal : public IAlphaSignal {
public:
    AtmSigSignal() : m_name("AtmSig") {}
    const std::string& getName() const override { return m_name; }
    bool init(wtp::WTSVariant* cfg) override;
    double getWeight() const override { return m_weight; }
    void setWeight(double w) override { m_weight = w; }
    double getDeltaAdjust(const OptionData* od, const SignalContext& ctx) const override;
private:
    std::string m_name;
    double m_weight = 0.0;
};

class RollEmaSignal : public IAlphaSignal {
public:
    RollEmaSignal() : m_name("RollEma") {}
    const std::string& getName() const override { return m_name; }
    bool init(wtp::WTSVariant* cfg) override;
    double getWeight() const override { return m_weight; }
    void setWeight(double w) override { m_weight = w; }
    void onBatchStart(SignalContext& ctx) override;
    double getDeltaAdjust(const OptionData* od, const SignalContext& ctx) const override;
private:
    std::string m_name;
    double m_weight = 0.0;
    EMAFilter m_ema;
    double m_window = 120.0;
    double m_rollema = 0.0;
};

class FrontFutSkewSignal : public IAlphaSignal {
public:
    FrontFutSkewSignal() : m_name("FrontFutSkew") {}
    const std::string& getName() const override { return m_name; }
    bool init(wtp::WTSVariant* cfg) override;
    double getWeight() const override { return m_weight; }
    void setWeight(double w) override { m_weight = w; }
    void onBatchStart(SignalContext& ctx) override;
    double getVegaAdjust(const OptionData* od, const SignalContext& ctx) const override;
private:
    std::string m_name;
    double m_weight = 0.0;
    EMAFilter m_ema;
    double m_window = 120.0;
    double m_skew = 0.0;
};

class FrontAtmvFlowSignal : public IAlphaSignal {
public:
    FrontAtmvFlowSignal() : m_name("FrontAtmvFlow") {}
    const std::string& getName() const override { return m_name; }
    bool init(wtp::WTSVariant* cfg) override;
    double getWeight() const override { return m_weight; }
    void setWeight(double w) override { m_weight = w; }
    void onBatchStart(SignalContext& ctx) override;
    double getVegaAdjust(const OptionData* od, const SignalContext& ctx) const override;
private:
    std::string m_name;
    double m_weight = 0.0;
    EMAFilter m_ema;
    double m_window = 120.0;
    double m_flow = 0.0;
};

class ForwardSpreadSignal : public IAlphaSignal {
public:
    ForwardSpreadSignal() : m_name("ForwardSpread") {}
    const std::string& getName() const override { return m_name; }
    bool init(wtp::WTSVariant* cfg) override;
    double getWeight() const override { return m_weight; }
    void setWeight(double w) override { m_weight = w; }
    double getDeltaAdjust(const OptionData* od, const SignalContext& ctx) const override;
private:
    std::string m_name;
    double m_weight = 0.0;
};

} // namespace wt_option
