/*!
 * \file LinearVolCurve.h
 * \brief Linear interpolation vol curve (migrated from quantbox)
 *
 * Original: longbeach::optioncore::LinearVolCurve inherited IVolCurve and
 * LinearVolCurveParams (autogen luabind params). Migration:
 *  - namespace -> wt_option
 *  - LinearVolCurveParams autogen -> inline struct members on Config
 *  - boost::tuple/boost::optional -> std::pair / plain members
 *  - dataset_t is now std::vector<std::pair<double,double>>
 *  - fpmath helpers (LE/GE/EQ) -> direct comparisons (see optioncoretypes.h note)
 */
#pragma once

#include "IVolCurve.h"

#include <string>
#include <map>

namespace wt_option {

const double LINEAR_MINVOL = 0.1;  // multiple of atmvol
const double LINEAR_MAXVOL = 5.0;  // multiple of atmvol

class LinearVolCurve : public IVolCurve
{
public:
    struct Config : public IVolCurve::ConfigBase
    {
        Config() : flat_extrapolation(false) {}

        static const char* name() { return "LinearVolCurve"; }

        bool flat_extrapolation;
        // initial fit points: (strike_offset_from_atm, vol_multiplier)
        std::map<double, double> points;
    };

public:
    LinearVolCurve(const Config& c);
    LinearVolCurve();
    ~LinearVolCurve();

    using IVolCurve::operator();
    virtual double operator()(const OptionData& od, double atmforward) const;
    virtual double operator()(double diff) const;
    double eval(double x) const;
    virtual bool fit(const dataset_t& points);
    virtual bool isInitialized() const { return m_bInitialized; }
    virtual const std::string getNameString() const { return "LinearVolCurve"; }

    virtual void setATMForward(double atmforward) { m_atmforward = atmforward; }
    virtual void setMaturity(double maturity) { m_maturity = maturity; }

private:
    bool m_bInitialized;
    bool m_extrapolateFlat;

    dataset_t m_points;
    datapoint_t m_minXpt;
    datapoint_t m_maxXpt;

    double m_atmforward;
    double m_maturity;
};

} // namespace wt_option
