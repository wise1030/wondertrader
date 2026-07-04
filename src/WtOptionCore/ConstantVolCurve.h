/*!
 * \file ConstantVolCurve.h
 * \brief Constant volatility curve (migrated from quantbox, header-only)
 *
 * Original: longbeach::optioncore::ConstantVolCurve used luabind/LuaStream in
 * its Config. The Config here is a plain struct.
 *
 * Migration: namespace longbeach::optioncore -> wt_option; luabind removed.
 */
#pragma once

#include "IVolCurve.h"

namespace wt_option {

class ConstantVolCurve : public IVolCurve
{
public:
    struct Config : public IVolCurve::ConfigBase
    {
        Config() : value(1.0) {}
        Config(double v) : value(v) {}
        double value;
    };

    ConstantVolCurve(double constant) : m_constant(constant) {}
    ConstantVolCurve(const Config& c) : m_constant(c.value) {}

    using IVolCurve::operator();
    virtual double operator()(const OptionData& od, double atmforward) const { return 1; }
    virtual double operator()(double diff) const { return m_constant; }
    virtual const std::string getNameString() const { return "ConstantVolCurve"; }

private:
    double m_constant;
};

} // namespace wt_option
