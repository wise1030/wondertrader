#include "OptionConfigLoader.h"

#include "fmtlib.h"

#include <cmath>
#include <sstream>

namespace wt_option {

static void setEff(OptionConfigLoader::Result& r, const std::string& key,
                   const std::string& value) {
    r.effective[key] = value;
}

static std::string num(double v) {
    std::ostringstream oss;
    oss.precision(10);
    oss << v;
    return oss.str();
}

OptionConfigLoader::Result OptionConfigLoader::load(wtp::WTSVariant* cfg) {
    Result r;
    if (!cfg) {
        r.ok = false;
        r.errors.push_back("config root is null");
        return r;
    }

    // Required identity fields.
    const char* exchange = cfg->has("exchange") ? cfg->getCString("exchange") : "";
    const char* futuresProduct = cfg->has("futuresProduct") ? cfg->getCString("futuresProduct") : "";
    if (!exchange || !*exchange)
        r.errors.push_back("required key 'exchange' is missing");
    if (!futuresProduct || !*futuresProduct)
        r.errors.push_back("required key 'futuresProduct' is missing");
    setEff(r, "exchange", exchange ? exchange : "");
    setEff(r, "futuresProduct", futuresProduct ? futuresProduct : "");

    // riskFreeRate: explicit zero is valid; absence means default 3%.
    if (cfg->has("riskFreeRate")) {
        r.values.riskFreeRate = cfg->getDouble("riskFreeRate");
        r.values.riskFreeRateConfigured = true;
        if (!std::isfinite(r.values.riskFreeRate) || r.values.riskFreeRate < 0) {
            r.errors.push_back("riskFreeRate must be a finite non-negative number");
        }
    } else {
        r.values.riskFreeRate = 0.03;
        r.warnings.push_back("riskFreeRate not configured; using default 0.03");
    }
    setEff(r, "riskFreeRate", num(r.values.riskFreeRate));

    if (cfg->has("maxTPS")) {
        r.values.maxTPS = cfg->getInt32("maxTPS");
        if (r.values.maxTPS < 0)
            r.errors.push_back("maxTPS must be >= 0");
        if (r.values.maxTPS == 0) {
            r.values.maxTPS = 50;
            r.warnings.push_back("maxTPS=0 means default; effective maxTPS=50");
        }
    }
    setEff(r, "maxTPS", std::to_string(r.values.maxTPS));

    if (cfg->has("minComputeInterval")) {
        r.values.minComputeInterval = cfg->getDouble("minComputeInterval");
        if (!std::isfinite(r.values.minComputeInterval) || r.values.minComputeInterval < 0)
            r.errors.push_back("minComputeInterval must be >= 0");
    }
    setEff(r, "minComputeInterval", num(r.values.minComputeInterval));

    if (cfg->has("marketFreshTimeoutMs")) {
        const double v = cfg->getDouble("marketFreshTimeoutMs");
        if (!std::isfinite(v) || v < 0)
            r.errors.push_back("marketFreshTimeoutMs must be >= 0");
        else
            r.values.marketFreshTimeoutMs = static_cast<uint64_t>(v);
    }
    setEff(r, "marketFreshTimeoutMs", std::to_string(r.values.marketFreshTimeoutMs));

    wtp::WTSVariant* pricer = cfg->get("pricer");
    if (pricer && pricer->has("slow_compute_interval")) {
        r.values.slowComputeInterval = pricer->getDouble("slow_compute_interval");
        if (!std::isfinite(r.values.slowComputeInterval) || r.values.slowComputeInterval < 0)
            r.errors.push_back("pricer.slow_compute_interval must be >= 0");
    }
    setEff(r, "pricer.slow_compute_interval", num(r.values.slowComputeInterval));

    // B12 deprecation cycle: retain the old vega-price keys as a fallback and
    // keep them in schema, but make every legacy configuration visible.
    if (pricer) {
        const std::pair<const char*, const char*> deprecatedVega[] = {
            {"risk_shift_distort_vega", "risk_shift_distort_vega_vol"},
            {"risk_shift_max_vega", "risk_shift_max_vega_vol"}
        };
        for (const auto& [oldKey, newKey] : deprecatedVega) {
            if (pricer->has(oldKey))
                r.warnings.push_back(fmt::format(
                    "pricer.{} is deprecated; use {} (old fallback retained this version)",
                    oldKey, newKey));
        }
    }

    // PositionGuard: wire every field, but preserve the conservative defaults.
    // clamp/undo/stale remain opt-in because they alter reconciliation state.
    auto& pgv = r.values.positionGuard;
    wtp::WTSVariant* pg = cfg->get("positionGuard");
    if (pg) {
        if (pg->has("tolerance")) pgv.tolerance = pg->getInt32("tolerance");
        if (pg->has("alertCooldownSec")) pgv.alertCooldownSec = pg->getDouble("alertCooldownSec");
        if (pg->has("disableOnBreach")) pgv.disableOnBreach = pg->getBoolean("disableOnBreach");
        if (pg->has("staleFreezeSec")) pgv.staleFreezeSec = pg->getDouble("staleFreezeSec");
        if (pg->has("clampLimit")) pgv.clampLimit = pg->getInt32("clampLimit");
        if (pg->has("undoWindowSec")) pgv.undoWindowSec = pg->getDouble("undoWindowSec");
    }
    if (pgv.tolerance < 0) r.errors.push_back("positionGuard.tolerance must be >= 0");
    if (!std::isfinite(pgv.alertCooldownSec) || pgv.alertCooldownSec < 0)
        r.errors.push_back("positionGuard.alertCooldownSec must be finite and >= 0");
    if (!std::isfinite(pgv.staleFreezeSec) || pgv.staleFreezeSec < 0)
        r.errors.push_back("positionGuard.staleFreezeSec must be finite and >= 0");
    if (pgv.clampLimit < 0) r.errors.push_back("positionGuard.clampLimit must be >= 0");
    if (!std::isfinite(pgv.undoWindowSec) || pgv.undoWindowSec < 0)
        r.errors.push_back("positionGuard.undoWindowSec must be finite and >= 0");
    setEff(r, "positionGuard.tolerance", std::to_string(pgv.tolerance));
    setEff(r, "positionGuard.alertCooldownSec", num(pgv.alertCooldownSec));
    setEff(r, "positionGuard.disableOnBreach", pgv.disableOnBreach ? "true" : "false");
    setEff(r, "positionGuard.staleFreezeSec", num(pgv.staleFreezeSec));
    setEff(r, "positionGuard.clampLimit", std::to_string(pgv.clampLimit));
    setEff(r, "positionGuard.undoWindowSec", num(pgv.undoWindowSec));

    // Legacy OQM keys that are known to be ignored by the current loader.
    wtp::WTSVariant* om = cfg->get("orderManager");
    auto warnLegacy = [&](wtp::WTSVariant* section, const char* legacy, const char* replacement) {
        if (section && section->has(legacy)) {
            r.warnings.push_back(fmt::format(
                "orderManager.{} is legacy/ignored; use {}", legacy, replacement));
        }
    };
    if (om) {
        warnLegacy(om->get("option"), "cancel_timeout_ms", "cancel_ack_timeout_ms");
        warnLegacy(om->get("option"), "ttl_ms", "time_in_force_ms");
        warnLegacy(om->get("option"), "max_orders_per_code", "max_side_orders");
        warnLegacy(om->get("future"), "cancel_timeout_ms", "cancel_ack_timeout_ms");
        warnLegacy(om->get("future"), "ttl_ms", "time_in_force_ms");
        warnLegacy(om->get("future"), "max_orders_per_code", "max_side_orders");
    }

    // Expiry ranges.
    wtp::WTSVariant* expiries = cfg->get("expiries");
    if (expiries && expiries->isArray()) {
        for (uint32_t i = 0; i < expiries->size(); ++i) {
            wtp::WTSVariant* e = expiries->get(i);
            if (!e) continue;
            const uint32_t expiry = e->getUInt32("expiry");
            const double dmin = e->has("delta_min") ? e->getDouble("delta_min") : 0.05;
            const double dmax = e->has("delta_max") ? e->getDouble("delta_max") : 0.95;
            const int32_t maxQSize = e->has("max_qsize") ? e->getInt32("max_qsize") : 1;
            // P0-3: the grid keys everything by 6-digit YYYYMM parsed from the
            // option code (e.g. 202607).  A 4-digit YYMM (2607) used to load
            // silently and match nothing — the expiry never got enabled and
            // quoting stayed off with no warning.
            if (expiry < 202001 || expiry > 209912)
                r.errors.push_back(fmt::format(
                    "expiries[].expiry must be YYYYMM in [202001, 209912], got {} "
                    "(grid expiry keys are 6-digit, e.g. 202607)", expiry));
            if (!(dmin >= 0.0 && dmin < dmax && dmax <= 1.0))
                r.errors.push_back(fmt::format("expiry {} delta range invalid: [{}, {}]", expiry, dmin, dmax));
            setEff(r, fmt::format("expiries[{}].delta_min", expiry), num(dmin));
            setEff(r, fmt::format("expiries[{}].delta_max", expiry), num(dmax));
            setEff(r, fmt::format("expiries[{}].max_qsize", expiry), std::to_string(maxQSize));
        }
    }

    r.ok = r.errors.empty();
    return r;
}

std::string OptionConfigLoader::dumpEffectiveValues(const Result& result) {
    std::ostringstream oss;
    oss << "effective config values:\n";
    for (const auto& [key, value] : result.effective)
        oss << "  " << key << " = " << value << '\n';
    if (!result.warnings.empty()) {
        oss << "warnings:\n";
        for (const auto& w : result.warnings) oss << "  - " << w << '\n';
    }
    if (!result.errors.empty()) {
        oss << "errors:\n";
        for (const auto& e : result.errors) oss << "  - " << e << '\n';
    }
    return oss.str();
}

} // namespace wt_option
