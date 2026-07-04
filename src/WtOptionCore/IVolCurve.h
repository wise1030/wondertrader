/*!
 * \file IVolCurve.h
 * \brief Volatility curve interface (migrated from quantbox, no longbeach dependency)
 *
 * Original: longbeach::optioncore::IVolCurve depended on longbeach::ICurve,
 * LuaStream/LuabindScripting (luabind config magic), and longbeach::datasetT.
 *
 * Migration notes:
 *  - namespace longbeach::optioncore -> wt_option
 *  - luabind scripting / LuaStream / ConfigBase factory magic removed.
 *    Config subclasses are kept as plain structs; construction is direct.
 *  - boost::tuple -> std::tuple, boost::shared_ptr -> std::shared_ptr
 *  - longbeach::datasetT<boost::tuple<double,double>> -> std::vector<std::pair<double,double>>
 *  - VCConfigurable template removed (it only wired luabind); Config<T>::Config
 *    now just inherits ConfigBase directly.
 */
#pragma once

#include "optioncoretypes.h"

#include <vector>
#include <tuple>
#include <memory>
#include <string>
#include <utility>

namespace wt_option {

// ---------------------------------------------------------------------------
// ICurve — generic 1-D curve (minimal replacement for longbeach::ICurve)
// ---------------------------------------------------------------------------
class ICurve
{
public:
    typedef std::pair<double, double> datapoint_t;
    typedef std::vector<datapoint_t> dataset_t;

    static datapoint_t make_datapoint(double a, double b) { return std::make_pair(a, b); }

    virtual ~ICurve() {}

    virtual double operator()(double diff) const { return 0; }
    virtual bool   fit(const dataset_t& points) { return false; }
    virtual bool   isInitialized() const { return false; }
};
using ICurvePtr = std::shared_ptr<ICurve>;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
class OptionGrid;
class OptionData;

class IVolCurve;
using IVolCurvePtr = std::shared_ptr<IVolCurve>;
using IVolCurveCPtr = std::shared_ptr<const IVolCurve>;

// ---------------------------------------------------------------------------
// IVolCurve — volatility curve interface
// ---------------------------------------------------------------------------
class IVolCurve : public ICurve
{
public:
    // ConfigBase is retained as a base for concrete Config structs, but the
    // luabind/virtual-instance factory magic is removed. Concrete curves are
    // constructed directly (e.g. std::make_shared<GvvVolCurve>(config)).
    class ConfigBase
    {
    public:
        virtual ~ConfigBase() {}
    };
    using ConfigBasePtr = std::shared_ptr<ConfigBase>;

    virtual ~IVolCurve() {}

    using ICurve::operator();
    /// Evaluate vol multiplier (relative to atmvol) for an option given atm forward.
    virtual double operator()(const OptionData& od, double atmforward) const = 0;
    virtual const std::string getNameString() const = 0;

    virtual void setATMForward(double atmforward) {}
    virtual void setMaturity(double maturity) {}

    virtual void setATMVol(double atmvol) {}
    virtual void setInitialized(bool b) {}
};

// Convenience alias used by concrete configs.
template<typename D> class VCConfigurable
{
protected:
    template<class T> class Config : public D::ConfigBase
    {
    };
};

} // namespace wt_option
