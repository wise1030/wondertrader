/*!
 * \file OptionPricer.h
 * \brief Theoretical option pricer v1 (migrated from quantbox)
 *
 * Original: longbeach::optioncore::OptionPricer inherited OptionPricerImp and
 * used QuantLib (VanillaOption, AnalyticEuropeanEngine, Business252 day count,
 * impliedVolatility solver). It also depended on ClockMonitor, CommandServices,
 * ClientContext, OptionRisk, OptionGrid (not yet migrated), and BSplineVolCurve.
 *
 * Migration:
 *  - namespace -> wt_option
 *  - QuantLib -> wt_option::BlackCalc / BlackImpliedCalculator (already migrated)
 *  - OptionPricerImp inlined (trace level, values index, panic stubs)
 *  - ClockMonitor -> TimeUtils::getLocalTimeNow()
 *  - CommandServices / ClientContext -> removed
 *  - expiry_t -> uint32_t
 *  - boost::shared_ptr -> std::shared_ptr, boost::tuple -> std::tuple
 *  - OptionRisk / OptionGrid kept as forward-declared dependencies (not yet
 *    migrated; this file will not compile until they land, per task spec)
 */
#pragma once

#include "IOptionPricer.h"
#include "IVolCurve.h"

#include <map>
#include <memory>
#include <string>
#include <cstdint>

namespace wt_option {

class OptionGrid;
class ExpiryData;
class OptionRisk;
using OptionRiskPtr = std::shared_ptr<OptionRisk>;

// ---------------------------------------------------------------------------
// OptionPricerImp — minimal base (inlined from original OptionPricerImp.h)
// ---------------------------------------------------------------------------
class OptionPricerImp : public IOptionPricer
{
public:
    OptionPricerImp() : m_traceLevel(0), m_valuesIndex(0) {}

    void    setTraceLevel(int32_t i) override { m_traceLevel = i; }
    int32_t getTraceLevel() const override { return m_traceLevel; }

    bool isPanicked() const override { return false; }

    void    setValuesIndex(int32_t i) { m_valuesIndex = i; }
    int32_t getValuesIndex() const { return m_valuesIndex; }

protected:
    int32_t m_traceLevel;
    int32_t m_valuesIndex;
};

// ---------------------------------------------------------------------------
// OptionPricer
// ---------------------------------------------------------------------------
class OptionPricer : public OptionPricerImp
{
public:
    struct Config : public IOptionPricer::ConfigBase
    {
        Config()
            : volcurve_weight(0.5)
            , atm_forward_range(0.1)
            , bid_offer_px_spread_floor(1)
            , bid_offer_vol_spread_cap(0.1)
            , max_order_size(10)
        {}

        IVolCurve::ConfigBasePtr volcurve;
        IVolCurve::ConfigBasePtr volcurve2;
        double volcurve_weight;

        double  atm_forward_range;
        double  bid_offer_px_spread_floor;
        double  bid_offer_vol_spread_cap;
        uint32_t max_order_size;
    };
    using ConfigPtr = std::shared_ptr<Config>;

    class ExpiryInfo
    {
    public:
        void computeForwardPrice(OptionGrid* grid, ExpiryData* ed, double atm_forward_range);
        void computeMaturity(ExpiryData* ed);

        uint32_t   m_expiry = 0;
        IVolCurvePtr m_spVolCurve;
        IVolCurvePtr m_spVolCurve2;

        ExpiryData* m_spExpiryData = nullptr;
        double m_atmforward = NAN;
        double m_atmvol = 0;
        double m_maturity = 0;

        double m_futsprd = 0;
        double m_atmvolsprd = 0;
    };
    using ExpiryInfoPtr = std::shared_ptr<ExpiryInfo>;

public:
    OptionPricer(const Config& c, OptionRiskPtr positions);
    virtual ~OptionPricer();

    ExpiryInfoPtr  getExpiryInfo(uint32_t exp) const;
    virtual IVolCurvePtr getVolCurve(uint32_t exp) const;
    virtual IVolCurvePtr getVolCurve2(uint32_t exp) const;
    virtual IVolCurvePtr getFwdCurve(uint32_t exp) const { return nullptr; }

    virtual double getMaturity(uint32_t exp) const;
    virtual double getATMForward(uint32_t exp) const;

    virtual void   setATMVol(uint32_t exp, double atmvol);
    virtual double getATMVol(uint32_t exp) const;

    virtual double getFutSprd(uint32_t exp) const;
    virtual double getATMVolSprd(uint32_t exp) const;
    virtual void   setReprice(bool bReprice) { m_bReprice = bReprice; }

    virtual bool computeValues(OptionGrid* grid);
    virtual bool computeImpliedValues(OptionGrid* grid);

    virtual bool initValuesCompute(OptionGrid* grid);
    virtual void computeValue(OptionData* option);
    virtual void finalizeCompute(OptionGrid* grid);

private:
    const Config& config() const { return m_config; }

    void computeTheoreticalValues(OptionData* option, double forward,
                                  double maturity, double discount);
    void computeImpliedValues(OptionData* option, double forward,
                              double maturity, double discount);

private:
    Config m_config;

    mutable std::map<uint32_t, ExpiryInfoPtr> m_expiryInfoTable;
    OptionRiskPtr m_spPositionRisk;
    double m_defaultVolatility;

    bool m_bReprice;
};
using OptionPricerPtr = std::shared_ptr<OptionPricer>;

} // namespace wt_option
