/*!
 * \file VolCurve.h
 * \brief Volatility curve interface and implementations
 * 
 * Migrated from longbeach/quantbox/strategy/optioncore/IVolCurve.h
 * and GvvVolCurve.h with full GVV model implementation.
 */

#pragma once

#include "OptionTypes.h"
#include <vector>
#include <tuple>
#include <memory>
#include <string>

namespace wt_option {

// Forward declarations
class OptionData;

// GVV parameter bounds
constexpr double GVV_MINATMVOL = 0.01;
constexpr double GVV_MAXATMVOL = 5.0;
constexpr double GVV_MINVOLVOL = 0.01;
constexpr double GVV_MAXVOLVOL = 5.0;
constexpr double GVV_MINRHO = -0.99;
constexpr double GVV_MAXRHO = 0.99;
constexpr double GVV_MINALPHA = 0.0;
constexpr double GVV_MAXALPHA = 2.0;

/**
 * @brief Data point for curve fitting (x, y)
 */
using DataPoint = std::tuple<double, double>;
using DataSet = std::vector<DataPoint>;

/**
 * @brief Base curve interface
 */
class ICurve {
public:
    virtual ~ICurve() = default;
    
    virtual double operator()(double x) const = 0;
    virtual bool fit(const DataSet& points) = 0;
    virtual bool isInitialized() const = 0;
};

using ICurvePtr = std::shared_ptr<ICurve>;

/**
 * @brief Volatility curve interface
 */
class IVolCurve : public ICurve {
public:
    virtual ~IVolCurve() = default;
    
    virtual double getVol(double strike, double atmForward) const = 0;
    virtual std::string getName() const = 0;
    virtual void setATMForward(double atmForward) {}
    virtual void setATMVol(double atmVol) {}
    virtual void setMaturity(double maturity) {}
};

using IVolCurvePtr = std::shared_ptr<IVolCurve>;

/**
 * @brief Constant volatility curve (flat)
 */
class ConstantVolCurve : public IVolCurve {
public:
    explicit ConstantVolCurve(double vol = 0.0) : m_vol(vol), m_initialized(vol > 0) {}
    
    double operator()(double x) const override { return m_vol; }
    bool fit(const DataSet& points) override;
    bool isInitialized() const override { return m_initialized; }
    
    double getVol(double strike, double atmForward) const override { return m_vol; }
    std::string getName() const override { return "Constant"; }
    
    void setVol(double vol) { m_vol = vol; m_initialized = true; }
    
private:
    double m_vol;
    bool m_initialized;
};

/**
 * @brief Linear volatility curve (smile approximation)
 */
class LinearVolCurve : public IVolCurve {
public:
    LinearVolCurve() : m_a(0), m_b(0), m_initialized(false) {}
    
    double operator()(double x) const override;
    bool fit(const DataSet& points) override;
    bool isInitialized() const override { return m_initialized; }
    
    double getVol(double strike, double atmForward) const override;
    std::string getName() const override { return "Linear"; }
    
    double getA() const { return m_a; }
    double getB() const { return m_b; }
    
private:
    double m_a;  // ATM vol
    double m_b;  // Skew
    bool m_initialized;
};

/**
 * @brief GVV (Gatheral Vol-of-Vol) curve implementation
 * 
 * Full implementation matching longbeach GvvVolCurve.cc
 * 
 * Model: variance(k) = atmvol^2 + skew * x + kurt * x^2
 * where x = log(K/F) * vol^(alpha-1)
 * 
 * Parameters:
 * - spotvol: Spot volatility
 * - rho: Correlation between spot and vol
 * - volvol: Vol-of-vol
 * - alpha: Backbone exponent
 * - beta: Weight decay for fitting
 */
class GvvVolCurve : public IVolCurve {
public:
    // GVV parameter types
    enum Parameter { SPOTVOL, RHO, VOLVOL, ALPHA, ATMVOL };
    
    GvvVolCurve();
    explicit GvvVolCurve(double beta, double alphaLow = 0.5, double alphaHigh = 1.5);
    
    double operator()(double x) const override;
    bool fit(const DataSet& points) override;
    bool isInitialized() const override { return m_initialized; }
    bool isLastFitOK() const { return m_lastFitOK; }
    
    double getVol(double strike, double atmForward) const override;
    std::string getName() const override { return "GVV"; }
    
    void setATMForward(double atmForward) override { m_atmForward = atmForward; }
    void setATMVol(double atmVol) override { m_atmVol = std::max(GVV_MINATMVOL, std::min(GVV_MAXATMVOL, atmVol)); }
    void setMaturity(double maturity) override { m_maturity = maturity; }
    
    // Parameter access
    double getParameter(Parameter param) const;
    void setParameter(Parameter param, double value);
    void updateParameters(double spotvol, double rho, double volvol, double alpha);
    
    // Get vol bump for sensitivity analysis
    double getVolBump(Parameter param, double paramBump, double strike);
    
    // Accessors
    double getSpotVol() const { return m_spotvol; }
    double getRho() const { return m_rho; }
    double getVolVol() const { return m_volvol; }
    double getAlpha() const { return m_alpha; }
    double getBeta() const { return m_beta; }
    double getATMVol() const { return m_atmVol; }
    
private:
    /**
     * @brief Evaluate normalized vol at log-moneyness x
     */
    double eval(double x) const;
    
    /**
     * @brief Fit with fixed alpha, returning chi-squared
     */
    double fitWithAlpha(double alpha, const DataSet& points,
                        double* atmvol2, double* skew, double* kurt);
    
    /**
     * @brief Weighted linear regression
     */
    void weightedLinearFit(const std::vector<double>& X1,
                           const std::vector<double>& X2,
                           const std::vector<double>& Y,
                           const std::vector<double>& W,
                           double* c0, double* c1, double* c2,
                           double* chisq);
    
    // Model parameters
    double m_beta;          // Weight decay for fitting
    double m_atmForward;    // ATM forward price
    double m_maturity;      // Time to expiry
    double m_atmVol;        // ATM volatility
    double m_spotvol;       // Spot volatility
    double m_rho;           // Spot-vol correlation
    double m_volvol;        // Vol-of-vol
    double m_alpha;         // Backbone exponent
    double m_alphaLow;      // Alpha lower bound for optimization
    double m_alphaHigh;     // Alpha upper bound for optimization
    
    bool m_initialized;
    bool m_lastFitOK;
};

using ConstantVolCurvePtr = std::shared_ptr<ConstantVolCurve>;
using LinearVolCurvePtr = std::shared_ptr<LinearVolCurve>;
using GvvVolCurvePtr = std::shared_ptr<GvvVolCurve>;

} // namespace wt_option
