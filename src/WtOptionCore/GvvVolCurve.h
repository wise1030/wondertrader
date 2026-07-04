/*!
 * \file GvvVolCurve.h
 * \brief GVV (Generalised Vanna-Volga) volatility curve (migrated from quantbox)
 *
 * GSL dependency replaced by WLS3 (hand-written 3×3 weighted least squares).
 * QuantLib::Brent replaced by wt_option::Brent (BlackImpliedCalculator.h).
 * sqrt negative-value protection applied (AGENTS.md known bug fix).
 */
#pragma once

#include "IVolCurve.h"
#include "WLS3.h"

#include <string>

namespace wt_option {

const double GVV_MINATMVOL = 0.01;
const double GVV_MAXATMVOL = 2.0;
const double GVV_MINRHO    = -0.99;
const double GVV_MAXRHO    =  0.99;
const double GVV_MINVOLVOL = 0.01;
const double GVV_MAXVOLVOL = 5.0;
const double GVV_MINALPHA  = 0.0;
const double GVV_MAXALPHA  = 2.0;

class GvvVolCurve : public IVolCurve
{
public:
    struct Config : public IVolCurve::ConfigBase
    {
        Config()
            : beta(0.0)
            , alpha_dn(GVV_MINALPHA)
            , alpha_up(GVV_MAXALPHA)
        {}

        double beta;
        double alpha_dn;
        double alpha_up;
    };

    enum Parameter { SPOTVOL, RHO, VOLVOL, ALPHA, ATMVOL };

    GvvVolCurve(const Config& c);
    GvvVolCurve();
    ~GvvVolCurve();

    double eval(double x) const;

    using IVolCurve::operator();
    virtual double operator()(const OptionData& od, double atmforward) const;
    virtual double operator()(double diff) const;
    virtual bool fit(const IVolCurve::dataset_t& points);
    void setInitialized(bool b) { m_bInitialized = b; }
    virtual bool isInitialized() const { return m_bInitialized && !std::isnan(m_atmforward) && !std::isnan(m_maturity) && !std::isnan(m_atmvol); }
    bool isLastFitOK() const { return m_bLastFitOK; }
    virtual const std::string getNameString() const { return "GvvVolCurve"; }

    virtual void setATMForward(double atmforward);
    virtual void setMaturity(double maturity);
    void setATMVol(double atmvol);

    void setParameter(const Parameter& paramType, double val);
    double getParameter(const Parameter& paramType) const;
    double getVolBump(const Parameter& paramType, double paramBump, double stk);

private:
    double fitWithAlpha(double alpha, const IVolCurve::dataset_t& points,
                        double* atmvol2, double* skew, double* kurt);

    double m_beta;
    double m_atmforward;
    double m_maturity;
    double m_atmvol;

    double m_spotvol;
    double m_rho;
    double m_volvol;
    double m_alpha;
    void update_parameters(double spotvol, double rho, double volvol, double alpha);
    double m_alpha_dn;
    double m_alpha_up;

    // WLS3 solver (replaces gsl_multifit workspace) — zero allocation
    double m_chisq;
    bool m_bInitialized;
    bool m_bLastFitOK;
};

using GvvVolCurvePtr = std::shared_ptr<GvvVolCurve>;

} // namespace wt_option
