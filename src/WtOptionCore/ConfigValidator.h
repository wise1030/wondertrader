/*!
 * \file ConfigValidator.h
 * \brief Startup configuration schema validation — closes defect B33
 *        ("配置静默漂移"): keys that the loader never reads used to be
 *        silently ignored (slow_compute_interval etc. differed from intent
 *        by 150×). This walker WARNs on unknown keys per section and can
 *        dump the effective values of critical parameters.
 *
 * The schema is a static name→child-set table; it intentionally does NOT try
 * to be a full type system — its job is catching typos and stale configs.
 */
#pragma once

#include <initializer_list>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "../Includes/WTSVariant.hpp"
#include "../WTSTools/WTSLogger.h"

namespace wt_option {

class ConfigValidator {
public:
    using Keys = std::initializer_list<const char*>;

    static const std::unordered_map<std::string, std::unordered_set<std::string>>& schema() {
        static const std::unordered_map<std::string, std::unordered_set<std::string>> s = {
            { "", { "exchange","futuresProduct","optionProduct","underlyingCode","underlyingType",
                    "riskFreeRate","riskFreeRateCurve","maxTPS","minComputeInterval",
                    "use_parallel","sessionSchedule","pricer","orderManager","expiries",
                    "riskLimits","signals","scanners","optionValueWriter","margin","pnl",
                    "pricerType","wgt_vegaflow","wgt_frontfut_skew","wgt_deltaflow",
                    "wgt_atmsig","wgt_rollema","sticky_base","improve_retreat_ratio",
                    "gammascalp","delta_hedge" } },
            { "pricer", { "wgt_vegaflow","wgt_frontfut_skew","wgt_deltaflow","wgt_atmsig",
                          "wgt_rollema","sticky_base","improve_retreat_ratio",
                          "hedge_ratio_delta","hedge_ratio_vega","lambda_vega_decay",
                          "lambda_vega_wing","alpha","quoting","blackCalc","trace_level",
                          "gammascalp","trade_shock_ticks","trade_shock_interval",
                          "panic_blackout_interval","interesting_ticks_window",
                          "interesting_ticks_vol_multiple","risk_prem_bps","risk_prem_ratio",
                          "rp_quote","rp_future","reduce_large_size","slow_compute_interval",
                          "min_compute_interval" } },
            { "pricer.blackCalc", { "volCurve","risk_free","dividend" } },
            { "pricer.blackCalc.volCurve", { "fitter","volcurve_weight","curve2_type" } },
            { "pricer.blackCalc.volCurve.fitter", { "start_time","end_time","period",
                          "decay_window","threshold","fit_all_expiries","good_points_thresh" } },
            { "orderManager", { "option","future","riskFilters" } },
            { "orderManager.option", { "max_side_orders","max_position","time_in_force_ms",
                          "enable_quote_api","avoid_trade","check_potential_position",
                          "leave_outer_orders","max_cancels_allowed","hard_flat_after_n_fills",
                          "quote_style","enable_offset_guard","wait_for_cancels",
                          "cancel_ack_timeout_ms","cancel_max_retries","order_delay_ms",
                          "cancel_buffer","cancel_soft_max","reject_max_new_orders",
                          "min_intra_update_period_ms","cautious_flipping","scale_factor",
                          "enable_ioc","right_flag" } },
            { "orderManager.future", { "max_side_orders","max_position","time_in_force_ms",
                          "enable_quote_api","avoid_trade","check_potential_position",
                          "quote_style","enable_offset_guard","wait_for_cancels",
                          "cancel_ack_timeout_ms","cancel_max_retries","order_delay_ms",
                          "cancel_buffer","cancel_soft_max","reject_max_new_orders",
                          "min_intra_update_period_ms","cautious_flipping","scale_factor",
                          "enable_ioc","right_flag" } },
            { "orderManager.riskFilters", { "enabled","max_order_size","max_order_size_reject",
                          "min_sell_price","max_position","max_position_mode",
                          "max_cancel_soft","max_cancel_hard","max_new_orders_hard_flat",
                          "max_new_orders_reject","option","future" } },
            { "margin", { "enable","fut_rate","opt_short_rate","max_margin",
                          "warn_ratio","check_period_sec" } },
            { "pnl", { "fifo_mode" } },
            { "sessionSchedule", { "stopLeadSeconds","resumeLagSeconds" } },
            { "riskLimits", { "maxOrderSize","maxOrderValue","clearlyErroneousPercent",
                          "minSellPrice","maxPositionPerOption","maxTotalPosition",
                          "maxDelta","maxGamma","maxVega","maxLossPerDay",
                          "maxShortCallPerSymbol","maxShortPutPerSymbol",
                          "maxStressLoss" } },
            { "optionValueWriter", { "enable","outputDir","startTime","endTime","outputPeriod" } },
            { "delta_hedge", { "enable","threshold_delta","target_delta","max_order_size",
                               "cooldown_sec","delta_per_lot","minHedgeQty" } },
            { "expiries[]", { "expiry","underlyingCode","hedgeCode","secondaryHedgeCodes",
                              "delta_min","delta_max","sprd_fwd","sprd_atmvol","sprd_corr",
                              "max_pos_fut","max_pos_stk","max_pos_opt","max_qsize",
                              "enable_auto_close","close_pos_thresh","include_future",
                              "min_strikes_for_synthetic" } },
            { "orderManager.riskFilters.option", { "enabled","max_order_size","max_order_size_reject",
                              "min_sell_price","max_position","max_position_mode",
                              "max_cancel_soft","max_cancel_hard","max_new_orders_hard_flat",
                              "max_new_orders_reject" } },
            { "orderManager.riskFilters.future", { "enabled","max_order_size","max_order_size_reject",
                              "min_sell_price","max_position","max_position_mode",
                              "max_cancel_soft","max_cancel_hard","max_new_orders_hard_flat",
                              "max_new_orders_reject" } },
        };
        return s;
    }

    /// Recursively validate `cfg`. `path` is the section path ("" = root).
    /// Arrays are validated element-wise for known array-of-object sections.
    static void validate(wtp::WTSVariant* cfg, const std::string& path = "", int depth = 0) {
        if (!cfg || depth > 8) return;
        if (cfg->type() != wtp::WTSVariant::VT_Object &&
            cfg->type() != wtp::WTSVariant::VT_Array)
            return;

        const auto& sch = schema();
        auto sit = sch.find(path);

        if (cfg->type() == wtp::WTSVariant::VT_Object) {
            // Generic walk: WTSVariant::memberNames() is available in this tree.
            // Skip sections whose keys are intentionally free-form (signal /
            // scanner plugin config) to avoid false positives.
            const bool dynamic_section = (path == "signals" || path == "scanners");
            for (const auto& name : cfg->memberNames()) {
                if (sit != sch.end() && !dynamic_section && sit->second.count(name) == 0) {
                    WTSLogger::log_by_cat("strategy", LL_WARN,
                        "ConfigValidator: UNKNOWN key '{}' in section '{}' (ignored by loader?)",
                        name, path.empty() ? "<root>" : path);
                }
                wtp::WTSVariant* child = cfg->get(name.c_str());
                if (!child) continue;
                std::string child_path = path.empty() ? name : path + "." + name;
                if (child->type() == wtp::WTSVariant::VT_Object) {
                    validate(child, child_path, depth + 1);
                } else if (child->type() == wtp::WTSVariant::VT_Array) {
                    for (uint32_t i = 0; i < child->size(); ++i) {
                        wtp::WTSVariant* elem = child->get(i);
                        if (elem && elem->type() == wtp::WTSVariant::VT_Object)
                            validate(elem, child_path + "[]", depth + 1);
                    }
                }
            }
        }
    }

private:
    static void validateProbed(wtp::WTSVariant* cfg, const std::string& path) {
        static const char* kAllKeys[] = {
            "exchange","futuresProduct","optionProduct","underlyingCode","underlyingType",
            "riskFreeRate","riskFreeRateCurve","maxTPS","minComputeInterval","use_parallel",
            "sessionSchedule","pricer","orderManager","expiries","riskLimits","signals",
            "scanners","optionValueWriter","margin","pnl","pricerType",
            "wgt_vegaflow","wgt_frontfut_skew","wgt_deltaflow","wgt_atmsig","wgt_rollema",
            "sticky_base","improve_retreat_ratio","gammascalp","trade_shock_ticks",
            "slow_compute_interval","trade_shock_interval","panic_blackout_interval",
            "interesting_ticks_window","interesting_ticks_vol_multiple","risk_prem_bps",
            "risk_prem_ratio","rp_quote","rp_future","reduce_large_size",
            "hedge_ratio_delta","hedge_ratio_vega","lambda_vega_decay","lambda_vega_wing",
            "alpha","quoting","blackCalc","trace_level",
            "volCurve","volcurve_weight","curve2_type","fitter","risk_free","dividend",
            "start_time","end_time","period","decay_window","threshold","fit_all_expiries",
            "good_points_thresh","option","future","riskFilters",
            "max_side_orders","max_position","time_in_force_ms","enable_quote_api",
            "avoid_trade","check_potential_position","leave_outer_orders",
            "max_cancels_allowed","hard_flat_after_n_fills","quote_style","enable_offset_guard",
            "enabled","max_order_size","max_order_size_reject","min_sell_price",
            "max_position_mode","max_cancel_soft","max_cancel_hard",
            "max_new_orders_hard_flat","max_new_orders_reject",
            "enable","fut_rate","opt_short_rate","max_margin","warn_ratio","check_period_sec",
            "fifo_mode","stopLeadSeconds","resumeLagSeconds",
            "maxOrderSize","maxOrderValue","clearlyErroneousPercent","minSellPrice",
            "maxPositionPerOption","maxTotalPosition","maxDelta","maxGamma","maxVega",
            "maxLossPerDay","maxShortCallPerSymbol","maxShortPutPerSymbol",
            "outputDir","startTime","endTime","outputPeriod"
        };
        const auto& sch = schema();
        auto sit = sch.find(path);

        for (const char* k : kAllKeys) {
            if (!cfg->has(k)) continue;
            bool known = (sit != sch.end()) && (sit->second.count(k) != 0);
            if (!known)
                WTSLogger::log_by_cat("strategy", LL_WARN,
                    "ConfigValidator: UNKNOWN key '{}' in section '{}' (ignored by loader?)",
                    k, path.empty() ? "<root>" : path);
        }

        // Recurse into known object sections
        static const char* kSections[] = { "pricer","blackCalc","volCurve","fitter",
                                           "orderManager","riskFilters","margin","pnl",
                                           "sessionSchedule","riskLimits","optionValueWriter" };
        for (const char* sec : kSections) {
            if (!cfg->has(sec)) continue;
            wtp::WTSVariant* child = cfg->get(sec);
            if (child && !child->isArray()) {
                std::string sub = path.empty() ? sec : path + "." + sec;
                validate(child, sub, 1);
            }
        }
        // orderManager.option / future / riskFilters.{option,future}
        for (const char* sec : { "option", "future" }) {
            if (!cfg->has(sec)) continue;
            wtp::WTSVariant* child = cfg->get(sec);
            if (child && !child->isArray()) {
                std::string sub = path.empty() ? sec : path + "." + sec;
                if (sub == "orderManager.option" || sub == "orderManager.future")
                    validate(child, sub, 1);
            }
        }
    }
};

} // namespace wt_option
