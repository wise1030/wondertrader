/*!
 * \file OptionGreeks.cpp
 * \brief Option Greeks container implementation (migrated from quantbox)
 */
#include "OptionGreeks.h"

namespace wt_option {

OptionGreeks::OptionGreeks() { reset(); }

void OptionGreeks::reset() {
    m_delta = 0; m_gamma = 0; m_vega = 0; m_vega_tw = 0;
    m_theta = 0; m_rho = 0; m_vanna = 0; m_volga = 0;
}

OptionGreeks& OptionGreeks::accum(const OptionGreeks& g) {
    m_delta += g.m_delta; m_gamma += g.m_gamma; m_vega += g.m_vega; m_vega_tw += g.m_vega_tw;
    m_theta += g.m_theta; m_rho += g.m_rho; m_vanna += g.m_vanna; m_volga += g.m_volga;
    return *this;
}

OptionGreeks& OptionGreeks::accum(double m, const OptionGreeks& g) {
    m_delta += m * g.m_delta; m_gamma += m * g.m_gamma; m_vega += m * g.m_vega; m_vega_tw += m * g.m_vega_tw;
    m_theta += m * g.m_theta; m_rho += m * g.m_rho; m_vanna += m * g.m_vanna; m_volga += m * g.m_volga;
    return *this;
}

OptionGreeks& OptionGreeks::reduce(const OptionGreeks& g) {
    m_delta -= g.m_delta; m_gamma -= g.m_gamma; m_vega -= g.m_vega; m_vega_tw -= g.m_vega_tw;
    m_theta -= g.m_theta; m_rho -= g.m_rho; m_vanna -= g.m_vanna; m_volga -= g.m_volga;
    return *this;
}

OptionGreeks& OptionGreeks::reduce(double m, const OptionGreeks& g) {
    m_delta -= m * g.m_delta; m_gamma -= m * g.m_gamma; m_vega -= m * g.m_vega; m_vega_tw -= m * g.m_vega_tw;
    m_theta -= m * g.m_theta; m_rho -= m * g.m_rho; m_vanna -= m * g.m_vanna; m_volga -= m * g.m_volga;
    return *this;
}

OptionGreeks& OptionGreeks::apply(double m, const OptionGreeks& g) {
    m_delta = m * g.m_delta; m_gamma = m * g.m_gamma; m_vega = m * g.m_vega; m_vega_tw = m * g.m_vega_tw;
    m_theta = m * g.m_theta; m_rho = m * g.m_rho; m_vanna = m * g.m_vanna; m_volga = m * g.m_volga;
    return *this;
}

OptionGreeks& OptionGreeks::operator -= (const OptionGreeks& rhs) { return reduce(rhs); }
OptionGreeks OptionGreeks::operator + (const OptionGreeks& rhs) { OptionGreeks r = *this; return r.accum(rhs); }
OptionGreeks OptionGreeks::operator - (const OptionGreeks& rhs) { OptionGreeks r = *this; return r.reduce(rhs); }
OptionGreeks OptionGreeks::operator - () { OptionGreeks r; return r.apply(-1, *this); }

} // namespace wt_option
