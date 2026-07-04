/*!
 * \file OptionGreeks.h
 * \brief Option Greeks container (migrated from quantbox, no longbeach dependency)
 */
#pragma once

#include "optioncoretypes.h"

namespace wt_option {

enum GreekType { GT_delta, GT_vega, GT_vega_tw, GT_theta, GT_gamma, GT_vanna, GT_volga, GT_unknown_greek };

class OptionGreeks
{
public:
    OptionGreeks();

    void reset();
    OptionGreeks& accum(const OptionGreeks& g);
    OptionGreeks& accum(double m, const OptionGreeks& g);
    OptionGreeks& reduce(const OptionGreeks& g);
    OptionGreeks& reduce(double m, const OptionGreeks& g);
    OptionGreeks& apply(double m, const OptionGreeks& g);

    double& delta() { return m_delta; }
    double delta() const { return m_delta; }

    double& gamma() { return m_gamma; }
    double gamma() const { return m_gamma; }

    double& vega() { return m_vega; }
    double vega() const { return m_vega; }

    double& vegaTW() { return m_vega_tw; }
    double vegaTW() const { return m_vega_tw; }

    double& theta() { return m_theta; }
    double theta() const { return m_theta; }

    double& rho() { return m_rho; }
    double rho() const { return m_rho; }

    double& vanna() { return m_vanna; }
    double vanna() const { return m_vanna; }

    double& volga() { return m_volga; }
    double volga() const { return m_volga; }

    OptionGreeks& operator -= (const OptionGreeks& rhs);
    OptionGreeks operator + (const OptionGreeks& rhs);
    OptionGreeks operator - (const OptionGreeks& rhs);
    OptionGreeks operator - ();

protected:
    double m_delta;
    double m_gamma;
    double m_vega;
    double m_vega_tw;
    double m_theta;
    double m_rho;
    double m_vanna;
    double m_volga;
};

using OptionGreeksPtr = std::shared_ptr<OptionGreeks>;

} // namespace wt_option
