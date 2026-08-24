#include "AlphaSignals.h"
#include "SignalFactory.h"
#include "../Includes/WTSVariant.hpp"
#include "../OptionData.h"
#include "../ExpiryData.h"
#include "../OptionGrid.h"
#include <cmath>

USING_NS_WTP;

namespace wt_option {

static inline double mat_adjusted(const OptionData* od) {
    if (!od) return 1.0;
    auto ed = od->getExpiryData();
    double mat = ed ? ed->getMaturity() : 0.25;
    return std::max(0.01, mat) / 0.25;
}

// ============================================================================
// VegaFlowSignal
// ============================================================================
bool VegaFlowSignal::init(WTSVariant* cfg) {
    if (!cfg) return true;
    m_weight = cfg->getDouble("weight");
    m_window = cfg->has("window_sec") ? cfg->getDouble("window_sec") : 120.0;
    m_ema.setWindow(m_window);
    return true;
}

void VegaFlowSignal::onTradeTick(const std::string& code, double price, double size,
                                  const OptionData* od) {
    (void)code;
    if (!m_enabled || !od || price <= 0 || size <= 0) return;
    const OptionValues& v = od->values(0);
    if (!v.isPriced()) return;
    double best_bid = od->getBid(), best_ask = od->getAsk();
    if (best_bid <= 0 || best_ask <= 0) return;
    double theo = std::min(std::max(v.theo(), best_bid), best_ask);
    double s = 0.0;
    if (price > theo + 1e-9) s = 1.0;
    else if (price < theo - 1e-9) s = -1.0;
    double delta_val = std::fabs(v.greeks().delta());
    if (delta_val < 0.2 || delta_val > 0.8) return;
    auto ed = od->getExpiryData();
    double maturity = ed ? ed->getMaturity() : 0.25;
    double ve = s * std::sqrt(size) * v.greeks().vega() * maturity;
    m_ema.update(m_signalTime, ve);  // B19 fix: was update(0,...) — decay never applied
}

double VegaFlowSignal::getVegaAdjust(const OptionData* od, const SignalContext& ctx) const {
    (void)ctx;
    if (!m_enabled) return 0;
    if (!m_ema.isOK()) return 0;
    return m_ema.getSum() / std::sqrt(mat_adjusted(od));
}

// ============================================================================
// DeltaFlowSignal
// ============================================================================
bool DeltaFlowSignal::init(WTSVariant* cfg) {
    if (!cfg) return true;
    m_weight = cfg->getDouble("weight");
    m_window = cfg->has("window_sec") ? cfg->getDouble("window_sec") : 120.0;
    m_ema.setWindow(m_window);
    return true;
}

void DeltaFlowSignal::onTradeTick(const std::string& code, double price, double size,
                                   const OptionData* od) {
    (void)code;
    if (!m_enabled || !od || price <= 0 || size <= 0) return;
    const OptionValues& v = od->values(0);
    if (!v.isPriced()) return;
    double best_bid = od->getBid(), best_ask = od->getAsk();
    if (best_bid <= 0 || best_ask <= 0) return;
    double theo = std::min(std::max(v.theo(), best_bid), best_ask);
    double s = 0.0;
    if (price > theo + 1e-9) s = 1.0;
    else if (price < theo - 1e-9) s = -1.0;
    double delta_val = std::fabs(v.greeks().delta());
    if (delta_val < 0.2 || delta_val > 0.8) return;
    auto ed = od->getExpiryData();
    double maturity = ed ? ed->getMaturity() : 0.25;
    double de = s * std::sqrt(size) * v.greeks().delta() / maturity;
    m_ema.update(m_signalTime, de);  // B19 fix: was update(0,...) — decay never applied
}

double DeltaFlowSignal::getDeltaAdjust(const OptionData* od, const SignalContext& ctx) const {
    (void)ctx;
    if (!m_enabled) return 0;
    if (!m_ema.isOK()) return 0;
    return m_ema.getSum() * 1e-4;
}

// ============================================================================
// AtmSigSignal
// ============================================================================
bool AtmSigSignal::init(WTSVariant* cfg) {
    if (!cfg) return true;
    m_weight = cfg->getDouble("weight");
    return true;
}

double AtmSigSignal::getDeltaAdjust(const OptionData* od, const SignalContext& ctx) const {
    (void)od;
    if (!m_enabled || !ctx.grid) return 0;
    double atmsig = ctx.grid->getAtmSig();
    return atmsig * ctx.frontForward * 1e-4;
}

// ============================================================================
// RollEmaSignal
// ============================================================================
bool RollEmaSignal::init(WTSVariant* cfg) {
    if (!cfg) return true;
    m_weight = cfg->getDouble("weight");
    m_window = cfg->has("window_sec") ? cfg->getDouble("window_sec") : 120.0;
    m_ema.setWindow(m_window);
    return true;
}

void RollEmaSignal::onBatchStart(SignalContext& ctx) {
    if (!m_enabled) return;
    if (ctx.underlyingPrice > 0 && !std::isnan(ctx.frontForward) && ctx.frontForward > 0) {
        double roll = ctx.underlyingPrice - ctx.frontForward;
        m_ema.update(ctx.time, roll);
        m_rollema = m_ema.isOK() ? (roll - m_ema.getMean()) : 0.0;
    }
}

double RollEmaSignal::getDeltaAdjust(const OptionData* od, const SignalContext& ctx) const {
    (void)od; (void)ctx;
    if (!m_enabled) return 0;
    return m_rollema;
}

// ============================================================================
// FrontFutSkewSignal
// ============================================================================
bool FrontFutSkewSignal::init(WTSVariant* cfg) {
    if (!cfg) return true;
    m_weight = cfg->getDouble("weight");
    m_window = cfg->has("window_sec") ? cfg->getDouble("window_sec") : 120.0;
    m_ema.setWindow(m_window);
    return true;
}

void FrontFutSkewSignal::onBatchStart(SignalContext& ctx) {
    if (!m_enabled || ctx.underlyingPrice <= 0) return;
    m_ema.update(ctx.time, ctx.underlyingPrice);
    m_skew = (m_ema.isOK() && ctx.underlyingPrice > 0)
        ? (ctx.underlyingPrice - m_ema.getMean()) / ctx.underlyingPrice * 1e4
        : 0.0;
}

double FrontFutSkewSignal::getVegaAdjust(const OptionData* od, const SignalContext& ctx) const {
    (void)ctx;
    if (!m_enabled) return 0;
    return m_skew / std::sqrt(mat_adjusted(od));
}

// ============================================================================
// FrontAtmvFlowSignal
// ============================================================================
bool FrontAtmvFlowSignal::init(WTSVariant* cfg) {
    if (!cfg) return true;
    m_weight = cfg->getDouble("weight");
    m_window = cfg->has("window_sec") ? cfg->getDouble("window_sec") : 120.0;
    m_ema.setWindow(m_window);
    return true;
}

void FrontAtmvFlowSignal::onBatchStart(SignalContext& ctx) {
    if (!m_enabled || ctx.frontAtmVol <= 0) return;
    m_ema.update(ctx.time, ctx.frontAtmVol);
    m_flow = m_ema.isOK() ? (ctx.frontAtmVol - m_ema.getMean()) * 100.0 : 0.0;
}

double FrontAtmvFlowSignal::getVegaAdjust(const OptionData* od, const SignalContext& ctx) const {
    (void)ctx;
    if (!m_enabled) return 0;
    return m_flow / std::sqrt(mat_adjusted(od));
}

// ============================================================================
// ForwardSpreadSignal - syntheticForward vs underlyingPrice spread signal
// ============================================================================
bool ForwardSpreadSignal::init(WTSVariant* cfg) {
    if (!cfg) return true;
    m_weight = cfg->getDouble("weight");
    return true;
}

double ForwardSpreadSignal::getDeltaAdjust(const OptionData* od, const SignalContext& ctx) const {
    (void)ctx;
    if (!m_enabled || !od) return 0;
    auto ed = od->getExpiryData();
    if (!ed || !ed->isForwardReady()) return 0;
    auto& ema = ed->emaForwardSpread();
    if (!ema.isOK()) return 0;
    // signal = forwardSpread - EMA(forwardSpread)
    // forwardSpread = syntheticForward - underlyingPrice
    // Positive: option market implies forward above mean -> bullish -> delta positive
    // Negative: option market implies forward below mean -> bearish -> delta negative
    return ed->getForwardSpread() - ema.getMean();
}

// ============================================================================
// Registration
// ============================================================================
REGISTER_ALPHA_SIGNAL("VegaFlow", VegaFlowSignal)
REGISTER_ALPHA_SIGNAL("DeltaFlow", DeltaFlowSignal)
REGISTER_ALPHA_SIGNAL("AtmSig", AtmSigSignal)
REGISTER_ALPHA_SIGNAL("RollEma", RollEmaSignal)
REGISTER_ALPHA_SIGNAL("FrontFutSkew", FrontFutSkewSignal)
REGISTER_ALPHA_SIGNAL("FrontAtmvFlow", FrontAtmvFlowSignal)
REGISTER_ALPHA_SIGNAL("ForwardSpread", ForwardSpreadSignal)

} // namespace wt_option
