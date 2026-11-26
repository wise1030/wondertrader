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

    /// Re-base the curve's stored normalization by `factor` after the ATM
    /// anchor moved (2026-11-26 scale-race fix).  Curves that fit in ABSOLUTE
    /// vol space (GVV: eval divides by the live anchor) are anchor-invariant
    /// and keep the default no-op.  Curves that store NORMALIZED multipliers
    /// (Linear: y = IV/anchor_at_fit; Power: a) must scale them by
    /// old_anchor/new_anchor so the absolute vol they encode stays unchanged.
    /// factor = oldAnchor / newAnchor.
    virtual void rescale(double /*factor*/) {}
};

// Convenience alias used by concrete configs.
template<typename D> class VCConfigurable
{
protected:
    template<class T> class Config : public D::ConfigBase
    {
    };
};

// B2-2.2 convergence: canonical curve-type whitelists (single source).
// 2026-11-26 design decision (user-approved): the curve pair is chosen by
// CONFIG, never by runtime fallback —
//   curve1 (main):  "gvv" | "power"   (weight=1 pricing / 0<w<1 blend)
//   curve2:         "linear"          (weight=0 pricing)
// "constant" is retired (inaccurate as a smile; was the silent fallback of
// the removed fallback chain) and is no longer an accepted type anywhere.
inline bool isValidMainCurveType(const std::string& t) {
    return t == "gvv" || t == "power";
}
inline bool isValidCurve2Type(const std::string& t) {
    return t == "linear";
}

} // namespace wt_option
