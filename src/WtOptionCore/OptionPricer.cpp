/*!
 * \file OptionPricer.cpp
 * \brief OptionPricer implementation (migrated from quantbox)
 *
 * Original used QuantLib (VanillaOption, AnalyticEuropeanEngine, Business252,
 * impliedVolatility solver). Migration rewrites the QL calls using the already-
 * migrated wt_option::BlackCalc (analytic European) and BlackImpliedCalculator
 * (implied vol via Brent). Business logic (forward-price computation from
 * call-put parity, vol-curve weighting, maturity via Business252 equivalent,
 * decay) is preserved.
 *
 * Dependencies not yet migrated (OptionGrid, OptionRisk) are forward-declared;
 * methods that need them are stubbed with TODO markers. This file will not
 * link until those components land.
 */
#include "OptionPricer.h"
#include "ExpiryData.h"
#include "OptionData.h"
#include "StrikeData.h"
#include "BlackCalc.h"
#include "BlackImpliedCalculator.h"

#include "LinearVolCurve.h"
// #include "GvvVolCurve.h"  // TODO: needs GSL
#include "ConstantVolCurve.h"

#include "../WTSTools/WTSLogger.h"

#include <limits>
#include <cmath>
#include <iostream>
#include <numeric>

namespace wt_option {

// ---------------------------------------------------------------------------
// ExpiryInfo
// ---------------------------------------------------------------------------
void OptionPricer::ExpiryInfo::computeForwardPrice(
    OptionGrid* grid, ExpiryData* ed, double atm_forward_range)
{
    // TODO[OptionGrid migration]: the original iterated grid->iterExpiry(),
    // matched call/put pairs near ATM, and computed implied forward + spread
    // via put-call parity. This requires OptionGrid's iteration API which is
    // not yet migrated. For now we leave m_atmforward/m_futsprd at defaults;
    // OptionPricer2's ExpiryInfo::computeForwardPrice has the same logic and
    // will be the canonical implementation once the grid lands.
    (void)grid; (void)ed; (void)atm_forward_range;
}

void OptionPricer::ExpiryInfo::computeMaturity(ExpiryData* ed)
{
    // Original: m_maturity = (ed->getIntradayFraction()
    //   + Business252.dayCount(curDate, expirationDate)) / 252;
    // The migrated ExpiryData already folds intraday fraction + business days
    // into getMaturity(), so we delegate.
    if (ed)
        m_maturity = ed->getMaturity();
}

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------
OptionPricer::OptionPricer(const Config& c, OptionRiskPtr positions)
    : m_config(c)
    , m_spPositionRisk(positions)
    , m_defaultVolatility(0.1)
    , m_bReprice(true)
{
    WTSLogger::log_by_cat("strategy", LL_INFO, "OptionPricer created");
}

OptionPricer::~OptionPricer() {}

// ---------------------------------------------------------------------------
// Expiry info accessors
// ---------------------------------------------------------------------------
OptionPricer::ExpiryInfoPtr OptionPricer::getExpiryInfo(uint32_t exp) const
{
    ExpiryInfoPtr& ei = m_expiryInfoTable[exp];
    if (!ei)
    {
        ei = std::make_shared<ExpiryInfo>();
        ei->m_atmforward = NAN;
        ei->m_atmvol = m_defaultVolatility;
        ei->m_expiry = exp;
        // vol curves: original created BSplineVolCurve / LinearVolCurve from
        // config. Config-based factory was removed in migration; default to
        // LinearVolCurve until the engine wires concrete curves.
        ei->m_spVolCurve  = std::make_shared<LinearVolCurve>();
        ei->m_spVolCurve2 = std::make_shared<LinearVolCurve>();
    }
    return ei;
}

IVolCurvePtr OptionPricer::getVolCurve(uint32_t exp) const  { return getExpiryInfo(exp)->m_spVolCurve; }
IVolCurvePtr OptionPricer::getVolCurve2(uint32_t exp) const { return getExpiryInfo(exp)->m_spVolCurve2; }

double OptionPricer::getMaturity(uint32_t exp) const   { return getExpiryInfo(exp)->m_maturity; }
void   OptionPricer::setATMVol(uint32_t exp, double v) { getExpiryInfo(exp)->m_atmvol = v; }
double OptionPricer::getATMVol(uint32_t exp) const     { return getExpiryInfo(exp)->m_atmvol; }
double OptionPricer::getATMForward(uint32_t exp) const { return getExpiryInfo(exp)->m_atmforward; }
double OptionPricer::getFutSprd(uint32_t exp) const    { return getExpiryInfo(exp)->m_futsprd; }
double OptionPricer::getATMVolSprd(uint32_t exp) const { return getExpiryInfo(exp)->m_atmvolsprd; }

// ---------------------------------------------------------------------------
// computeValues / initValuesCompute / computeValue / finalizeCompute
// ---------------------------------------------------------------------------
bool OptionPricer::computeValues(OptionGrid* grid)
{
    if (initValuesCompute(grid))
    {
        // TODO[OptionGrid migration]: iterate grid->getAllOptions() and call
        // computeValue on each. The grid's option-by-instrument index is not
        // yet migrated.
        finalizeCompute(grid);
        return false;
    }
    return true;
}

bool OptionPricer::computeImpliedValues(OptionGrid* grid)
{
    // TODO[OptionGrid migration]: iterate options, setup black calc info,
    // call computeImpliedValues(option, ...).
    (void)grid;
    return true;
}

bool OptionPricer::initValuesCompute(OptionGrid* grid)
{
    // TODO[OptionGrid migration]: original read grid->getUnderlyingPxP()-
    // >getRefPrice(), built a QuantLib ComputeState (calendar, day counter,
    // flat vol TS), then for each expiry computed forward price + maturity.
    // The migrated version delegates maturity to ExpiryData::getMaturity()
    // (which uses IBaseDataMgr business-day count) and forward price to
    // ExpiryInfo::computeForwardPrice (stubbed until grid lands).
    (void)grid;
    return true;
}

void OptionPricer::computeValue(OptionData* option)
{
    if (!option) return;

    OptionValues& values = option->values(0); // default bin-0

    uint32_t exp = option->getExpiry();
    double atmvol = getExpiryInfo(exp)->m_atmvol;
    IVolCurvePtr spVolCurve  = getVolCurve(exp);
    IVolCurvePtr spVolCurve2 = getVolCurve2(exp);
    double weight = std::max(0.0, std::min(1.0, config().volcurve_weight));
    double volMultiplier = 1.0;
    if (spVolCurve && spVolCurve2)
    {
        double atmforward = getATMForward(exp);
        volMultiplier = (*spVolCurve)(*option, atmforward) * weight
                      + (*spVolCurve2)(*option, atmforward) * (1.0 - weight);
    }

    double volsprd = getExpiryInfo(exp)->m_atmvolsprd;

    values.m_theoVol = volMultiplier * atmvol;
    values.m_theoVolSprd = std::max(0.0, volsprd);
    values.m_theoBidVol = std::max(0.0, values.m_theoVol - 0.5 * values.m_theoVolSprd);
    values.m_theoAskVol = values.m_theoVol + 0.5 * values.m_theoVolSprd;

    // Theoretical values via BlackCalc (replaces QuantLib VanillaOption).
    auto ed = option->getExpiryData();
    if (!ed) return;
    double forward   = getATMForward(exp);
    double maturity  = getMaturity(exp);
    double discount  = ed->getDiscountFactor();
    computeTheoreticalValues(option, forward, maturity, discount);
}

void OptionPricer::computeTheoreticalValues(
    OptionData* option, double forward, double maturity, double discount)
{
    OptionValues& values = option->values(0);
    values.setPriced(true);

    if (std::isnan(forward) || forward <= 0 || maturity < 0)
    {
        values.setTheo(0.0);
        values.setPriced(false);
        return;
    }

    OptionType ot = (option->getRight() == OR_Call) ? OT_Call : OT_Put;
    BlackCalc bc(ot, option->getStrike(), forward,
                 values.m_theoVol * std::sqrt(maturity), discount);
    values.setForward(forward);
    values.setTheo(std::max(0.0, bc.value()));

    values.greeks().delta() = bc.delta();
    values.greeks().gamma() = bc.gamma();
    values.greeks().vega()  = bc.vega(maturity) / 100.0;   // w.r.t one vol point
    values.greeks().theta() = bc.thetaPerDay(0.0, values.m_theoVol);
}

void OptionPricer::computeImpliedValues(
    OptionData* option, double forward, double maturity, double discount)
{
    OptionValues& values = option->values(0);

    if (option->getBid() <= 0 || option->getAsk() <= 0)
    {
        values.m_impliedVol = -1;
        values.m_impliedBidVol = -1;
        values.m_impliedAskVol = -1;
        values.m_impliedVolSprd = 0;
        return;
    }

    double bid = option->getBid();
    double mid = option->getMid();
    double ask = option->getAsk();

    double iv_bid = -1, iv_mid = -1, iv_ask = -1;

    if (bid > 0)
    {
        try {
            OptionType ot = (option->getRight() == OR_Call) ? OT_Call : OT_Put;
            BlackImpliedCalculator bic(ot, option->getStrike(),
                                       forward, maturity, discount);
            iv_bid = bic.volatility(bid);
            iv_mid = bic.volatility(mid);
            iv_ask = bic.volatility(ask);

            if (iv_ask - iv_bid >= config().bid_offer_vol_spread_cap)
            {
                iv_bid = -2; iv_mid = -2; iv_ask = -2;
            }
        }
        catch (...) {
            iv_bid = -3; iv_mid = -3; iv_ask = -3;
        }
    }

    values.m_impliedVol = iv_mid;
    values.m_impliedBidVol = iv_bid;
    values.m_impliedAskVol = iv_ask;
    values.m_impliedVolSprd = iv_ask - iv_bid;
}

void OptionPricer::finalizeCompute(OptionGrid* grid)
{
    // TODO[OptionGrid migration]: iterate grid->getAllOptions() and call
    // od->notifyMarketsPriced(0) on each.
    (void)grid;
}

} // namespace wt_option
