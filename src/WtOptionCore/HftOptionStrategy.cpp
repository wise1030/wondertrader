/*!
 * \file HftOptionStrategy.cpp
 * \brief Option MM Strategy - HFT framework implementation
 *
 * Hot-update params via ShareManager.
 * Full chain: on_tick -> grid.onTick(discovery) -> computeValues -> CTG.refresh -> quote/buy/sell
 */
// Standard headers FIRST
#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>
#include <set>
#include "../WTSTools/WTSLogger.h"
#include "../Share/fmtlib.h"
#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/CodeHelper.hpp"

#include "HftOptionStrategy.h"
#include "OptionAsyncEventProcessor.h"
#include "OptionGrid.h"
#include "OptionData.h"
#include "OptionValues.h"
#include "ExpiryData.h"
#include "StrikeData.h"
#include "OptionGrid.h"
#include "OptionTradingGrid.h"
#include "OptionQuoteManager.h"
#include "OptionRisk.h"
#include "OptionRiskData.h"
#include "CompositeOptionPricer.h"
#include "OptionPricer2.h"
#include "ControllableTradingGrid.h"
#include "Scanners/IScanModule.h"
#include "Signals/SignalFactory.h"
#include "Signals/AlphaSignals.h"
#include "Signals/RiskSignals.h"
#include "CffexIOStrikeFinder.h"
#include "DceMOStrikeFinder.h"
#include "TradingState.h"
#include "CommandBus.h"
#include "DeltaHedgeExecutor.h"
#include "PredictionEvaluator.h"
#include "ConfigValidator.h"

#include <algorithm>

namespace wt_option {
// Forward-declared in header, full definition in ControllableTradingGrid.h
}

HftOptionStrategy::HftOptionStrategy(const char* name)
    : HftStrategy(name)
{
}

void HftOptionStrategy::shutdown()
{
    if (_async) _async->stop();
    // Explicit listener deregistration to prevent dangling raw pointers
    if (_grid) {
        if (_risk)  _grid->removeListener(_risk.get());
        if (_otg)   _grid->removeListener(_otg.get());
        if (_compositePricer) _grid->removeListener(_compositePricer.get());
        if (_ctg)   _grid->removeListener(_ctg.get());
    }
}

HftOptionStrategy::~HftOptionStrategy()
{
    shutdown();
}

// ============================================================================
// init — read config
// ============================================================================
bool HftOptionStrategy::init(WTSVariant* cfg)
{
    if (!cfg) return false;

    // Typed loader: high-risk drift keys are validated here and their actual
    // effective values are dumped below.  Unknown/legacy keys that look valid
    // but are ignored are reported as warnings.
    _configLoad = wt_option::OptionConfigLoader::load(cfg);
    for (const auto& err : _configLoad.errors)
        WTSLogger::log_by_cat("strategy", LL_ERROR, "ConfigLoader: {}", err);
    for (const auto& warn : _configLoad.warnings)
        WTSLogger::log_by_cat("strategy", LL_WARN, "ConfigLoader: {}", warn);
    if (!_configLoad.ok) {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "ConfigLoader: invalid config; strategy init aborted");
        return false;
    }

    const auto& cv = _configLoad.values;
    _underlyingCode = cfg->getCString("underlyingCode");
    _futuresProduct = cfg->has("futuresProduct") ? cfg->getCString("futuresProduct") : "";
    _exchange = cfg->getCString("exchange");
    _underlyingType = cfg->has("underlyingType") ? cfg->getCString("underlyingType") : "future";
    _riskFreeRate = cv.riskFreeRate;
    _maxTPS = cv.maxTPS;
    _minComputeInterval = cv.minComputeInterval;
    _marketStaleTimeoutMs = cv.marketFreshTimeoutMs;

    // Derive option product IDs from exchange + futures product
    if (cfg->has("optionProduct")) {
        _optionPids.push_back(cfg->getCString("optionProduct"));
    } else if (!_futuresProduct.empty()) {
        // Auto-derive based on exchange rules
        if (_exchange == "CFFEX") {
            static const std::map<std::string, std::string> CFFEX_MAP = {
                {"IF","IO"}, {"IC","MO"}, {"IH","HO"}, {"T","T"}
            };
            auto it = CFFEX_MAP.find(_futuresProduct);
            _optionPids.push_back((it != CFFEX_MAP.end()) ? it->second : _futuresProduct);
        } else if (_exchange == "CZCE") {
            // CZCE has separate products for call (C suffix) and put (P suffix)
            _optionPids.push_back(_futuresProduct + "C");
            _optionPids.push_back(_futuresProduct + "P");
        } else {
            // SHFE/INE/DCE: futuresProduct + "_o"
            _optionPids.push_back(_futuresProduct + "_o");
        }
    }
    _optionProduct = _optionPids.empty() ? "" : _optionPids[0];

    std::string optPidsStr;
    for (size_t i = 0; i < _optionPids.size(); i++) {
        if (i > 0) optPidsStr += ",";
        optPidsStr += _optionPids[i];
    }
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "HftOptionStrategy config: exchange={} futuresProduct={} optionPids=[{}] underlyingType={} underlyingCode={}",
        _exchange, _futuresProduct, optPidsStr, _underlyingType, _underlyingCode);

    // B2: Risk-free rate curve (optional). If configured, each expiry gets
    // its rate from the curve via linear interpolation; otherwise flat rate.
    if (cfg->has("riskFreeRateCurve")) {
        WTSVariant* curveCfg = cfg->get("riskFreeRateCurve");
        if (curveCfg && curveCfg->isArray()) {
            for (uint32_t i = 0; i < curveCfg->size(); i++) {
                WTSVariant* pt = curveCfg->get(i);
                double days = pt->getDouble("days");
                double rate = pt->getDouble("rate");
                _rateCurve.emplace_back(days, rate);
            }
            // Sort by days (required for interpolation)
            std::sort(_rateCurve.begin(), _rateCurve.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "Rate curve loaded with {} points", _rateCurve.size());
        }
    }

    // B4: Session-based mid-day stop/resume scheduling
    if (cfg->has("sessionSchedule")) {
        WTSVariant* ssCfg = cfg->get("sessionSchedule");
        _stopLeadSeconds = ssCfg->has("stopLeadSeconds") ? ssCfg->getInt32("stopLeadSeconds") : 30;
        _resumeLagSeconds = ssCfg->has("resumeLagSeconds") ? ssCfg->getInt32("resumeLagSeconds") : 0;
    }

    // Alpha weights from config
    _wgt_vegaflow      = cfg->has("wgt_vegaflow") ? cfg->getDouble("wgt_vegaflow") : 0.0;
    _wgt_frontfut_skew = cfg->has("wgt_frontfut_skew") ? cfg->getDouble("wgt_frontfut_skew") : 0.0;
    _wgt_deltaflow     = cfg->has("wgt_deltaflow") ? cfg->getDouble("wgt_deltaflow") : 0.0;
    _wgt_atmsig        = cfg->has("wgt_atmsig") ? cfg->getDouble("wgt_atmsig") : 0.0;
    _wgt_rollema       = cfg->has("wgt_rollema") ? cfg->getDouble("wgt_rollema") : 0.0;
    _sticky_base       = cfg->has("sticky_base") ? cfg->getDouble("sticky_base") : 0.5;
    _improve_retreat   = cfg->has("improve_retreat_ratio") ? cfg->getDouble("improve_retreat_ratio") : 3.0;

    // Read pricer config as nested section (no flat member variables)
    WTSVariant* pracerCfg = cfg->get("pricer");
    if (!pracerCfg) pracerCfg = cfg;  // backward compat: flat config

    // Read OQM configs
    WTSVariant* omCfg = cfg->get("orderManager");
    wt_option::OptionQuoteManager::Config optionOqmCfg;
    wt_option::OptionQuoteManager::Config futureOqmCfg;
    if (omCfg) {
        WTSVariant* optOm = omCfg->get("option");
        if (optOm) {
            optionOqmCfg.max_side_orders = optOm->has("max_side_orders") ? optOm->getUInt32("max_side_orders") : 1;
            optionOqmCfg.max_position = optOm->has("max_position") ? optOm->getUInt32("max_position") : 50;
            optionOqmCfg.time_in_force_ms = optOm->has("time_in_force_ms") ? optOm->getDouble("time_in_force_ms") : 45000;
            optionOqmCfg.enable_quote_api = optOm->has("enable_quote_api") ? optOm->getBoolean("enable_quote_api") : true;
            optionOqmCfg.avoid_trade = optOm->has("avoid_trade") ? optOm->getBoolean("avoid_trade") : false;
            optionOqmCfg.check_potential_position = optOm->has("check_potential_position") ? optOm->getBoolean("check_potential_position") : false;
            optionOqmCfg.leave_outer_orders = optOm->has("leave_outer_orders") ? optOm->getBoolean("leave_outer_orders") : true;
            optionOqmCfg.wait_for_cancels = optOm->has("wait_for_cancels") ? optOm->getBoolean("wait_for_cancels") : false;
            optionOqmCfg.cancel_ack_timeout_ms = optOm->has("cancel_ack_timeout_ms") ? optOm->getDouble("cancel_ack_timeout_ms") : 2000;
            optionOqmCfg.cancel_max_retries = optOm->has("cancel_max_retries") ? optOm->getUInt32("cancel_max_retries") : 3;
            optionOqmCfg.max_cancels_allowed = optOm->has("max_cancels_allowed") ? optOm->getInt32("max_cancels_allowed") : 0;
            optionOqmCfg.hard_flat_after_n_fills = optOm->has("hard_flat_after_n_fills") ? optOm->getInt32("hard_flat_after_n_fills") : 0;
            // A4: exchange quoting style
            {
                std::string qs = optOm->has("quote_style") ? optOm->getCString("quote_style") : "paired";
                optionOqmCfg.quote_style = (qs == "buy_sell")
                    ? wt_option::OptionQuoteManager::Config::QS_BUYSELL
                    : wt_option::OptionQuoteManager::Config::QS_PAIRED;
            }
            // A1: SHFE/INE close-side offset guard
            optionOqmCfg.enable_offset_guard = optOm->has("enable_offset_guard")
                ? optOm->getBoolean("enable_offset_guard") : false;
            // P1-3: keys the schema validates but init() never read — wiring
            // them in so configuration is not silently dropped.
            optionOqmCfg.scale_factor = optOm->has("scale_factor") ? optOm->getDouble("scale_factor") : 1.0;
            optionOqmCfg.min_intra_update_period_ms = optOm->has("min_intra_update_period_ms") ? optOm->getDouble("min_intra_update_period_ms") : 0.0;
            optionOqmCfg.cautious_flipping = optOm->has("cautious_flipping") ? optOm->getBoolean("cautious_flipping") : false;
            optionOqmCfg.cancel_buffer = optOm->has("cancel_buffer") ? optOm->getInt32("cancel_buffer") : 0;
            optionOqmCfg.cancel_soft_max = optOm->has("cancel_soft_max") ? optOm->getInt32("cancel_soft_max") : 0;
            optionOqmCfg.reject_max_new_orders = optOm->has("reject_max_new_orders") ? optOm->getInt32("reject_max_new_orders") : -1;
            optionOqmCfg.enable_ioc = optOm->has("enable_ioc") ? optOm->getBoolean("enable_ioc") : false;
            // order_delay_ms is validated but has no OQM consumer yet — carry
            // the value and warn once instead of dropping it silently.
            optionOqmCfg.order_delay_ms = optOm->has("order_delay_ms") ? optOm->getDouble("order_delay_ms") : 0.0;
            if (optionOqmCfg.order_delay_ms > 0) {
                WTSLogger::log_by_cat("strategy", LL_WARN,
                    "orderManager.option.order_delay_ms={} set but order delay is not implemented yet — value ignored",
                    optionOqmCfg.order_delay_ms);
                optionOqmCfg.order_delay_ms = 0.0;
            }
        }
        WTSVariant* futOm = omCfg->get("future");
        if (futOm) {
            futureOqmCfg.max_side_orders = futOm->has("max_side_orders") ? futOm->getUInt32("max_side_orders") : 1;
            futureOqmCfg.max_position = futOm->has("max_position") ? futOm->getUInt32("max_position") : 10;
            futureOqmCfg.time_in_force_ms = futOm->has("time_in_force_ms") ? futOm->getDouble("time_in_force_ms") : 45000;
            futureOqmCfg.enable_quote_api = futOm->has("enable_quote_api") ? futOm->getBoolean("enable_quote_api") : true;
            futureOqmCfg.check_potential_position = futOm->has("check_potential_position") ? futOm->getBoolean("check_potential_position") : true;
            futureOqmCfg.avoid_trade = futOm->has("avoid_trade") ? futOm->getBoolean("avoid_trade") : false;
            futureOqmCfg.wait_for_cancels = futOm->has("wait_for_cancels") ? futOm->getBoolean("wait_for_cancels") : false;
            futureOqmCfg.cancel_ack_timeout_ms = futOm->has("cancel_ack_timeout_ms") ? futOm->getDouble("cancel_ack_timeout_ms") : 2000;
            futureOqmCfg.cancel_max_retries = futOm->has("cancel_max_retries") ? futOm->getUInt32("cancel_max_retries") : 3;
            std::string qs = futOm->has("quote_style") ? futOm->getCString("quote_style") : "paired";
            futureOqmCfg.quote_style = (qs == "buy_sell")
                ? wt_option::OptionQuoteManager::Config::QS_BUYSELL
                : wt_option::OptionQuoteManager::Config::QS_PAIRED;
            futureOqmCfg.enable_offset_guard = futOm->has("enable_offset_guard")
                ? futOm->getBoolean("enable_offset_guard") : false;
            // P1-3: schema-validated keys previously dropped (see option block)
            futureOqmCfg.scale_factor = futOm->has("scale_factor") ? futOm->getDouble("scale_factor") : 1.0;
            futureOqmCfg.min_intra_update_period_ms = futOm->has("min_intra_update_period_ms") ? futOm->getDouble("min_intra_update_period_ms") : 0.0;
            futureOqmCfg.cautious_flipping = futOm->has("cautious_flipping") ? futOm->getBoolean("cautious_flipping") : false;
            futureOqmCfg.cancel_buffer = futOm->has("cancel_buffer") ? futOm->getInt32("cancel_buffer") : 0;
            futureOqmCfg.cancel_soft_max = futOm->has("cancel_soft_max") ? futOm->getInt32("cancel_soft_max") : 0;
            futureOqmCfg.reject_max_new_orders = futOm->has("reject_max_new_orders") ? futOm->getInt32("reject_max_new_orders") : -1;
            futureOqmCfg.enable_ioc = futOm->has("enable_ioc") ? futOm->getBoolean("enable_ioc") : false;
            futureOqmCfg.order_delay_ms = futOm->has("order_delay_ms") ? futOm->getDouble("order_delay_ms") : 0.0;
            if (futureOqmCfg.order_delay_ms > 0) {
                WTSLogger::log_by_cat("strategy", LL_WARN,
                    "orderManager.future.order_delay_ms={} set but order delay is not implemented yet — value ignored",
                    futureOqmCfg.order_delay_ms);
                futureOqmCfg.order_delay_ms = 0.0;
            }
        }
    }
    _optionOqmCfg = optionOqmCfg;
    _futureOqmCfg = futureOqmCfg;

    // Read RiskFilterChain configs from orderManager.riskFilters
    if (omCfg) {
        WTSVariant* rfCfg = omCfg->get("riskFilters");
        if (rfCfg) {
            // Option risk filters
            WTSVariant* optRf = rfCfg->get("option");
            if (optRf) {
                _optionFilterCfg.enabled = optRf->has("enabled") ? optRf->getBoolean("enabled") : false;
                _optionFilterCfg.max_order_size = optRf->has("max_order_size") ? optRf->getUInt32("max_order_size") : 100;
                _optionFilterCfg.max_order_size_reject = optRf->has("max_order_size_reject") ? optRf->getBoolean("max_order_size_reject") : false;
                _optionFilterCfg.min_sell_price = optRf->has("min_sell_price") ? optRf->getDouble("min_sell_price") : 1e-6;
                _optionFilterCfg.max_position = optRf->has("max_position") ? optRf->getUInt32("max_position") : 0;
                _optionFilterCfg.max_position_mode = optRf->has("max_position_mode") ? optRf->getInt32("max_position_mode") : 0;
                _optionFilterCfg.max_cancel_soft = optRf->has("max_cancel_soft") ? optRf->getInt32("max_cancel_soft") : 100;
                _optionFilterCfg.max_cancel_hard = optRf->has("max_cancel_hard") ? optRf->getInt32("max_cancel_hard") : 200;
                _optionFilterCfg.max_new_orders_hard_flat = optRf->has("max_new_orders_hard_flat") ? optRf->getInt32("max_new_orders_hard_flat") : 100;
                _optionFilterCfg.max_new_orders_reject = optRf->has("max_new_orders_reject") ? optRf->getInt32("max_new_orders_reject") : 200;
            }
            // Future risk filters
            WTSVariant* futRf = rfCfg->get("future");
            if (futRf) {
                _futureFilterCfg.enabled = futRf->has("enabled") ? futRf->getBoolean("enabled") : false;
                _futureFilterCfg.max_order_size = futRf->has("max_order_size") ? futRf->getUInt32("max_order_size") : 100;
                _futureFilterCfg.max_order_size_reject = futRf->has("max_order_size_reject") ? futRf->getBoolean("max_order_size_reject") : false;
                _futureFilterCfg.min_sell_price = futRf->has("min_sell_price") ? futRf->getDouble("min_sell_price") : 1e-6;
                _futureFilterCfg.max_position = futRf->has("max_position") ? futRf->getUInt32("max_position") : 0;
                _futureFilterCfg.max_position_mode = futRf->has("max_position_mode") ? futRf->getInt32("max_position_mode") : 0;
                _futureFilterCfg.max_cancel_soft = futRf->has("max_cancel_soft") ? futRf->getInt32("max_cancel_soft") : 100;
                _futureFilterCfg.max_cancel_hard = futRf->has("max_cancel_hard") ? futRf->getInt32("max_cancel_hard") : 200;
                _futureFilterCfg.max_new_orders_hard_flat = futRf->has("max_new_orders_hard_flat") ? futRf->getInt32("max_new_orders_hard_flat") : 100;
                _futureFilterCfg.max_new_orders_reject = futRf->has("max_new_orders_reject") ? futRf->getInt32("max_new_orders_reject") : 200;
            }
        }
    }

    _pricerType = cfg->has("pricerType") ? cfg->getCString("pricerType") : "composite_mm";
    if (!wt_option::parseOptionPricerMode(_pricerType, _pricerMode)) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "Unknown pricerType '{}'; expected composite_mm|gammascalp (defaulting to composite_mm)",
            _pricerType);
        _pricerType = "composite_mm";
        _pricerMode = wt_option::OptionPricerMode::CompositeMM;
    }

    // Save config pointer for setupPricer
    cfgPtr_ = cfg;

    // riskFreeRate/maxTPS defaults are already normalized by OptionConfigLoader;
    // an explicitly configured zero risk rate remains valid.

    // Read expiry configs from params top-level
    WTSVariant* expCfg = cfg->get("expiries");
    if (expCfg && expCfg->isArray()) {
        for (uint32_t i = 0; i < expCfg->size(); i++) {
            WTSVariant* e = expCfg->get(i);
            uint32_t expiry = static_cast<uint32_t>(e->getUInt32("expiry"));
            if (expiry == 0) continue;
            _expiryConfigs[expiry] = {
                e->getBoolean("enable"),
                e->has("underlyingCode") ? e->getCString("underlyingCode") : "",
                e->has("hedgeCode") ? e->getCString("hedgeCode") : "",
                {},  // secondaryHedgeCodes filled below
                e->has("delta_min") ? e->getDouble("delta_min") : 0.05,
                e->has("delta_max") ? e->getDouble("delta_max") : 0.95,
                e->has("sprd_fwd") ? e->getDouble("sprd_fwd") : 0.01,
                e->has("sprd_atmvol") ? e->getDouble("sprd_atmvol") : 0.1,
                e->has("sprd_corr") ? e->getDouble("sprd_corr") : 0.0,
                e->has("max_pos_fut") ? static_cast<int32_t>(e->getInt32("max_pos_fut")) : 1,
                e->has("max_pos_stk") ? static_cast<int32_t>(e->getInt32("max_pos_stk")) : 50,
                e->has("max_pos_opt") ? static_cast<int32_t>(e->getInt32("max_pos_opt")) : 50,
                e->has("max_qsize") ? static_cast<int32_t>(e->getInt32("max_qsize")) : 5,
                e->getBoolean("enable_auto_close"),
                e->has("close_pos_thresh") ? e->getDouble("close_pos_thresh") : 0,
                e->has("include_future") ? e->getBoolean("include_future") : true,
                e->has("min_strikes_for_synthetic") ? static_cast<int>(e->getInt32("min_strikes_for_synthetic")) : 1
            };
            // B14: Parse secondary hedge codes
            if (e->has("secondaryHedgeCodes")) {
                WTSVariant* shc = e->get("secondaryHedgeCodes");
                if (shc && shc->isArray()) {
                    for (uint32_t j = 0; j < shc->size(); j++) {
                        std::string code = shc->get(j)->asString();
                        _expiryConfigs[expiry].secondaryHedgeCodes.push_back(code);
                        _hedgeCodes.insert(code);
                    }
                }
            }
            const auto& hc = _expiryConfigs[expiry].hedgeCode;
            if (!hc.empty() && hc != _underlyingCode)
                _hedgeCodes.insert(hc);
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "Expiry config loaded: {} enable={} hedge={} delta=[{},{}]", expiry,
                e->getBoolean("enable"),
                hc.empty() ? "(=underlying)" : hc.c_str(),
                e->has("delta_min") ? e->getDouble("delta_min") : 0.05,
                e->has("delta_max") ? e->getDouble("delta_max") : 0.95);
        }
    }

    if (_underlyingCode.empty() && !_futuresProduct.empty()) {
        _underlyingCode = fmt::format("{}.{}.main", _exchange, _futuresProduct);
    }
    if (_futuresProduct.empty()) {
        // Extract from underlying code
        auto pos1 = _underlyingCode.find('.');
        if (pos1 != std::string::npos) {
            auto pos2 = _underlyingCode.find('.', pos1 + 1);
            _futuresProduct = (pos2 != std::string::npos)
                ? _underlyingCode.substr(pos1 + 1, pos2 - pos1 - 1)
                : _underlyingCode.substr(pos1 + 1);
        }
    }
    if (_exchange.empty()) {
        auto pos = _underlyingCode.find('.');
        if (pos != std::string::npos)
            _exchange = _underlyingCode.substr(0, pos);
    }

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "HftOptionStrategy[{}] init: underlying={} product={} exchange={} rate={} tps={}",
        id(), _underlyingCode, _optionProduct, _exchange, _riskFreeRate, _maxTPS);

    // Enhancement: Read extended risk limits config
    WTSVariant* riskCfg = cfg->get("riskLimits");
    if (riskCfg) {
        _riskLimitsEx.maxOrderSize = riskCfg->has("maxOrderSize") ? riskCfg->getUInt32("maxOrderSize") : 100;
        _riskLimitsEx.maxOrderValue = riskCfg->has("maxOrderValue") ? riskCfg->getDouble("maxOrderValue") : 1000000;
        _riskLimitsEx.clearlyErroneousPercent = riskCfg->has("clearlyErroneousPercent") ? riskCfg->getDouble("clearlyErroneousPercent") : 0.05;
        _riskLimitsEx.minSellPrice = riskCfg->has("minSellPrice") ? riskCfg->getDouble("minSellPrice") : 0;
        _riskLimitsEx.maxPositionPerOption = riskCfg->has("maxPositionPerOption") ? riskCfg->getInt32("maxPositionPerOption") : 100;
        _riskLimitsEx.maxTotalPosition = riskCfg->has("maxTotalPosition") ? riskCfg->getInt32("maxTotalPosition") : 1000;
        _riskLimitsEx.maxDelta = riskCfg->has("maxDelta") ? riskCfg->getDouble("maxDelta") : 1000;
        _riskLimitsEx.maxGamma = riskCfg->has("maxGamma") ? riskCfg->getDouble("maxGamma") : 100;
        _riskLimitsEx.maxVega = riskCfg->has("maxVega") ? riskCfg->getDouble("maxVega") : 10000;
        _riskLimitsEx.maxLossPerDay = riskCfg->has("maxLossPerDay") ? riskCfg->getDouble("maxLossPerDay") : 100000;
        // B3: short option limits (0 = disabled)
        _riskLimitsEx.maxShortCallPerSymbol = riskCfg->has("maxShortCallPerSymbol") ? riskCfg->getInt32("maxShortCallPerSymbol") : 0;
        _riskLimitsEx.maxShortPutPerSymbol = riskCfg->has("maxShortPutPerSymbol") ? riskCfg->getInt32("maxShortPutPerSymbol") : 0;
    }

    // B1: estimated-margin guard config
    if (cfg->has("margin")) {
        WTSVariant* mCfg = cfg->get("margin");
        if (mCfg) {
            _marginCfg.enabled = mCfg->has("enable") ? mCfg->getBoolean("enable") : false;
            _marginCfg.futRate = mCfg->has("fut_rate") ? mCfg->getDouble("fut_rate") : 0.10;
            _marginCfg.optShortRate = mCfg->has("opt_short_rate") ? mCfg->getDouble("opt_short_rate") : 0.12;
            _marginCfg.maxMargin = mCfg->has("max_margin") ? mCfg->getDouble("max_margin") : 0;
            _marginCfg.warnRatio = mCfg->has("warn_ratio") ? mCfg->getDouble("warn_ratio") : 0.9;
            _marginCfg.checkPeriodSec = mCfg->has("check_period_sec") ? mCfg->getDouble("check_period_sec") : 5.0;
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "Margin guard: enable={} max={:.0f} warnRatio={:.2f}",
                _marginCfg.enabled, _marginCfg.maxMargin, _marginCfg.warnRatio);
        }
    }

    // R10: portfolio delta-hedge executor config (default OFF)
    if (cfg->has("delta_hedge")) {
        WTSVariant* dh = cfg->get("delta_hedge");
        if (dh) {
            _deltaHedgeEnable = dh->has("enable") ? dh->getBoolean("enable") : false;
            _hedgeCfg.thresholdDelta = dh->has("threshold_delta") ? dh->getDouble("threshold_delta") : 0.0;
            _hedgeCfg.targetDelta    = dh->has("target_delta") ? dh->getInt32("target_delta") : 0;
            _hedgeCfg.maxOrderSize   = dh->has("max_order_size") ? dh->getUInt32("max_order_size") : 5;
            _hedgeCfg.cooldownSec    = dh->has("cooldown_sec") ? dh->getDouble("cooldown_sec") : 3.0;
            // delta_per_lot must be explicit (or derivable from the hedge contract
            // multiplier); defaulting to 1.0 would over-hedge by the multiplier.
            // Legacy migration: old configs used `minHedgeQty` with the same
            // intended meaning; accept it once with a warning.
            if (dh->has("delta_per_lot")) {
                _hedgeCfg.deltaPerLot = dh->getDouble("delta_per_lot");
            } else if (dh->has("minHedgeQty")) {
                _hedgeCfg.deltaPerLot = dh->getDouble("minHedgeQty");
                WTSLogger::log_by_cat("strategy", LL_WARN,
                    "delta_hedge.minHedgeQty is deprecated; migrating to delta_per_lot={}",
                    _hedgeCfg.deltaPerLot);
            } else {
                _hedgeCfg.deltaPerLot = 0.0;
            }
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "DeltaHedger: enable={} threshold={} delta_per_lot={}",
                _deltaHedgeEnable, _hedgeCfg.thresholdDelta, _hedgeCfg.deltaPerLot);
        }
    }

    // Phase 2: optional hard stress limit (0 = alert only)
    if (cfg->has("riskLimits") && cfg->get("riskLimits")->has("maxStressLoss"))
        _stressLimit = cfg->get("riskLimits")->getDouble("maxStressLoss");

    // B5: FIFO PnL matching mode
    if (cfg->has("pnl")) {
        WTSVariant* pnlCfg = cfg->get("pnl");
        if (pnlCfg)
            _fifoPnlMode = pnlCfg->has("fifo_mode") ? pnlCfg->getBoolean("fifo_mode") : false;
    }

    // Enhancement: Initialize FillPriceChecker
    _fillPriceChecker = std::make_shared<wt_option::FillPriceChecker>();
    _fillPriceChecker->setPanicCallback([this](const std::string& code, double, double, double) {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "FillPriceChecker PANIC callback: {} -> triggering risk check", code);
        // P1-e: route through the unified state machine (single writer of
        // panicked) instead of poking the atomic directly.
        setState(wt_option::TradingStateType::Panic, "fill_price_deviation");
    });

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "Enhancement modules initialized: RiskLimitsEx FillPriceChecker");

    // Effective-value dump: proves which high-risk keys were actually consumed
    // and with what value, instead of relying on schema membership alone.
    WTSLogger::log_by_cat("strategy", LL_INFO, "{}",
        wt_option::OptionConfigLoader::dumpEffectiveValues(_configLoad));

    // R-B33: startup config schema validation — unknown keys WARN loudly
    wt_option::ConfigValidator::validate(cfg);
    return true;
}

// ============================================================================
// on_init — create grid, pricer, CTG, wire async callbacks
// ============================================================================
void HftOptionStrategy::on_init(IHftStraCtx* ctx)
{
    if (_stress.scenarios().empty())
        _stress.addDefaultScenarios();

    _hedgeCoord.configure(_hedgeCfg, 0.0, "",
        [this](const std::string& code, bool isBuy, double px, uint32_t qty) {
            // Hedge in-flight tracking requires the real framework localids,
            // not the executeOrder() success-count wrapper.
            return executeOrderIds(code, isBuy, px, qty);
        },
        [this](const std::string& code) -> double {
            if (_otg && !code.empty()) {
                auto utd = _otg->getUnderlyingTradingData(code);
                if (utd && utd->getMid() > 0) return utd->getMid();
            }
            return _grid ? _grid->getUnderlyingPrice() : 0.0;
        },
        [](const std::string& msg) {
            WTSLogger::log_by_cat("strategy", LL_INFO, "{}", msg);
        });
    _ctx = ctx;
    WTSLogger::log_by_cat("strategy", LL_INFO, "HftOptionStrategy[{}] on_init", id());

    // Auto-discover option contracts from framework base data
    // Discover all option product IDs (e.g. ["SRP","SRC"] for CZCE, ["IO"] for CFFEX)
    {
        for (const auto& optPid : _optionPids) {
            std::string commKey = fmt::format("{}.{}", _exchange, optPid);
            WTSCommodityInfo* commInfo = _ctx->stra_get_comminfo(commKey.c_str());
            if (commInfo && commInfo->isOption()) {
                const CodeSet& codes = commInfo->getCodes();
                size_t skipped = 0;
                for (const auto& rawCode : codes) {
                    // getCodes() returns ALL contracts of the commodity,
                    // including underlying futures (e.g. cu2607). Passing a
                    // pure-digit code to rawFutOptCodeToStdCode underflows
                    // its index scan and throws std::length_error — filter
                    // to real option codes first.
                    // C6: accept BOTH 4-digit-month codes (CFFEX/SHFE/INE/
                    // GFEX, e.g. IO2301-P-4000) and CZCE's 3-digit-month
                    // convention (e.g. TA308P4000 — the very reason
                    // rawFutOptCodeToStdCode inserts "2" for CZCE, see
                    // Share/CodeHelper.hpp).  The previous \\d{4} required
                    // 4 digits and silently dropped EVERY CZCE option.
                    static const std::regex optCodeRe(
                        "^[A-Za-z]{1,3}\\d{3,4}[-]?(C|P)[-]?\\d+$");
                    if (!std::regex_match(rawCode, optCodeRe)) {
                        skipped++;
                        continue;
                    }
                    std::string stdCode = CodeHelper::rawFutOptCodeToStdCode(rawCode.c_str(), _exchange.c_str());
                    _optionCodes.emplace_back(stdCode);
                    _positionGuards[stdCode] = std::make_shared<wt_option::PositionGuard>();
                    _positionOffsets[stdCode] = std::make_shared<wt_option::PositionOffsetMgr>();
                }
                WTSLogger::log_by_cat("strategy", LL_INFO,
                    "Auto-discovered {} option contracts for {}{}", codes.size() - skipped, commKey,
                    skipped ? fmt::format(" (skipped {} non-option codes)", skipped).c_str() : "");
            } else {
                WTSLogger::log_by_cat("strategy", LL_WARN,
                    "Option product {} not found", commKey);
            }
        }
        // Create PositionGuard/PositionOffsetMgr for underlying (skip if index - not tradeable)
        if (!_underlyingCode.empty() && _underlyingType != "index") {
            _positionGuards[_underlyingCode] = std::make_shared<wt_option::PositionGuard>();
            _positionOffsets[_underlyingCode] = std::make_shared<wt_option::PositionOffsetMgr>();
        }
    }

    // Get session info from futures product (for trading hours, holidays)
    if (!_futuresProduct.empty()) {
        std::string commKey = fmt::format("{}.{}", _exchange, _futuresProduct);
        WTSCommodityInfo* commInfo = _ctx->stra_get_comminfo(commKey.c_str());
        if (commInfo && commInfo->getSessionInfo()) {
            _sessionInfo = commInfo->getSessionInfo();
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "Session info loaded for {}: {} sections",
                commKey, _sessionInfo->getTradingSections().size());
        }
    }

    // Create async event processor (must be before setupGrid, which registers listeners)
    _async = std::make_shared<wt_option::OptionAsyncEventProcessor>();

    // B2: anomaly guard (IssuedOrderTracker absorption)
    _anomalyGuard.setGetTimeFn([this]() { return _ctx ? _timeService.nowSeconds(_ctx) : 0.0; });
    _anomalyGuard.setAlertCallback([](const std::string& msg) {
        WTSLogger::log_by_cat("strategy", LL_ERROR, "OrderAnomaly: {}", msg);
    });

    // R-P1: prediction accuracy loop closure + R9 command bus
    _predEvaluator.setGetTimeFn([this]() { return _ctx ? _timeService.nowSeconds(_ctx) : 0.0; });
    registerCommands();

    setupGrid();
    setupPricer();
    setupCTG();
    setupSignals();
    setupScanners();

    // B7: Initialize attribute publisher
    _attrPub = std::make_shared<wt_option::AttributePublisher>();
    _attrPub->setRisk(_risk.get());
    _attrPub->setGrid(_grid.get());

    // B9: Initialize expiration simulator
    _expSim = std::make_shared<wt_option::ExpirationSimulator>();
    _expSim->setGrid(_grid.get());
    _expSim->setRisk(_risk.get());

    // B10: Initialize option value writer (if configured)
    if (cfgPtr_->has("optionValueWriter")) {
        WTSVariant* ovwCfg = cfgPtr_->get("optionValueWriter");
        _ovwEnabled = ovwCfg->has("enable") ? ovwCfg->getBoolean("enable") : false;
        if (_ovwEnabled) {
            _valWriter = std::make_shared<wt_option::OptionValueWriter>();
            _valWriter->setOutputDir(ovwCfg->has("outputDir") ? ovwCfg->getCString("outputDir") : ".");
            _ovwStartTime = ovwCfg->has("startTime") ? ovwCfg->getDouble("startTime") : 0;
            _ovwEndTime = ovwCfg->has("endTime") ? ovwCfg->getDouble("endTime") : 86400;
            _ovwPeriod = ovwCfg->has("outputPeriod") ? ovwCfg->getDouble("outputPeriod") : 5.0;
            _valWriter->setStartTime(_ovwStartTime);
            _valWriter->setEndTime(_ovwEndTime);
            _valWriter->setOutputPeriod(_ovwPeriod);
        }
    }

    // B11: Initialize predictor infrastructure
    _predictor = std::make_shared<wt_option::SignalsPredictor>();
    _triggerEngine = std::make_shared<wt_option::TriggerEngine>();
    // Trigger predictor update every 1 second
    _triggerEngine->addTrigger(1.0, [this]() {
        if (_predictor && _grid) {
            wt_option::PredictionState state;
            state.forward = _grid->getUnderlyingPrice();
            auto frontEd = _grid->getFrontMonthExpiryData();
            if (frontEd) {
                state.atmVol = 0;
                state.timestamp = static_cast<uint64_t>(
                    _timeService.nowSeconds(_ctx) * 1000);
                state.confidence = frontEd->isValuesReady() ? 1.0 : 0.0;
            }
            _predictor->update(state);
            // R-P1: score predictions against realized forward
            const double fwdNow = state.forward > 0 ? state.forward
                                : (_grid ? _grid->getUnderlyingPrice() : 0.0);
            if (fwdNow > 0) {
                _predEvaluator.onPrediction(state.forward, 30.0 /*30s horizon*/);
                _predEvaluator.onRealized(fwdNow);
            }
        }
    });
    setupAsyncCallbacks();

    _async->start();
    _initialized = true;

    // P10: Register hot-update params via sync_param (实盘生效, 回测返回nullptr)
    _hot.wgt_vegaflow      = _ctx->sync_param("wgt_vegaflow", _wgt_vegaflow);
    _hot.wgt_frontfut_skew = _ctx->sync_param("wgt_frontfut_skew", _wgt_frontfut_skew);
    _hot.wgt_deltaflow     = _ctx->sync_param("wgt_deltaflow", _wgt_deltaflow);
    _hot.wgt_atmsig        = _ctx->sync_param("wgt_atmsig", _wgt_atmsig);
    _hot.wgt_rollema       = _ctx->sync_param("wgt_rollema", _wgt_rollema);
    _hot.sticky_base       = _ctx->sync_param("sticky_base", _sticky_base);
    _hot.improve_retreat_ratio = _ctx->sync_param("improve_retreat_ratio", _improve_retreat);
    _hot.trade_shock_ticks = _ctx->sync_param("trade_shock_ticks", 1.0);
    _hot.max_tps           = _ctx->sync_param("max_tps", _maxTPS);
    _hot.command           = _ctx->sync_param("command", 0);
    _hot.qmode_override    = _ctx->sync_param("qmode_override", 0);
    // B8: Manual order hot-param (string-based command)
    _hot.manual_order      = _ctx->sync_param("manual_order", "");
    _ctx->commit_param_watcher();

    // Subscribe to underlying ticks (global pricing underlying, e.g. index for CFFEX)
    if (!_underlyingCode.empty()) {
        ctx->stra_sub_ticks(_underlyingCode.c_str());
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "HftOptionStrategy subscribed underlying: {}", _underlyingCode);
    }

    // Subscribe to per-expiry underlying contracts (multi-series scenario)
    std::set<std::string> perExpiryUnderlyingCodes;
    for (const auto& [exp, ec] : _expiryConfigs) {
        std::string ulCode = ec.underlyingCode.empty() ? _underlyingCode : ec.underlyingCode;
        if (!ulCode.empty() && ulCode != _underlyingCode) {
            perExpiryUnderlyingCodes.insert(ulCode);
        }
        // Also collect hedge codes
        std::string hedgeCode = ec.hedgeCode.empty() ? ulCode : ec.hedgeCode;
        if (!hedgeCode.empty() && hedgeCode != _underlyingCode) {
            _hedgeCodes.insert(hedgeCode);
            // Ensure future OQMs created for per-expiry hedge codes also have
            // guard/offset objects available through the dynamic lookup.
            if (!_positionGuards.count(hedgeCode))
                _positionGuards[hedgeCode] = std::make_shared<wt_option::PositionGuard>();
            if (!_positionOffsets.count(hedgeCode))
                _positionOffsets[hedgeCode] = std::make_shared<wt_option::PositionOffsetMgr>();
        }
    }
    for (const auto& code : perExpiryUnderlyingCodes) {
        ctx->stra_sub_ticks(code.c_str());
    }
    if (!perExpiryUnderlyingCodes.empty()) {
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "HftOptionStrategy subscribed {} per-expiry underlying contracts",
            perExpiryUnderlyingCodes.size());
    }

    // Subscribe to ALL listed option contracts
    int optCount = 0;
    for (const auto& code : _optionCodes) {
        ctx->stra_sub_ticks(code.c_str());
        optCount++;
    }
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "HftOptionStrategy subscribed {} option contracts", optCount);

    // Subscribe to hedge contracts (distinct from underlying)
    for (const auto& code : _hedgeCodes) {
        ctx->stra_sub_ticks(code.c_str());
    }
    if (!_hedgeCodes.empty()) {
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "HftOptionStrategy subscribed {} hedge contracts", _hedgeCodes.size());
    }

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "HftOptionStrategy[{}] initialized, grid={} pricer={} ctg={}",
        id(), (bool)_grid, (bool)_pricer, (bool)_ctg);
}

// ============================================================================
// setupGrid — create OptionGrid with IBaseDataMgr from HftStraContext
// ============================================================================
void HftOptionStrategy::setupGrid()
{
    // In backtest mode, _ctx is HftMocker (not HftStraContext)
    // IBaseDataMgr is only available in live mode via HftStraContext
    // For backtest, pass nullptr — grid will use holidays from holidays.json
    wtp::IBaseDataMgr* bdMgr = nullptr;
    wtp::WTSSessionInfo* sessInfo = _sessionInfo;

    _grid = std::make_shared<wt_option::OptionGrid>(
        _optionProduct, _underlyingCode, bdMgr, sessInfo);
    _grid->setMarketStaleTimeoutMs(_marketStaleTimeoutMs);

    // Load holidays from holidays.json (same as longbeach ExchangeCalendar)
    // This is used by ExpiryData::countTradingDays when m_bdMgr is null.
    // Path candidates: live deployment uses common/, replay uses
    // replay_common/ (dist/WtBtRunner has no common/ dir).
    std::set<uint32_t> holidays;
    const char* holidayCandidates[] = {
        "common/holidays.json", "../common/holidays.json", "replay_common/holidays.json",
    };
    std::ifstream ifs;
    for (const char* cand : holidayCandidates) {
        ifs.open(cand);
        if (ifs.is_open()) break;
    }
    if (ifs.is_open()) {
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        // Parse JSON to extract CHINA holidays array
        // Simple extraction: find "CHINA": [ ... ] and parse dates
        size_t pos = content.find("\"CHINA\"");
        if (pos != std::string::npos) {
            size_t arrStart = content.find('[', pos);
            size_t arrEnd = content.find(']', arrStart);
            if (arrStart != std::string::npos && arrEnd != std::string::npos) {
                std::string arr = content.substr(arrStart + 1, arrEnd - arrStart - 1);
                // Parse comma-separated quoted date strings
                size_t i = 0;
                while (i < arr.size()) {
                    size_t quote1 = arr.find('"', i);
                    if (quote1 == std::string::npos) break;
                    size_t quote2 = arr.find('"', quote1 + 1);
                    if (quote2 == std::string::npos) break;
                    std::string dateStr = arr.substr(quote1 + 1, quote2 - quote1 - 1);
                    if (dateStr.size() == 8) {
                        uint32_t d = std::stoul(dateStr);
                        holidays.insert(d);
                    }
                    i = quote2 + 1;
                }
            }
        }
    }
    _grid->setHolidays(std::move(holidays));

    // B2: Set risk-free rate / curve on grid
    _grid->setRiskFreeRate(_riskFreeRate);
    if (!_rateCurve.empty()) {
        _grid->setRateCurve(_rateCurve);
    }

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "HftOptionStrategy grid created: product={} underlying={} holidays={}",
        _optionProduct, _underlyingCode, _grid->numHolidays());

    // R3: wire exchange strike-grid finders (quantbox absorption)
    wireStrikeFinders();
}

// ============================================================================
// wireStrikeFinders — R3: per-expiry strike-grid rules absorbed from
// quantbox optioncore StrikeFinders. CFFEX IO/MO/HO: monthly 50 / quarterly
// 100; DCE products: price-tiered 25/50/100. Rule of thumb for expiry type:
// the nearest two expiries are MONTHLY, the rest QUARTERLY.
// ============================================================================
void HftOptionStrategy::wireStrikeFinders()
{
    if (!_grid || _expiryConfigs.empty()) return;

    const bool useCffex = (_exchange == "CFFEX");
    const bool useDceTiered = (_exchange == "DCE");
    if (!useCffex && !useDceTiered) return;

    std::vector<uint32_t> exps;
    for (const auto& [exp, ec] : _expiryConfigs)
        if (ec.enable) exps.push_back(exp);
    std::sort(exps.begin(), exps.end());

    for (size_t i = 0; i < exps.size(); ++i) {
        auto type = (i < 2) ? wt_option::IStrikeFinder::MONTHLY
                            : wt_option::IStrikeFinder::QUARTERLY;
        if (useCffex)
            _grid->setStrikeFinder(exps[i],
                std::make_shared<wt_option::CffexIOStrikeFinder>(type));
        else
            _grid->setStrikeFinder(exps[i],
                std::make_shared<wt_option::DceMOStrikeFinder>(type));
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "StrikeFinder wired: exp={} type={}", exps[i],
            type == wt_option::IStrikeFinder::MONTHLY ? "MONTHLY" : "QUARTERLY");
    }
}

// ============================================================================
// setupPricer — create pricer (composite_mm or gammascalp) + OptionPricer2
// ============================================================================
void HftOptionStrategy::setupPricer()
{
    if (!_grid) return;

    // Create OptionRisk — hedge instruments auto-registered per expiry via getExpiryGreeks
    _risk = std::make_shared<wt_option::OptionRisk>(_grid);
    _grid->addListener(_risk.get());

    // Create OptionTradingGrid (middle layer)
    _otg = std::make_shared<wt_option::OptionTradingGrid>(_grid);
    _otg->setPositionRisk(_risk);
    _otg->setExchange(_exchange);
    // Wire executors through OTG → OTD
    _otg->setQuoteExecutor([this](const std::string& code, double bidP, uint32_t bidQ,
                                   double askP, uint32_t askQ) -> int32_t {
        return executeQuote(code, bidP, bidQ, askP, askQ);
    });
    _otg->setOrderExecutor([this](const std::string& code, bool isBuy,
                                   double price, uint32_t qty) -> int32_t {
        return executeOrder(code, isBuy, price, qty);
    });
    _otg->setCancelExecutor([this](const std::string& code) -> int32_t {
        return executeCancel(code);
    });
    _grid->addListener(_otg.get());  // OTG receives onAddOption/onAddExpiry
    _otg->setHftCtx(_ctx);

    // Formal production order gateway.  OQM never calls IHftStraCtx directly;
    // all exchange operations enter through this adapter after strategy-side
    // price-limit and issuance-registration policy.
    _orderGateway = std::make_shared<wt_option::OrderGateway>(
        wt_option::OrderGateway::Handlers{
            [this](const std::string& code, double bidP, uint32_t bidQ,
                   double askP, uint32_t askQ) {
                return executeQuoteIds(code, bidP, bidQ, askP, askQ);
            },
            [this](const std::string& code, bool isBuy,
                   double price, uint32_t qty) {
                return executeOrderIds(code, isBuy, price, qty);
            },
            [this](uint32_t localid) -> bool {
                return executeCancel(localid);
            },
            [this](const std::string& code) {
                executeCancel(code);
            }
        });
    _otg->setOrderGateway(_orderGateway);
    _otg->setTimeFn([this]() {
        return _timeService.nowSeconds(_ctx);
    });

    _otg->setOptionOQMConfig(_optionOqmCfg);
    _otg->setFutureOQMConfig(_futureOqmCfg);
    _otg->setQuoteStatistics(&_quoteStats);

    // A2: pre-trade limit checker (RiskLimitsEx) — applied to every OQM at creation
    _otg->setPreTradeCheckFn(
        [this](const std::string& pcode, bool isBuy, double price, uint32_t& qty,
               int32_t curPos, std::string& reason) -> bool {
            auto rep = _riskLimitsEx.checkPreTrade(pcode, isBuy, price, qty, curPos, /*refPrice*/ 0.0);
            if (rep.result == wt_option::RiskLimitsEx::CheckResult::REJECT) {
                reason = rep.reason;
                return false;
            }
            if (rep.result == wt_option::RiskLimitsEx::CheckResult::WARN) {
                WTSLogger::log_by_cat("strategy", LL_WARN,
                    "RiskLimits WARN {}: {}", pcode, rep.reason);
            }
            return true;
        });

    // B5: FIFO PnL matching mode for newly created trackers
    _otg->setFifoPnlMode(_fifoPnlMode);

    // Inject SessionInfo for QuoteStatistics (session-based time tracking)
    if (_sessionInfo)
        _quoteStats.setSessionInfo(_sessionInfo);

    // Create shared theoretical pricer (OptionPricer2) — used by both strategies
    wt_option::OptionPricer2Config p2cfg;
    // P1-a: fitter mode. "async" (default) drives AsyncVolFitter; "sync"
    // re-enables the legacy PeriodicCurveFitter inside OptionPricer2 (used
    // for backtest determinism / fit debugging). The two chains are mutually
    // exclusive — only one path may mutate the curves.
    _volFitMode = "async";
    p2cfg.enable_fitter = false;
    p2cfg.use_parallel_for = cfgPtr_->has("use_parallel")
        ? cfgPtr_->getBoolean("use_parallel") : false;

    // Read fitter config from pricer.blackCalc.volCurve.fitter
    {
        WTSVariant* pCfg0 = cfgPtr_->get("pricer");
        if (!pCfg0) pCfg0 = cfgPtr_;
        WTSVariant* bcCfg0 = pCfg0->get("blackCalc");
        if (bcCfg0) {
            WTSVariant* vcCfg0 = bcCfg0->get("volCurve");
            if (vcCfg0) {
                WTSVariant* fitterCfg = vcCfg0->get("fitter");
                if (fitterCfg) {
                    // P1-a: mode switch (async default / sync legacy)
                    if (fitterCfg->has("mode")) {
                        std::string m = fitterCfg->getCString("mode");
                        if (m == "async" || m == "sync") {
                            _volFitMode = m;
                            p2cfg.enable_fitter = (m == "sync");
                        } else {
                            WTSLogger::log_by_cat("strategy", LL_WARN,
                                "pricer.blackCalc.volCurve.fitter.mode '{}' invalid (use async|sync), defaulting to async",
                                m);
                        }
                    }
                    p2cfg.vol_fitting_start_time = fitterCfg->has("start_time")
                        ? fitterCfg->getDouble("start_time") : 0.0;
                    p2cfg.vol_fitting_end_time = fitterCfg->has("end_time")
                        ? fitterCfg->getDouble("end_time") : 86400.0;
                    p2cfg.vol_fitting_period = fitterCfg->has("period")
                        ? fitterCfg->getDouble("period") : 60.0;
                    p2cfg.vol_fitting_decay_window = fitterCfg->has("decay_window")
                        ? fitterCfg->getDouble("decay_window") : 300.0;
                    p2cfg.vol_fitting_threshold = fitterCfg->has("threshold")
                        ? fitterCfg->getDouble("threshold") : 0.0;
                    p2cfg.vol_fitting_to_all_expiries = fitterCfg->has("fit_all_expiries")
                        ? fitterCfg->getBoolean("fit_all_expiries") : false;
                    WTSVariant* threshCfg = fitterCfg->get("good_points_thresh");
                    if (threshCfg && threshCfg->isArray()) {
                        for (uint32_t i = 0; i < threshCfg->size(); i++) {
                            WTSVariant* item = threshCfg->get(i);
                            if (item && item->has("days") && item->has("thresh")) {
                                p2cfg.vol_fitting_good_points_thresh[item->getInt32("days")] =
                                    item->getDouble("thresh");
                            }
                        }
                    }
                }
            }
        }
        p2cfg.trace_level = pCfg0->has("trace_level")
            ? pCfg0->getInt32("trace_level") : 0;
        p2cfg.fast_use_gamma_correction = pCfg0->has("fast_use_gamma_correction")
            ? pCfg0->getBoolean("fast_use_gamma_correction") : true;
        p2cfg.fast_full_reprice_forward_step = pCfg0->has("fast_full_reprice_forward_step")
            ? pCfg0->getDouble("fast_full_reprice_forward_step") : 0.0;

        // R2: curve2 type — "power" restores the original GVV×Power dual-curve
        // design and makes volcurve_weight effective; "gvv" keeps legacy behavior
        // P1-b: both type and curve2_type accept gvv|power|linear|constant.
        {
            WTSVariant* bc = pCfg0->get("blackCalc");
            WTSVariant* vc = bc ? bc->get("volCurve") : nullptr;
            if (vc) {
                if (vc->has("type")) {
                    std::string t = vc->getCString("type");
                    if (t == "gvv" || t == "linear" || t == "constant")
                        p2cfg.volcurve_type = t;
                    else
                        WTSLogger::log_by_cat("strategy", LL_WARN,
                            "pricer.blackCalc.volCurve.type '{}' invalid (use gvv|linear|constant)", t);
                }
                if (vc->has("curve2_type")) {
                    std::string t = vc->getCString("curve2_type");
                    if (t == "power" || t == "gvv" || t == "linear" || t == "constant")
                        p2cfg.curve2_type = t;
                    else
                        WTSLogger::log_by_cat("strategy", LL_WARN,
                            "pricer.blackCalc.volCurve.curve2_type '{}' invalid (use gvv|power|linear|constant)", t);
                }
                if (vc->has("fallback"))
                    p2cfg.volcurve_fallback = vc->getBoolean("fallback");
            }
        }
    }

    auto pricer2 = std::make_shared<wt_option::OptionPricer2>(p2cfg, _grid.get(), _risk.get());
    _blackPricer = pricer2;
    _asyncVolFitter.setCurveType(p2cfg.volcurve_type);
    _asyncVolFitter.setCurve2Type(p2cfg.curve2_type);
    _asyncVolFitter.setFallbackEnabled(p2cfg.volcurve_fallback);
    _asyncVolFitter.setDecayWindow(p2cfg.vol_fitting_decay_window);

    // Read pricer config from nested section
    WTSVariant* pCfg = cfgPtr_->get("pricer");
    if (!pCfg) pCfg = cfgPtr_;  // backward compat

    // Vol curve config (read from pricer.blackCalc.volCurve)
    double fitInterval = 60.0;
    {
        WTSVariant* bcCfg = pCfg->get("blackCalc");
        if (bcCfg) {
            WTSVariant* vcCfg = bcCfg->get("volCurve");
            if (vcCfg) {
                fitInterval = vcCfg->has("fitter") && vcCfg->get("fitter")->has("period")
                    ? vcCfg->get("fitter")->getDouble("period") : 60.0;
            }
        }
    }

    // === Pluggable pricer factory ===
    if (_pricerMode == wt_option::OptionPricerMode::GammaScalp) {
        // --- Gamma Scalping pricer ---
        _gammaPricer = std::make_shared<wt_option::GammaScalpOptionPricer>();
        _gammaPricer->setTheoreticalPricer(pricer2);
        _gammaPricer->setOptionRisk(_risk);

        // Wire order execution for dynamic hedging
        _gammaPricer->setOrderSender([this](const std::string& code, bool isBuy,
                                             double price, int32_t qty) {
            return executeOrderIds(code, isBuy, price, static_cast<uint32_t>(qty));
        });
        _gammaPricer->setTimeProvider([this]() {
            return _timeService.nowSeconds(_ctx);
        });

        // Wire fill price provider for trade shock back-away
        // (returns 0 = disabled; CompositeOptionPricer has its own back-away via setInstrumentLastBuy/Sell)
        _gammaPricer->setFillPriceProvider([](const std::string&, bool) -> double { return 0; });

        // Read gammascalp config
        wt_option::GammaScalpConfig gsCfg;
        WTSVariant* gsCfgNode = cfgPtr_->get("gammascalp");
        if (gsCfgNode) {
            gsCfg.enable = gsCfgNode->has("enable") ? gsCfgNode->getBoolean("enable") : true;
            gsCfg.targetGamma = gsCfgNode->has("targetGamma") ? gsCfgNode->getDouble("targetGamma") : 1000.0;
            gsCfg.hedgeThresholdRisk = gsCfgNode->has("hedgeThresholdRisk") ? gsCfgNode->getDouble("hedgeThresholdRisk") : 10.0;
            gsCfg.transactionCost = gsCfgNode->has("transactionCost") ? gsCfgNode->getDouble("transactionCost") : 0.0005;
            gsCfg.impliedVolatility = gsCfgNode->has("impliedVolatility") ? gsCfgNode->getDouble("impliedVolatility") : 0.20;
            gsCfg.maxOrderSize = gsCfgNode->has("maxOrderSize") ? gsCfgNode->getInt32("maxOrderSize") : 10;
            gsCfg.deltaMin = gsCfgNode->has("deltaMin") ? gsCfgNode->getDouble("deltaMin") : 0.1;
            gsCfg.deltaMax = gsCfgNode->has("deltaMax") ? gsCfgNode->getDouble("deltaMax") : 0.9;
            gsCfg.spreadFut = gsCfgNode->has("spreadFut") ? gsCfgNode->getDouble("spreadFut") : 0.0002;
            gsCfg.spreadVol = gsCfgNode->has("spreadVol") ? gsCfgNode->getDouble("spreadVol") : 0.005;
            gsCfg.minSpread = gsCfgNode->has("minSpread") ? gsCfgNode->getDouble("minSpread") : 0.0005;
            gsCfg.shockTicks = gsCfgNode->has("shockTicks") ? gsCfgNode->getInt32("shockTicks") : 2;
        }

        for (const auto& [exp, ec] : _expiryConfigs) {
            if (ec.enable) {
                _gammaPricer->setExpiryConfig(exp, gsCfg);
            }
        }

        _pricer = _gammaPricer;
        _compositePricer = nullptr;

        WTSLogger::log_by_cat("strategy", LL_INFO,
            "HftOptionStrategy: gammascalp pricer created");
    } else {
        // --- CompositeOptionPricer (default: market making) ---
        wt_option::CompositeOptionPricerConfig copCfg;
        copCfg.slow_compute_interval = _configLoad.values.slowComputeInterval;
        copCfg.wgt_vegaflow      = pCfg->has("wgt_vegaflow") ? pCfg->getDouble("wgt_vegaflow") :
            (pCfg->has("alpha") ? pCfg->get("alpha")->getDouble("wgt_vegaflow") : 0.0);
        copCfg.wgt_frontfut_skew = pCfg->has("wgt_frontfut_skew") ? pCfg->getDouble("wgt_frontfut_skew") :
            (pCfg->has("alpha") ? pCfg->get("alpha")->getDouble("wgt_frontfut_skew") : 0.0);
        copCfg.wgt_deltaflow     = pCfg->has("wgt_deltaflow") ? pCfg->getDouble("wgt_deltaflow") :
            (pCfg->has("alpha") ? pCfg->get("alpha")->getDouble("wgt_deltaflow") : 0.0);
        copCfg.wgt_atmsig        = pCfg->has("wgt_atmsig") ? pCfg->getDouble("wgt_atmsig") :
            (pCfg->has("alpha") ? pCfg->get("alpha")->getDouble("wgt_atmsig") : 0.0);
        copCfg.wgt_rollema       = pCfg->has("wgt_rollema") ? pCfg->getDouble("wgt_rollema") :
            (pCfg->has("alpha") ? pCfg->get("alpha")->getDouble("wgt_rollema") : 0.0);
        copCfg.sticky_base       = pCfg->has("sticky_base") ? pCfg->getDouble("sticky_base") :
            (pCfg->has("quoting") ? pCfg->get("quoting")->getDouble("sticky_base") : 0.5);
        copCfg.improve_retreat_ratio = pCfg->has("improve_retreat_ratio") ? pCfg->getDouble("improve_retreat_ratio") :
            (pCfg->has("quoting") ? pCfg->get("quoting")->getDouble("improve_retreat_ratio") : 3.0);

        // Hedge ratios and lambda params (P0-D fix)
        copCfg.hedge_ratio_delta = pCfg->has("hedge_ratio_delta") ? pCfg->getDouble("hedge_ratio_delta") : 0.0;
        copCfg.hedge_ratio_vega  = pCfg->has("hedge_ratio_vega")  ? pCfg->getDouble("hedge_ratio_vega")  : 0.0;
        copCfg.lambda_vega_decay = pCfg->has("lambda_vega_decay") ? pCfg->getDouble("lambda_vega_decay") : 0.0;
        copCfg.lambda_vega_wing  = pCfg->has("lambda_vega_wing")  ? pCfg->getDouble("lambda_vega_wing")  : 0.0;

        // GVV blend weight
        {
            WTSVariant* bcCfg = pCfg->get("blackCalc");
            if (bcCfg) {
                WTSVariant* vcCfg = bcCfg->get("volCurve");
                if (vcCfg) {
                    copCfg.volcurve_weight = vcCfg->has("volcurve_weight")
                        ? vcCfg->getDouble("volcurve_weight") : 0.0;
                }
            }
        }

        _compositePricer = std::make_shared<wt_option::CompositeOptionPricer>(copCfg, _grid.get(), _risk.get());
        _compositePricer->setBlackPricer(pricer2);
        _compositePricer->setFitInterval(fitInterval);

        // Configure expiry risk from _expiryConfigs
        for (const auto& [exp, ec] : _expiryConfigs) {
            if (ec.enable) {
                _compositePricer->enableExpiry(exp, ec.delta_min, ec.delta_max);
                _compositePricer->setMaxPosQty(exp, ec.max_qsize, ec.max_pos_stk, ec.max_pos_opt);
                _compositePricer->setExpiryCloseParams(exp, ec.enable_auto_close,
                    static_cast<int32_t>(ec.close_pos_thresh));
            }
        }

        _pricer = _compositePricer;
        _gammaPricer = nullptr;
    }

    // Common per-expiry setup (shared by both pricer types)
    auto hedgeMultiplier = [this](const std::string& code) -> double {
        if (_ctx && !code.empty()) {
            auto ci = CodeHelper::extractStdCode(code.c_str(), nullptr);
            std::string prodKey = fmt::format("{}.{}", ci._exchg, ci._product);
            WTSCommodityInfo* cinfo = _ctx->stra_get_comminfo(prodKey.c_str());
            if (cinfo && cinfo->getVolScale() > 0)
                return cinfo->getVolScale();
        }
        return 1.0;
    };

    for (const auto& [exp, ec] : _expiryConfigs) {
        // Per-expiry underlying code registration.
        // P2/G3: an EXPLICIT expiries[].underlyingCode is exclusive — the
        // global underlying tick no longer overwrites this expiry's price.
        // An empty per-expiry code falls back to the global underlying.
        if (!ec.underlyingCode.empty()) {
            _grid->setExpiryOwnUnderlying(exp, ec.underlyingCode);
        } else if (!_underlyingCode.empty()) {
            _grid->setExpiryUnderlying(exp, _underlyingCode);
        }
        // P0-4: register forward-calculation config; ExpiryData is created
        // lazily on the first tick, so direct application at on_init time
        // used to be silently dropped.  The grid applies it on creation.
        _grid->setExpiryForwardConfig(exp, ec.include_future,
                                      ec.min_strikes_for_synthetic);
        // Hedge override
        std::string hedgeCode = ec.hedgeCode.empty()
            ? (ec.underlyingCode.empty() ? _underlyingCode : ec.underlyingCode)
            : ec.hedgeCode;
        if (!hedgeCode.empty()) {
            _otg->setHedgeOverride(exp, hedgeCode);
            // B18 primary hedge: register with the real multiplier before
            // OptionExpiryGreeks auto-registers the same code with 1.0.
            _risk->registerHedgeInstrument(hedgeCode, exp, hedgeMultiplier(hedgeCode));
        }
        // B14: Register secondary hedge instruments with OptionRisk
        for (const auto& shc : ec.secondaryHedgeCodes) {
            // B18 fix: pass the real contract multiplier (was hardcoded 1.0,
            // breaking unit consistency vs option greeks scaled by volScale)
            double mult = hedgeMultiplier(shc);
            _risk->registerHedgeInstrument(shc, exp, mult);
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "Secondary hedge registered: {} for expiry {} (mult={})", shc, exp, mult);
        }
    }

    // R10: resolve the primary hedge code = first enabled expiry's hedge
    for (const auto& [exp, ec] : _expiryConfigs) {
        if (!ec.enable) continue;
        std::string ul = ec.underlyingCode.empty() ? _underlyingCode : ec.underlyingCode;
        _primaryHedgeCode = ec.hedgeCode.empty() ? ul : ec.hedgeCode;
        break;
    }

    _grid->setOptionPricer(_pricer);
    // Only CompositeOptionPricer needs grid events (onAddOption/onComputeValuesCompleted)
    if (_compositePricer) {
        _grid->addListener(_compositePricer.get());
    }

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "HftOptionStrategy pricer+risk+OTG created (type={})", _pricerType);
}

// ============================================================================
// setupCTG — create ControllableTradingGrid + wire executors
// ============================================================================
void HftOptionStrategy::setupCTG()
{
    if (!_grid) return;

    _traderCtx = std::make_shared<wt_option::OptionTraderContext>();
    _traderCtx->enabled = false; // Enable on channel ready
    _traderCtx->panicked = false;
    // getTime → seconds-of-day (WT: HHMM + SSmmm). Used by CTG's TPS limiter,
    // which compares against a 1.0-second window, so it must be in seconds.
    _traderCtx->getTimeFn = [this]() {
        return _ctx ? _timeService.nowSeconds(_ctx) : 0.0;
    };

    _ctg = std::make_shared<wt_option::ControllableTradingGrid>(_grid, _traderCtx);
    _ctg->setOTG(_otg.get());
    _ctg->setMaxTransactionsPerSec(_maxTPS);

    // Wire executors to IHftStraCtx
    _ctg->setQuoteExecutor([this](const std::string& code, double bidP, uint32_t bidQ,
                                   double askP, uint32_t askQ) -> int32_t {
        return executeQuote(code, bidP, bidQ, askP, askQ);
    });

    _ctg->setOrderExecutor([this](const std::string& code, bool isBuy,
                                   double price, uint32_t qty) -> int32_t {
        return executeOrder(code, isBuy, price, qty);
    });

    _ctg->setCancelExecutor([this](const std::string& code) -> int32_t {
        return executeCancel(code);
    });

    // R8: mid-day stop/resume flows through the unified state machine
    _ctg->setSessionStopCallback([this](bool stopped) {
        if (stopped) {
            setState(wt_option::TradingStateType::SessionStopped, "session_stop");
        } else if (_tstate.type != wt_option::TradingStateType::Panic
                   && _tstate.type != wt_option::TradingStateType::Disabled) {
            // P1-e: session resume must neither downgrade Recovering to
            // Normal before the underlying gate clears, nor clear a risk
            // panic.
            setState(_tstate.underlyingReady ? wt_option::TradingStateType::Normal
                                             : wt_option::TradingStateType::Recovering,
                     "session_resume");
        }
    });

    // Wire PositionGuard/PositionOffsetMgr/RiskFilterChain to each OQM.
    // Build per-instrument RiskFilterChains from config (replaces hardcoded defaults).
    auto buildFilterChain = [this](const RiskFilterConfig& cfg) -> wt_option::RiskFilterChainPtr {
        if (!cfg.enabled) return nullptr;
        auto chain = std::make_shared<wt_option::RiskFilterChain>();
        chain->add(std::make_unique<wt_option::MaxOrderSizeFilter>(
            cfg.max_order_size, cfg.max_order_size_reject));
        chain->add(std::make_unique<wt_option::MinSellPriceFilter>(cfg.min_sell_price));
        if (cfg.max_position > 0) {
            chain->add(std::make_unique<wt_option::MaxPositionFilter>(
                cfg.max_position,
                static_cast<wt_option::MaxPositionFilter::Mode>(cfg.max_position_mode)));
        }
        chain->add(std::make_unique<wt_option::MaxCancelFilter>(
            cfg.max_cancel_soft, cfg.max_cancel_hard));
        chain->add(std::make_unique<wt_option::MaxNewOrdersFilter>(
            cfg.max_new_orders_hard_flat, cfg.max_new_orders_reject));

        // B3: short call/put aggregate limit (options only — the future chain
        // is built with right_flag=N/A and this filter no-ops there anyway;
        // we simply don't add it to the future chain for clarity)
        if (&cfg == &_optionFilterCfg &&
            (_riskLimitsEx.maxShortCallPerSymbol > 0 || _riskLimitsEx.maxShortPutPerSymbol > 0)) {
            auto provider = [this](bool isCall) -> int32_t {
                int32_t shorts = 0;
                if (!_grid) return 0;
                for (const auto& od : _grid->getAllOptions()) {
                    if (!od) continue;
                    bool c = (od->getRight() == wt_option::OR_Call);
                    if (c != isCall) continue;
                    int32_t p = static_cast<int32_t>(od->getPosition());
                    if (p < 0) shorts += -p;
                }
                return shorts;
            };
            chain->add(std::make_unique<wt_option::OptionsShortLimitFilter>(
                _riskLimitsEx.maxShortCallPerSymbol,
                _riskLimitsEx.maxShortPutPerSymbol, provider));
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "RiskFilterChain: OptionsShortLimit added (call<={}, put<={})",
                _riskLimitsEx.maxShortCallPerSymbol, _riskLimitsEx.maxShortPutPerSymbol);
        }
        return chain;
    };
    _optionFilterChain = buildFilterChain(_optionFilterCfg);
    _futureFilterChain = buildFilterChain(_futureFilterCfg);

    if (_optionFilterCfg.enabled || _futureFilterCfg.enabled) {
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "RiskFilterChain: option={} (filters={}), future={} (filters={})",
            _optionFilterCfg.enabled, _optionFilterChain ? _optionFilterChain->size() : 0,
            _futureFilterCfg.enabled, _futureFilterChain ? _futureFilterChain->size() : 0);
    }

    if (_otg) {
        // R-P0: keep the dependencies on OTG so contracts discovered later via
        // ticks receive the same filter/guard/offset wiring at creation time.
        _otg->setOptionRiskFilterChain(_optionFilterChain);
        _otg->setFutureRiskFilterChain(_futureFilterChain);
        _otg->setPositionGuardLookup([this](const std::string& code) -> wt_option::PositionGuardPtr {
            auto it = _positionGuards.find(code);
            return it != _positionGuards.end() ? it->second : nullptr;
        });
        _otg->setPositionOffsetLookup([this](const std::string& code) -> wt_option::PositionOffsetMgrPtr {
            auto it = _positionOffsets.find(code);
            return it != _positionOffsets.end() ? it->second : nullptr;
        });
        _otg->setOrderIssuedFn([this](const std::string& code, bool,
                                      double px, uint32_t qty, uint32_t localid) {
            // OQM normally sends via IHftStraCtx directly; mirror every issued
            // localid into the same guard/checker registration as Strategy orders.
            registerIssuedOrderIds(code, {localid}, px, qty);
        });

        for (const auto& pair : _otg->getAllUnderlyingTradingData()) {
            auto utd = pair.second;
            if (utd && utd->getQuoteManager()) {
                auto git = _positionGuards.find(pair.first);
                if (git != _positionGuards.end())
                    utd->getQuoteManager()->setPositionGuard(git->second);
                auto oit = _positionOffsets.find(pair.first);
                if (oit != _positionOffsets.end())
                    utd->getQuoteManager()->setPositionOffsetMgr(oit->second);
                if (_futureFilterChain)
                    utd->getQuoteManager()->setRiskFilterChain(_futureFilterChain);
            }
        }
        // Also wire option OTDs
        for (const auto& od : _grid->getAllOptions()) {
            if (!od) continue;
            auto otd = od->getTradingData();
            if (otd && otd->getQuoteManager()) {
                auto git = _positionGuards.find(od->getCode());
                if (git != _positionGuards.end())
                    otd->getQuoteManager()->setPositionGuard(git->second);
                auto oit = _positionOffsets.find(od->getCode());
                if (oit != _positionOffsets.end())
                    otd->getQuoteManager()->setPositionOffsetMgr(oit->second);
                if (_optionFilterChain)
                    otd->getQuoteManager()->setRiskFilterChain(_optionFilterChain);
            }
        }
    }

    // B17 fix: give PositionGuards a clock — without it the cooldown never
    // worked (now==0) and every breach re-alerted / re-disabled instantly
    for (auto& [gcode, guard] : _positionGuards) {
        if (guard)
            guard->setGetTimeFn([this]() { return _ctx ? _timeService.nowSeconds(_ctx) : 0.0; });
    }

    // B11: position source for OTD::getPosition() (QM_CLOSE / auto-close).
    // Reads the strategy's worker-thread-owned position map; all consumers
    // run on the same worker thread, so this is race-free.
    if (_otg) {
        _otg->setPositionSourceFn([this](const std::string& pcode) -> int32_t {
            auto it = _positions.find(pcode);
            return it != _positions.end() ? static_cast<int32_t>(it->second) : 0;
        });
    }

    WTSLogger::log_by_cat("strategy", LL_INFO, "HftOptionStrategy CTG created, tps={}", _maxTPS);
}

// ============================================================================
// setupAsyncCallbacks — wire async processor to grid/CTG
// ============================================================================
void HftOptionStrategy::setupAsyncCallbacks()
{
    wt_option::AsyncCallbacks cbs;

    // Per-code tick: route to hedge UTD or grid (worker thread)
    cbs.on_tick = [this](const std::string& code, const wt_option::TickData& tick) {
        bool isUnderlying = (code == _underlyingCode);
        bool isHedge = false;
        if (_otg) {
            auto utd = _otg->getUnderlyingTradingData(code);
            if (utd) {
                utd->setMarket(tick.bid, tick.ask, tick.updateTime);
                isHedge = true;
            }
        }
        // Check if this code is a per-expiry underlying (multi-series scenario)
        // If so, forward to grid for per-expiry price routing
        bool isPerExpiryUnderlying = _grid && _grid->isExpiryUnderlying(code);

        // Forward to grid: underlying OR per-expiry underlying OR option tick
        // (hedge-only ticks that are NOT per-expiry underlyings are not forwarded)
        if (!isHedge || isUnderlying || isPerExpiryUnderlying) {
            if (_grid) {
                wt_option::OptionGrid::TickDataRef ref;
                ref.price = tick.price;
                ref.bid = tick.bid;
                ref.ask = tick.ask;
                ref.bidQty = tick.bidQty;
                ref.askQty = tick.askQty;
                ref.expireDate = tick.expireDate;  // B28: exact expiry backfill
                ref.actionTime = tick.actionTime;
                ref.updateTime = tick.updateTime;
                _grid->onTick(code, ref);
            }

            // R1: remember the latest exchange price limits for this code
            if (tick.upperLimit > 0 || tick.lowerLimit > 0) {
                auto& pl = _priceLimits[code];
                if (tick.upperLimit > 0) pl.upper = tick.upperLimit;
                if (tick.lowerLimit > 0) pl.lower = tick.lowerLimit;
            }

            if (!isUnderlying && !isPerExpiryUnderlying)
                _dirtyOptions.insert(code);

            // Feed trade ticks to alpha signals (vega flow / delta flow)
            // Only for option ticks with non-zero trade volume
            if (!isUnderlying && !isPerExpiryUnderlying && tick.tradeVolume > 0) {
                auto od = _grid ? _grid->get(code) : nullptr;
                if (od && od->values(0).isPriced() && _compositePricer) {
                    for (auto& sig : _compositePricer->getAlphaSignals()) {
                        sig->onTradeTick(code, tick.price, tick.tradeVolume, od.get());
                    }
                }
            }

            // Any underlying tick (global or per-expiry) triggers compute
            if (isUnderlying || isPerExpiryUnderlying) {
                _underlyingChanged = true;
                _tstate.underlyingReady = true;

                // Recovery gate: after channel ready/restart we stay in
                // Recovering until broker snapshots have been processed and a
                // fresh underlying tick arrives.  Only then may quoting start.
                if (_channelReady && _tstate.type == wt_option::TradingStateType::Recovering) {
                    if (_otg) _otg->enableAll();
                    setState(wt_option::TradingStateType::Normal,
                             "recovery_first_underlying_tick");
                    WTSLogger::log_by_cat("strategy", LL_INFO,
                        "Recovery complete: trading activated on first underlying tick");
                }
            }
        }
        // First tick with preClose -> initialize PnlTracker for overnight positions
        if (tick.preClose > 0) {
            _preCloseCache[code] = tick.preClose;   // P1-5: seed for next session begin
            if (_pnlPendingInit.count(code)) {
                _pnlPendingInit.erase(code);
                double pos = _positions[code];
                if (_otg) {
                    auto utd = _otg->getUnderlyingTradingData(code);
                    if (utd) utd->getPnlTracker()->initPosition(pos, tick.preClose);
                    auto otd = _otg->getTradingData(code);
                    if (otd) otd->getPnlTracker()->initPosition(pos, tick.preClose);
                }
                if (pos != 0) {
                    WTSLogger::log_by_cat("strategy", LL_INFO,
                        "PnlTracker init {} pos={} preClose={}", code, pos, tick.preClose);
                }
            }
        }
    };

    // Batch start: set pricer time (use stra_get_time for FAST/SLOW scheduling)
    cbs.on_tick_batch = [this]() {
        _batchStartSteady = std::chrono::steady_clock::now();
        _batchStartValid = true;
        double timeSec = 0;
        if (_ctx) timeSec = _timeService.nowSeconds(_ctx);
        if (_compositePricer) {
            _compositePricer->setTime(timeSec);
            // B05/B19 fix: drive the signal clock — Toxicity windows, EMA decay
            // and recovery timers were frozen at t=0 without this.
            for (auto& sig : _compositePricer->getAlphaSignals())
                sig->setSignalTime(timeSec);
            for (auto& sig : _compositePricer->getRiskSignals())
                sig->setSignalTime(timeSec);
        }
        // Record tick batch timestamp for quote latency measurement
        if (_ctx) {
            // Use continuous event microseconds.  stra_get_secs() is SSmmm,
            // so multiplying it by 1e6 was not a valid timestamp.
            _tickTimestampUs = static_cast<uint64_t>(
                _timeService.nowSeconds(_ctx) * 1000000.0);
        }
        // A6: Notify scanners of underlying price update
        if (_grid && !_scanCoord.empty())
            _scanCoord.dispatchUnderlying(_grid->getUnderlyingPrice());
    };

    // Batch complete: computeValues + refresh + drainPendingQuotes (single pass per batch)
    cbs.on_batch_complete = [this]() {
        if (!_grid || !_pricer) return;
        publishAsyncVolFit();
        if (_hotUpdatePending.exchange(false, std::memory_order_acq_rel))
            applyHotParams();

        if (_batchStartValid) {
            const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - _batchStartSteady).count();
            _batchLatencyUs.add(static_cast<double>(elapsedUs));
            _batchStartValid = false;
        }

        ++_batchCount;
        if (_batchCount <= 5 || _batchCount % 100 == 0) {
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "HftOptionStrategy batch #{} enabled={} options={} strikes={} underlying={} p50={:.1f}us p95={:.1f}us p99={:.1f}us p99.9={:.1f}us",
                _batchCount, _traderCtx ? _traderCtx->enabled.load() : false,
                _grid->numOptions(), _grid->numStrikes(), _grid->getUnderlyingPrice(),
                _batchLatencyUs.p50(), _batchLatencyUs.p95(),
                _batchLatencyUs.p99(), _batchLatencyUs.p999());
        }

        // Market freshness gate: stale quotes are pulled once and no new quote
        // is generated until the underlying and option feeds are both fresh.
        const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        const bool marketFresh = isMarketFresh(nowMs);
        if (!marketFresh && _traderCtx && _traderCtx->enabled && !_marketStaleActive) {
            _marketStaleActive = true;
            WTSLogger::log_by_cat("strategy", LL_ERROR,
                "Market data stale (>{} ms); cancelling desired quotes and suspending refresh",
                _marketStaleTimeoutMs);
            if (_ctg) {
                _ctg->refresh();             // enabled=true here, so refresh stages CANCEL
                _ctg->drainPendingQuotes();  // unconditional drain sends those cancels
            }
        } else if (marketFresh && _marketStaleActive) {
            _marketStaleActive = false;
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "Market data fresh again; resuming quote refresh");
            _needsRefresh = true;
            _underlyingChanged = true;
        }

        if (_traderCtx && _traderCtx->enabled && marketFresh) {
            for (const auto& [code, pos] : _positions) {
                auto od = _grid->get(code);
                if (od) {
                    od->setPosition(pos);
                    // Sync option OQM position from broker position.
                    if (_otg) {
                        auto otd = _otg->getTradingData(code);
                        if (otd && otd->getQuoteManager())
                            otd->getQuoteManager()->setPosition(static_cast<int32_t>(pos));
                    }
                } else if (_otg) {
                    // Broker snapshots may describe hedge/future instruments that
                    // are not OptionData entries; keep their OQM position live too.
                    auto utd = _otg->getUnderlyingTradingData(code);
                    if (utd && utd->getQuoteManager())
                        utd->getQuoteManager()->setPosition(static_cast<int32_t>(pos));
                }
            }

            // P5: Fuse multiple getAllOptions/getAllUnderlyingTradingData passes
            // into a single pass each. Previously 4+ separate loops.
            double portfolioPnl = 0;

            double now = _timeService.nowSeconds(_ctx);
            const bool collectAttrs = _attrPub && _attrPub->shouldCollect(now);

            // Single pass over underlyings: PnlTracker update + portfolio PnL
            if (_otg) {
                for (const auto& pair : _otg->getAllUnderlyingTradingData()) {
                    auto utd = pair.second;
                    if (!utd) continue;
                    if (utd->isActive() && utd->getBid() > 0 && utd->getAsk() > 0)
                        utd->getPnlTracker()->onPriceUpdate(utd->getBid(), utd->getAsk());
                    portfolioPnl += utd->getPnlTracker()->getCurPnl();
                }
            }

            // Option loop: full portfolio PnL still needs every tracker's last
            // value, but expensive mark/scanner/attribute work is dirty-gated.
            const auto& allOpts = _grid->getAllOptions();
            for (const auto& od : allOpts) {
                if (!od) continue;
                auto otd = od->getTradingData();
                if (!otd) continue;
                const bool dirty = _dirtyOptions.count(od->getCode()) > 0;

                if (dirty && od->isActive() && od->getBid() > 0 && od->getAsk() > 0)
                    otd->getPnlTracker()->onPriceUpdate(od->getBid(), od->getAsk());
                portfolioPnl += otd->getPnlTracker()->getCurPnl();

                if (dirty && od->isActive())
                    _scanCoord.dispatchOption(od.get());
                if (collectAttrs)
                    _attrPub->collectOption(od->getCode(), otd.get(), od.get());
            }
            _dirtyOptions.clear();

            // Scanner grid-level dispatch
            _scanCoord.dispatchTick(_grid.get());

            // Attribute collection for underlyings only when publish is due.
            if (collectAttrs && _attrPub && _otg) {
                for (const auto& pair : _otg->getAllUnderlyingTradingData()) {
                    if (pair.second)
                        _attrPub->collectUnderlying(pair.first, pair.second.get());
                }
            }

            // Feed PnlLimitSignal with portfolio PnL
            if (_compositePricer) {
                for (auto& sig : _compositePricer->getRiskSignals()) {
                    auto pnlSig = std::dynamic_pointer_cast<wt_option::PnlLimitSignal>(sig);
                    if (pnlSig) pnlSig->setPortfolioPnl(portfolioPnl);
                }
            }

            // Underlying-driven compute scheduling (quantbox design):
            // Only reprice when the underlying book changed AND the debounce
            // interval has elapsed. Option-only ticks update market snapshots
            // but skip the expensive computeValues pass.
            // drainPendingQuotes still runs every batch to update quotes
            // based on the latest market data.
            bool shouldCompute = _underlyingChanged && (now - _lastComputeTime) >= _minComputeInterval;
            // Also compute if a trade/position/order event marked _needsRefresh
            // and the debounce interval has elapsed (even without underlying tick).
            if (_needsRefresh && !shouldCompute && (now - _lastComputeTime) >= _minComputeInterval) {
                shouldCompute = true;
            }
            if (shouldCompute) {
                _lastComputeTime = now;
                _underlyingChanged = false;
                _needsRefresh = false;

                // Refresh position Greeks before computeValues so that
                // risk_adjustment uses current-cycle position data (not stale).
                // OptionRisk::update() recomputes portfolio greeks from dirty flags
                // set by addFill/setHedgePosition. Without this, risk_adjustment
                // uses previous-cycle greeks (1-cycle lag for fills).
                if (_risk) _risk->update();

                // Phase 2: periodic scenario stress on current portfolio Greeks.
                if (_risk && now - _lastStressTime >= 60.0
                    && !_stress.scenarios().empty()) {
                    auto g = _risk->getPositionGreeks();
                    const double spot = _grid->getUnderlyingPrice();
                    if (g && spot > 0) {
                        auto rep = _stress.evaluateAll(spot, g->delta(), g->gamma(),
                                                       g->vega(), g->theta());
                        auto w = _stress.worst(spot, g->delta(), g->gamma(),
                                               g->vega(), g->theta());
                        WTSLogger::log_by_cat("strategy", LL_INFO,
                            "STRESS worst={:.2f} scenario={} (spot={:.2f} delta={:.2f} gamma={:.4f} vega={:.2f} theta={:.2f})",
                            w.pnl, w.scenario.name, spot, g->delta(), g->gamma(),
                            g->vega(), g->theta());
                        for (const auto& r : rep) {
                            WTSLogger::log_by_cat("strategy", LL_DEBUG,
                                "STRESS {} pnl={:.2f} spot_shock={:.2f} vol_shock={:.2f}",
                                r.scenario.name, r.pnl, r.scenario.spot_shock_pct,
                                r.scenario.vol_shock);
                        }
                        if (_stressLimit > 0 && w.pnl < -_stressLimit
                            && _tstate.type != wt_option::TradingStateType::Panic) {
                            WTSLogger::log_by_cat("strategy", LL_ERROR,
                                "STRESS limit breached: worst={:.2f} < -{:.2f}, PANIC",
                                w.pnl, _stressLimit);
                            setState(wt_option::TradingStateType::Panic, "stress_limit");
                        }
                        // Per-expiry view (IO expiries can have different hedge
                        // forwards); aggregate risk can hide a single-expiry tail.
                        for (const auto& [exp, ed] : _grid->expiries()) {
                            (void)ed;
                            auto eg = _risk->getExpiryGreeks(exp);
                            if (!eg) continue;
                            auto ew = _stress.worst(spot, eg->delta(), eg->gamma(),
                                                    eg->vega(), eg->theta());
                            WTSLogger::log_by_cat("strategy", LL_DEBUG,
                                "STRESS_EXP {} worst={:.2f} scenario={} delta={:.2f}",
                                exp, ew.pnl, ew.scenario.name, eg->delta());
                        }
                        _lastStressTime = now;
                    }
                }

                if (_compositePricer)
                    _compositePricer->setMarketTime(nowMs, _marketStaleTimeoutMs);
                _grid->computeValues(_pricer.get());

                // Propagate pricer panic state to the CTG context.
                // B13 fix: OR-merge with FillPriceChecker's latched panic —
                // the unconditional overwrite used to clear it after 1 cycle.
                if (_traderCtx && _pricer) {
                    bool wasPanicked = _traderCtx->panicked;
                    bool pricerPanic = _pricer->isPanicked();
                    bool checkerPanic = _fillPriceChecker && _fillPriceChecker->isPanicked();
                    const bool wantPanic = pricerPanic || checkerPanic
                         || _limitsBreached.load(std::memory_order_relaxed);
                    if (wantPanic && _tstate.type != wt_option::TradingStateType::Disabled)
                        setState(wt_option::TradingStateType::Panic,
                                 checkerPanic ? "fill_price_deviation" : "risk_signal");
                    else if (!wantPanic && _tstate.type == wt_option::TradingStateType::Panic
                             && !pricerPanic)
                        setState(wt_option::TradingStateType::Normal, "blackout_expired");
                    if (_traderCtx->panicked && !wasPanicked) {
                        WTSLogger::log_by_cat("strategy", LL_ERROR,
                            "Auto-panic triggered by risk signal{}{}",
                            checkerPanic ? " (fill price deviation)" : "",
                            _limitsBreached.load(std::memory_order_relaxed) ? " (limits breach)" : "");
                    } else if (!_traderCtx->panicked && wasPanicked) {
                        WTSLogger::log_by_cat("strategy", LL_INFO,
                            "Panic blackout expired, resuming");
                    }
                }

                // A2: periodic Greeks / daily-loss limits (non-hot-path)
                checkRiskLimitsEx();

                // refresh() is auto-triggered by OptionGrid::__notifyComputeCompleted
                // -> CTG::onComputeValuesCompleted -> refresh(). No explicit call needed.
            }

            // B7: Publish attributes (already collected in fused pass above)
            if (_attrPub) {
                _attrPub->publish(now);
            }

            // B1: estimated-margin guard (runs on its own throttle, even
            // without compute, so a margin breach is caught in quiet markets)
            checkMarginLimits(now);

            // R10: delta hedge executor (own cooldown inside decide())
            checkDeltaHedge(now);
            // R-P1: periodic accuracy report
            _predEvaluator.maybeReport(60.0);

            // B10: Write option values to CSV if configured
            if (_valWriter && _grid) {
                if (_valWriter->shouldWrite(now)) {
                    _valWriter->writeValues(_grid.get(), now);
                }
            }

            // B11: Trigger predictor updates
            if (_triggerEngine) {
                double now = _timeService.nowSeconds(_ctx);
                _triggerEngine->onTick(now);
            }

            // Check combo order timeouts and clean up completed
            if (!_activeCombos.empty()) {
                std::vector<wt_option::ComboOrderPtr> stillActive;
                for (auto& combo : _activeCombos) {
                    // Check timeout for spread combos
                    auto spread = std::dynamic_pointer_cast<wt_option::SpreadComboOrder>(combo);
                    if (spread && spread->isActive() && spread->checkTimeout()) {
                        spread->cancelAll();
                    }
                    auto syn = std::dynamic_pointer_cast<wt_option::SynComboOrder>(combo);
                    if (syn && syn->isActive() && syn->checkTimeout()) {
                        syn->cancelAll();
                    }
                    auto sim = std::dynamic_pointer_cast<wt_option::SimulComboOrder>(combo);
                    if (sim && sim->isActive() && sim->checkTimeout()) {
                        sim->cancelAll();
                    }
                    // Keep active combos AND recently failed/timed-out combos
                    // until every leg/top-up/unwind id reaches terminal state.
                    bool active = (spread && (spread->isActive() || spread->hasOpenOrders()))
                               || (syn && (syn->isActive() || syn->hasOpenOrders()))
                               || (sim && (sim->isActive() || sim->hasOpenOrders()));
                    if (active) stillActive.push_back(combo);
                }
                _activeCombos = std::move(stillActive);
            }

            if (_ctg) _ctg->drainPendingQuotes();
        }
    };

    // Trade callback
    cbs.on_trade = [this](const std::string& code, uint32_t localid,
                           bool isBuy, double vol, double price) {
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "HftOptionStrategy trade: {} {} {}@{}", code, isBuy?"BUY":"SELL", vol, price);
        uint32_t fq = static_cast<uint32_t>(vol);

        // B2: classify the fill against issued-order records FIRST. Unknown
        // fills still update positions/risk (money moved!) but skip the OQM
        // tracker so a bogus report cannot corrupt order bookkeeping.
        auto fillClass = _anomalyGuard.onFill(localid, fq);
        const bool skipOqm = (fillClass == wt_option::OrderAnomalyGuard::FillClass::Unknown);

        _positionBook.addFill(code, isBuy, vol);
        _positions[code] = _positionBook.net(code);
        _dirtyOptions.insert(code);

        // Forward to OQM (per-contract order tracker).  Futures use UTD/OQM;
        // leaving them unrouted leaves hedge order state permanently stale.
        if (_otg && !skipOqm) {
            auto otd = _otg->getTradingData(code);
            if (otd && otd->getQuoteManager()) {
                // Set current time for QuoteStatistics
                // P2-1: stra_get_secs() returns SSmmm (sec*1000+msec);
                // sec_in_min needs (secs/1000)%60 — raw %60 mis-buckets
                // (e.g. 45123%60=3 instead of 45).
                if (_ctx) {
                    uint32_t curTime = _ctx->stra_get_time();
                    uint32_t secs = _ctx->stra_get_secs();
                    otd->getQuoteManager()->setCurrentTime(curTime, (secs / 1000) % 60);
                }
                otd->getQuoteManager()->onFill(localid, isBuy, price, fq);
            } else {
                auto utd = _otg->getUnderlyingTradingData(code);
                if (utd && utd->getQuoteManager()) {
                    if (_ctx) {
                        uint32_t curTime = _ctx->stra_get_time();
                        uint32_t secs = _ctx->stra_get_secs();
                        // P2-1: SSmmm format — see on_trade routing above
                        utd->getQuoteManager()->setCurrentTime(curTime, (secs / 1000) % 60);
                    }
                    utd->getQuoteManager()->onFill(localid, isBuy, price, fq);
                }
            }
        }

        // Forward to Pricer onFill (trade-shock back-away + riskShift)
        if (_compositePricer) {
            auto stub = std::make_shared<wt_option::OrderStub>();
            stub->code = code;
            stub->dir = isBuy ? 0 : 1;  // 0=buy, 1=sell
            stub->fillPrice = price;
            _compositePricer->triggerOnFill(stub, price, static_cast<uint32_t>(vol));
        }

        // Enhancement: FillPriceChecker - monitor fill price deviation
        if (_fillPriceChecker)
            _fillPriceChecker->onFill(code, localid, price);

        // Enhancement: PositionGuard - track internal position for discrepancy detection
        auto git = _positionGuards.find(code);
        if (git != _positionGuards.end())
            git->second->onFill(isBuy, static_cast<uint32_t>(vol));

        // Enhancement: PositionOffsetMgr - update offset tracking
        auto oit = _positionOffsets.find(code);
        if (oit != _positionOffsets.end())
            oit->second->onFill(isBuy, static_cast<uint32_t>(vol), false);

        // Update OptionRisk position
        if (_risk) {
            auto rd = _risk->get(code);
            if (rd) {
                rd->addFill((isBuy ? 1 : -1) * static_cast<int32_t>(vol), price);
            }
            // Update hedge position if this is a hedge instrument
            _risk->setHedgePosition(code, static_cast<int32_t>(_positions[code]));
        }

        // Feed PnlTracker for hedge and option instruments
        if (_otg) {
            auto utd = _otg->getUnderlyingTradingData(code);
            if (utd) utd->getPnlTracker()->onFill(isBuy, static_cast<uint32_t>(vol), price);
            auto otd = _otg->getTradingData(code);
            if (otd) otd->getPnlTracker()->onFill(isBuy, static_cast<uint32_t>(vol), price);
        }
        // Feed risk signals
        if (_compositePricer) {
            // B05: refresh signal clock before fill-driven updates
            double nowSec = _ctx ? _timeService.nowSeconds(_ctx) : 0;
            for (auto& sig : _compositePricer->getRiskSignals()) {
                sig->setSignalTime(nowSec);
                sig->onFill(code, isBuy, vol, price);
            }
        }

        // B04 fix: route fills to active combo orders (was an empty loop —
        // leg2/hedge leg was never sent, leaving naked directional exposure)
        if (!_activeCombos.empty()) {
            uint32_t fq = static_cast<uint32_t>(vol);
            uint64_t tsMs = static_cast<uint64_t>(
                (_ctx ? _timeService.nowSeconds(_ctx) : 0.0) * 1000);
            for (auto& combo : _activeCombos) {
                auto spread = std::dynamic_pointer_cast<wt_option::SpreadComboOrder>(combo);
                if (spread && spread->onLegFill(localid, fq)) {
                    spread->legOrderFilled(localid, price, fq, tsMs);   // P1-d/W5 audit record
                    continue;
                }
                auto syn = std::dynamic_pointer_cast<wt_option::SynComboOrder>(combo);
                if (syn && syn->onLegFill(localid, fq)) {
                    syn->legOrderFilled(localid, price, fq, tsMs);
                    continue;
                }
                auto sim = std::dynamic_pointer_cast<wt_option::SimulComboOrder>(combo);
                if (sim && sim->onLegFill(localid, fq)) {
                    sim->legOrderFilled(localid, price, fq, tsMs);
                    continue;
                }
            }
        }

        // B9: Feed expiration simulator
        // B06 fix: pass the real per-contract fee (was literal 0) so simulated
        // PnL includes commissions; record fill time for diagnostics.
        if (_expSim) {
            double fee = 0;
            if (_grid) {
                auto odEx = _grid->get(code);
                if (odEx) fee = odEx->getFee() * vol;
            }
            uint64_t tsMs = static_cast<uint64_t>(
                (_ctx ? _timeService.nowSeconds(_ctx) : 0.0) * 1000);
            _expSim->onFill(code, isBuy, price, static_cast<uint32_t>(vol), fee, tsMs);
        }

        // Mark for refresh: trade affects risk/position, need to recompute ourMarket
        _needsRefresh = true;
    };

    // Order callback
    cbs.on_order = [this](const std::string& code, uint32_t localid, bool isBuy,
                           double totalQty, double leftQty, double price, bool isCanceled) {
        // Delta-hedge in-flight bookkeeping: release the slot on terminal state.
        if (isCanceled || leftQty <= 0) {
            _hedgeCoord.onTerminal(localid);
            if (_gammaPricer) _gammaPricer->onOrderTerminal(localid);
        }
        // Combo terminal routing: a canceled leg must reach the combo state
        // machine, otherwise a leg2 cancel silently leaves leg1 exposed.
        if (isCanceled && !_activeCombos.empty()) {
            for (auto& combo : _activeCombos) {
                auto spread = std::dynamic_pointer_cast<wt_option::SpreadComboOrder>(combo);
                if (spread) {
                    spread->onLegTerminal(localid, true, static_cast<uint32_t>(leftQty));
                    spread->legOrderTerminal(localid, false);   // P1-d/W5 audit record
                }
                auto syn = std::dynamic_pointer_cast<wt_option::SynComboOrder>(combo);
                if (syn) {
                    syn->onLegTerminal(localid, true, static_cast<uint32_t>(leftQty));
                    syn->legOrderTerminal(localid, false);
                }
                auto sim = std::dynamic_pointer_cast<wt_option::SimulComboOrder>(combo);
                if (sim) {
                    sim->onLegTerminal(localid, true, static_cast<uint32_t>(leftQty));
                    sim->legOrderTerminal(localid, false);
                }
            }
        }
        if (isCanceled) {
            WTSLogger::log_by_cat("strategy", LL_DEBUG,
                "HftOptionStrategy order canceled: {} id={}", code, localid);
        }

        // B2: record done/cancel in the anomaly guard
        _anomalyGuard.onOrderDone(localid, isCanceled, leftQty);

        // Forward to OQM (order status tracking), including future OQMs.
        if (_otg) {
            wt_option::OptionQuoteManager* qm = nullptr;
            auto otd = _otg->getTradingData(code);
            if (otd) qm = otd->getQuoteManager().get();
            if (!qm) {
                auto utd = _otg->getUnderlyingTradingData(code);
                if (utd) qm = utd->getQuoteManager().get();
            }
            if (qm) {
                qm->setTickTimestampUs(_tickTimestampUs);
                // Set current time for QuoteStatistics (HHMM + sec_in_min)
                if (_ctx) {
                    uint32_t curTime = _ctx->stra_get_time();
                    uint32_t secs = _ctx->stra_get_secs();
                    qm->setCurrentTime(curTime, (secs / 1000) % 60);   // P2-1: SSmmm
                }
                qm->onOrderStatusChange(localid, isBuy, totalQty, leftQty, price, isCanceled);
            }
        }

        // Enhancement: PositionOffsetMgr - track sent/cancelled orders
        // for frozen position tracking (close-today/prev availability)
        {
            auto oit = _positionOffsets.find(code);
            if (oit != _positionOffsets.end()) {
                if (isCanceled) {
                    // Order was cancelled - free the REMAINING frozen qty.
                    // P1-4: totalQty-leftQty is the FILLED portion (already
                    // unfrozen by onFill); freezing happened for the full
                    // qty at send.  Passing the filled count would leave
                    // (leftQty) permanently frozen once sent-tracking is
                    // wired into the executor path.
                    oit->second->onOrderCancelled(isBuy,
                        static_cast<uint32_t>(leftQty), false);
                }
                // Note: onOrderSent (freeze at send) is only reachable with
                // open/close intent, which the engine callback chain does not
                // carry — the frozen counters stay 0 until that plumbing
                // exists, so today/prev buckets are driven by broker position
                // snapshots only.
            }
        }

        // B14 fix: retire FillPriceChecker entries for finished orders so the
        // map does not grow unboundedly with fully-filled quotes
        if (_fillPriceChecker) {
            if (isCanceled)
                _fillPriceChecker->onOrderCancelled(localid);
            else if (leftQty <= 0)
                _fillPriceChecker->onOrderCompleted(localid);
        }

        // Mark for refresh: order status change affects OQM state
        _needsRefresh = true;
    };

    // P1-1: entrust failure (worker thread).  WT sends NO terminal on_order
    // after on_entrust(bSuccess=false), so the OQM OrderState must be cleared
    // here — otherwise the phantom order blocks its price level forever
    // (getMissingPriceLevelSize sees live size => missing==0 => never re-sent)
    // and the reject-retry mechanism never triggers.
    cbs.on_entrust = [this](uint32_t localid, bool success) {
        if (success) return;
        // A rejected hedge order must not leave the in-flight latch stuck.
        _hedgeCoord.onTerminal(localid);
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "HftOptionStrategy entrust failed: id={}", localid);
        // P1-d/W5: rejected combo legs go terminal in the audit records too.
        for (auto& combo : _activeCombos)
            combo->legOrderTerminal(localid, true);
        // Clear the phantom order in whichever OQM owns it (option or future).
        bool cleared = false;
        if (_grid) {
            for (const auto& od : _grid->getAllOptions()) {
                if (!od) continue;
                auto otd = od->getTradingData();
                if (otd && otd->getQuoteManager() &&
                    otd->getQuoteManager()->onOrderRejected(localid)) {
                    cleared = true; break;
                }
            }
        }
        if (!cleared && _otg) {
            for (const auto& pair : _otg->getAllUnderlyingTradingData()) {
                auto utd = pair.second;
                if (utd && utd->getQuoteManager() &&
                    utd->getQuoteManager()->onOrderRejected(localid)) {
                    cleared = true; break;
                }
            }
        }
        if (!cleared) {
            WTSLogger::log_by_cat("strategy", LL_DEBUG,
                "entrust failure for unowned localid={} (manual/hot order?)", localid);
        }
        _needsRefresh = true;
    };

    // Timer callback - trigger SLOW compute + session scheduling
    cbs.on_timer = [this](uint32_t curDate, uint32_t curTime) {
        // B4: Session-based mid-day stop/resume scheduling.
        // Uses the underlying commodity WTSSessionInfo sections to detect
        // section boundaries. Stops quoting stopLeadSeconds before a section
        // ends, resumes resumeLagSeconds after a section starts.
        if (_sessionInfo && _ctg && _traderCtx) {
            uint32_t hh = curTime / 100;
            uint32_t mm = curTime % 100;
            uint32_t ss = 0;
            if (_ctx) {
                uint32_t ssms = _ctx->stra_get_secs();
                ss = ssms / 1000;
            }
            uint32_t curSec = hh * 3600 + mm * 60 + ss;

            const auto& sections = _sessionInfo->getTradingSections();
            for (const auto& sec : sections) {
                uint32_t startSec = (sec.first_raw / 100) * 3600 + (sec.first_raw % 100) * 60;
                uint32_t endSec   = (sec.second_raw / 100) * 3600 + (sec.second_raw % 100) * 60;

                // Stop stopLeadSeconds before section end
                if (!_sessionStopped && curSec >= (endSec - _stopLeadSeconds) && curSec < endSec) {
                    _ctg->tradingStopMidDay();
                    _sessionStopped = true;
                    WTSLogger::log_by_cat("strategy", LL_WARN,
                        "Session stop: section {}-{}, curSec={} lead={}s",
                        sec.first_raw, sec.second_raw, curSec, _stopLeadSeconds);
                }
                // Resume resumeLagSeconds after section start
                if (_sessionStopped && curSec >= (startSec + _resumeLagSeconds)
                    && curSec < (endSec - _stopLeadSeconds)) {
                    _ctg->resumeTrading();
                    _sessionStopped = false;
                    WTSLogger::log_by_cat("strategy", LL_INFO,
                        "Session resume: section {}-{}, curSec={} lag={}s",
                        sec.first_raw, sec.second_raw, curSec, _resumeLagSeconds);
                }
            }
        }

        // Path 1: periodic vol-fit. async mode (default): build snapshots and
        // let SingleFlightTaskRunner fit off-thread; sync mode (fitter.mode:
        // "sync"): drive the legacy PeriodicCurveFitter on the worker thread
        // (backtest determinism / fit debugging).
        if (_traderCtx && _traderCtx->enabled) {
            if (_volFitMode == "sync") {
                if (_blackPricer && _blackPricer->triggerDoFit())
                    WTSLogger::log_by_cat("strategy", LL_DEBUG, "PeriodicCurveFitter fit completed");
            } else if (submitAsyncVolFit()) {
                WTSLogger::log_by_cat("strategy", LL_DEBUG, "AsyncVolFitter snapshot submitted");
            }
        }

        // Pricer timeout retry (equivalent to onComputeValuesTimeout):
        // If the underlying hasn't changed but interval elapsed, force a
        // recompute to catch vol curve / time decay changes.
        if (_grid && _pricer && _traderCtx && _traderCtx->enabled) {
            double now = _timeService.nowSeconds(_ctx);
            if ((now - _lastComputeTime) >= _minComputeInterval * 5) {
                _lastComputeTime = now;
                _underlyingChanged = false;
                if (_compositePricer) {
                    const uint64_t nowMs = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                    _compositePricer->setMarketTime(nowMs, _marketStaleTimeoutMs);
                }
                _grid->computeValues(_pricer.get());
            }
        }

        // B4: Retry dropped quotes - if CTG has pending dropped quotes
        // from a previous TPS-limited cycle, trigger a refresh to process them.
        // Note: refresh() here also handles the case where computeValues was
        // triggered by onFitCompleted (above) via listener auto-refresh, but
        // the pending quotes may need draining. drainPendingQuotes is always
        // safe to call (no-op if empty).
        if (_ctg && _traderCtx && _traderCtx->enabled) {
            _ctg->drainPendingQuotes();
        }
    };

    // Session callback
    cbs.on_session = [this](uint32_t tdate, bool isBegin) {
        if (isBegin) {
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "HftOptionStrategy session begin: {}", tdate);
            // Rebase the continuous clock at the session boundary.  Overnight
            // sessions keep the same trading date, so the wrap detector in
            // OptionTimeService is responsible for the actual midnight jump.
            _timeService.reset();
            _tstate.underlyingReady = false;
            setState(_channelReady.load() ? wt_option::TradingStateType::Recovering
                                          : wt_option::TradingStateType::Disabled,
                     "session_begin");
            if (_grid) {
                _grid->setCurrentDate(tdate);
                // Refresh days-to-expiration for all expiries (fixes stale maturity)
                // Phase 2: ExpiryLifecycle owns refresh/purge/roll.
                wt_option::OptionExpiryLifecycle::onSessionBegin(*_grid);
            }
            // B07 fix: reset OQM lifetime counters — without this, MaxCancel /
            // MaxNewOrders / hard_flat thresholds permanently lock all quoting
            if (_otg) _otg->onSessionBegin();

            // B20b: reset per-instrument PnL ledgers at the session boundary.
            // Signal latches alone are not enough; otherwise a long-running
            // process carries yesterday's realized PnL into today's limit.
            // P1-5: seed the carried position + last known preClose so fills
            // that arrive BEFORE the first tick of the day realize against
            // the overnight basis instead of 0.
            if (_otg) {
                for (const auto& pair : _otg->getAllUnderlyingTradingData()) {
                    if (pair.second) {
                        int32_t carried = _positions[pair.first];
                        double basis = 0.0;
                        auto pcIt = _preCloseCache.find(pair.first);
                        if (pcIt != _preCloseCache.end()) basis = pcIt->second;
                        pair.second->getPnlTracker()->resetSession(carried, basis);
                    }
                }
            }
            if (_grid) {
                for (const auto& od : _grid->getAllOptions()) {
                    if (od && od->getTradingData()) {
                        int32_t carried = _positions[od->getCode()];
                        double basis = 0.0;
                        auto pcIt = _preCloseCache.find(od->getCode());
                        if (pcIt != _preCloseCache.end()) basis = pcIt->second;
                        od->getTradingData()->getPnlTracker()->resetSession(carried, basis);
                    }
                }
            }
            // Re-arm cost-basis initialization for carried positions.  The next
            // preClose tick will seed each tracker with the new session basis.
            for (const auto& [posCode, posVal] : _positions)
                _pnlPendingInit.insert(posCode);

            // B2: clear anomaly-guard registries for the new session
            _anomalyGuard.reset();
            // B20 fix: clear risk-signal latches (PnlLimit panic used to
            // survive across sessions until process restart)
            if (_compositePricer) {
                for (auto& sig : _compositePricer->getRiskSignals())
                    sig->reset();
            }
            // A6: Start scanners
            _scanCoord.startAll();
            // B9: Clear expiration simulator / attribute maps for new session
            if (_expSim) _expSim->clear();
            if (_attrPub) _attrPub->clear();
            // B10: Initialize value writer for new session
            if (_valWriter) _valWriter->init(tdate);
            // QuoteStatistics: reset for new session
            _quoteStats.onSessionBegin(tdate);
        } else {
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "HftOptionStrategy session end: {}", tdate);
            setState(wt_option::TradingStateType::Disabled, "session_end");
            // Phase 2: ExpiryLifecycle owns the session-end hard stop.
            wt_option::OptionExpiryLifecycle::onSessionEnd(_ctx, _positions,
                [](double gross, size_t codes) {
                    WTSLogger::log_by_cat("strategy", LL_ERROR,
                        "HftOptionStrategy session_end: non-zero gross position {:.0f} lots across {} codes (manual/next-session closeout required)",
                        gross, codes);
                });
            // A6: Stop scanners
            _scanCoord.stopAll();
            // B9: ExpirationSimulator — run the daily settlement before the
            // summary so it has data (was permanently "0 day results"):
            // settlement PnL for contracts that expired today, mark-to-mid
            // for still-open ones; trading PnL comes from recorded fills.
            // P2-4: pass per-expiry underlying prices (multi-series setups
            // settled non-front expiries off the GLOBAL underlying before).
            if (_expSim) {
                _expSim->checkExpiration(tdate,
                    _grid ? _grid->getUnderlyingPrice() : 0.0,
                    _grid ? &_grid->expiries() : nullptr);
                _expSim->printSummary();
            }
            // B10: Close value writer at session end
            if (_valWriter) _valWriter->close();
            // QuoteStatistics: print session summary
            _quoteStats.onSessionEnd();
            // C5: dump OQM lifetime counters for exchange-report reconciliation
            if (_otg) {
                std::string path = fmt::format("outputs_option/oqm_counters_{}.csv", tdate);
                _otg->dumpCountersCsv(path);
                WTSLogger::log_by_cat("strategy", LL_INFO,
                    "OQM counters dumped to {}", path);
            }
        }
    };

    // A1: Position callback — full four-tuple now flows through the async
    // processor (prevol/preavail were previously dropped on the floor)
    cbs.on_position = [this](const std::string& code, bool isLong,
                             double prevol, double preavail,
                             double newvol, double newavail) {
        // PositionBook is authoritative and keeps long/short plus prev/new
        // availability.  `_positions` remains as a legacy net projection.
        _positionBook.onBrokerPosition(code, isLong, prevol, preavail, newvol, newavail);
        const auto& fullPos = _positionBook.get(code);
        const double pos = fullPos.net();
        _positions[code] = pos;

        if (_risk) {
            // Initialize option RiskData from the broker snapshot as well as
            // hedge data; otherwise restart/overnight option Greeks are empty.
            auto rd = _risk->get(code);
            if (rd)
                rd->setPosition(static_cast<int32_t>(pos));
            _risk->setHedgePosition(code, static_cast<int32_t>(pos));
        }
        _pnlPendingInit.insert(code);

        // Enhancement: PositionGuard - consume the authoritative full snapshot
        auto git = _positionGuards.find(code);
        if (git != _positionGuards.end())
            git->second->onBrokerPosition(fullPos);

        // Offset manager receives long/short + today/prev availability split.
        auto oit = _positionOffsets.find(code);
        if (oit != _positionOffsets.end())
            oit->second->onPositionUpdate(fullPos);

        // Mark for refresh: position changes affect risk/quote sizing
        _needsRefresh = true;
    };

    // A3: Channel callback (processed in worker thread)
    cbs.on_channel = [this](bool isReady) {
        if (isReady) {
            WTSLogger::log_by_cat("strategy", LL_INFO, "channel_ready (worker)");
            // Restart/reconnect recovery: do not quote immediately.  Wait for
            // broker position snapshots and a fresh underlying tick, and sweep
            // any orders that survived the restart before enabling trading.
            _tstate.channelReady = true;
            _tstate.underlyingReady = false;
            _positionBook.clearAll();
            _positions.clear();
            setState(wt_option::TradingStateType::Recovering, "channel_ready");
            if (_ctx) {
                _ctx->stra_cancel_all("");  // exchange-side recovery sweep
                WTSLogger::log_by_cat("strategy", LL_WARN,
                    "Recovery: exchange order sweep issued; waiting for broker snapshots and first underlying tick");
            }
        } else {
            WTSLogger::log_by_cat("strategy", LL_WARN, "channel_lost (worker)");
            _tstate.channelReady = false;
            _tstate.underlyingReady = false;
            setState(wt_option::TradingStateType::Disabled, "channel_lost");
            if (_otg) _otg->disableAll();
            // Cancel all existing quotes on channel loss
            if (_ctg) {
                _ctg->refresh();
                _ctg->drainPendingQuotes();
            }
        }
    };

    _async->setCallbacks(cbs);
}

// ============================================================================
// setupSignals — config-driven selective signal loading
// ============================================================================
void HftOptionStrategy::setupSignals()
{
    if (!_pricer || !cfgPtr_) return;

    WTSVariant* sigCfg = cfgPtr_->get("signals");
    if (!sigCfg) {
        WTSLogger::log_by_cat("strategy", LL_INFO, "No signals config, using hardcoded alpha");
        return;
    }

    std::vector<wt_option::IAlphaSignal::Ptr> alphaSignals;
    WTSVariant* alphaArr = sigCfg->get("alpha");
    if (alphaArr && alphaArr->isArray()) {
        for (uint32_t i = 0; i < alphaArr->size(); i++) {
            WTSVariant* s = alphaArr->get(i);
            if (!s->getBoolean("enable")) continue;
            std::string type = s->getCString("type");
            auto sig = wt_option::SignalFactory::instance().createAlpha(type);
            if (sig) {
                sig->init(s->get("params"));
                alphaSignals.push_back(sig);
                WTSLogger::log_by_cat("strategy", LL_INFO,
                    "Alpha signal loaded: {} weight={}", type, sig->getWeight());
            } else {
                WTSLogger::log_by_cat("strategy", LL_WARN,
                    "Alpha signal not found: {}", type);
            }
        }
    }

    std::vector<wt_option::IRiskSignal::Ptr> riskSignals;
    WTSVariant* riskArr = sigCfg->get("risk");
    if (riskArr && riskArr->isArray()) {
        for (uint32_t i = 0; i < riskArr->size(); i++) {
            WTSVariant* s = riskArr->get(i);
            if (!s->getBoolean("enable")) continue;
            std::string type = s->getCString("type");
            auto sig = wt_option::SignalFactory::instance().createRisk(type);
            if (sig) {
                sig->init(s->get("params"));
                riskSignals.push_back(sig);
                WTSLogger::log_by_cat("strategy", LL_INFO, "Risk signal loaded: {}", type);
            } else {
                WTSLogger::log_by_cat("strategy", LL_WARN,
                    "Risk signal not found: {}", type);
            }
        }
    }

    if (_compositePricer) {
        _compositePricer->setAlphaSignals(std::move(alphaSignals));
        _compositePricer->setRiskSignals(std::move(riskSignals));
    }
}

// ============================================================================
// setupScanners - A6: Create scanners from config and wire to CTG
// ============================================================================
void HftOptionStrategy::setupScanners()
{
    if (!cfgPtr_ || !_ctg) return;

    // Initialize combo execution context (bridges scanners to IHftStraCtx)
    _comboCtx.sendOrder = [this](const std::string& code, bool isBuy,
                                   double price, uint32_t qty) -> uint32_t {
        // Scanner orders now use the same gateway as market-making orders.
        const auto ids = executeOrderIds(code, isBuy, price, qty);
        return ids.empty() ? 0 : ids[0];
    };
    _comboCtx.cancelOrder = [this](uint32_t localid) -> bool {
        return executeCancel(localid);
    };
    _comboCtx.getTime = [this]() -> double {
        return _timeService.nowSeconds(_ctx);
    };

    WTSVariant* scannersCfg = cfgPtr_->has("scanners") ? cfgPtr_->get("scanners") : nullptr;
    if (!scannersCfg || !scannersCfg->isArray() || scannersCfg->size() == 0) {
        WTSLogger::log_by_cat("strategy", LL_INFO, "No scanners configured");
        return;
    }

    for (uint32_t i = 0; i < scannersCfg->size(); i++) {
        WTSVariant* sCfg = scannersCfg->get(i);
        if (sCfg->has("enable") && !sCfg->getBoolean("enable")) continue;  // B09 fix: was inverted

        std::string type = sCfg->getCString("type");
        wt_option::ScannerConfig config;
        config.name = sCfg->has("name") ? sCfg->getCString("name") : type;
        config.enabled = sCfg->has("enable") ? sCfg->getBoolean("enable") : true;
        config.priority = sCfg->has("priority") ? sCfg->getInt32("priority") : 0;
        // R6 follow-up: actually honor per-scanner numeric params instead of
        // silently running code defaults.
        config.min_edge = sCfg->has("min_edge") ? sCfg->getDouble("min_edge") : 0.5;
        config.delta_min = sCfg->has("delta_min") ? sCfg->getDouble("delta_min") : 0.1;
        config.delta_max = sCfg->has("delta_max") ? sCfg->getDouble("delta_max") : 0.9;
        config.max_spread = sCfg->has("max_spread") ? sCfg->getDouble("max_spread") : 10.0;
        config.min_size = sCfg->has("min_size") ? sCfg->getInt32("min_size") : 1;
        config.garch_omega = sCfg->has("garch_omega") ? sCfg->getDouble("garch_omega") : 1e-10;
        config.garch_alpha = sCfg->has("garch_alpha") ? sCfg->getDouble("garch_alpha") : 0.08;
        config.garch_beta = sCfg->has("garch_beta") ? sCfg->getDouble("garch_beta") : 0.90;
        config.warmup_returns = sCfg->has("warmup_returns") ? sCfg->getInt32("warmup_returns") : 50;
        config.annualization_factor = sCfg->has("annualization_factor") ? sCfg->getDouble("annualization_factor") : 60480.0;

        // Parse per-expiry overrides
        if (sCfg->has("expiryOverrides")) {
            WTSVariant* overrides = sCfg->get("expiryOverrides");
            if (overrides && overrides->isArray()) {
                for (uint32_t j = 0; j < overrides->size(); j++) {
                    WTSVariant* ov = overrides->get(j);
                    uint32_t exp = ov->getUInt32("expiry");
                    wt_option::ScannerExpiryOverrides eo;
                    eo.enabled = ov->has("enabled") ? ov->getBoolean("enabled") : true;
                    eo.maxPosOpt = ov->has("maxPosOpt") ? ov->getInt32("maxPosOpt") : -1;
                    eo.maxPosFut = ov->has("maxPosFut") ? ov->getInt32("maxPosFut") : -1;
                    config.expiryOverrides[exp] = eo;
                }
            }
        }

        auto scanner = wt_option::ScannerFactory::instance().createScanner(type, config);
        if (scanner) {
            scanner->setEnabled(config.enabled);  // B09 fix: was unconditional setEnabled(true)
            for (const auto& [exp, ov] : config.expiryOverrides)
                scanner->setExpiryEnabled(exp, ov.enabled);
            _scanCoord.add(scanner);
            _ctg->addScanner(scanner);
            // CTG implements IScannerListener, register it
            scanner->addListener(_ctg.get());
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "Scanner loaded: {} type={}", config.name, type);
        } else {
            WTSLogger::log_by_cat("strategy", LL_ERROR,
                "Scanner NOT FOUND: {} (scanner factory has no registered types — "
                "scanner subsystem is currently DISABLED, config ignored)", type);
        }
    }

    if (_scanCoord.empty()) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "Total scanners loaded: 0 (configured entries were skipped — see errors above)");
    } else {
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "Total scanners loaded: {}", _scanCoord.size());
    }

    // Wire scanner execution callback into CTG.  Scanners may provide full
    // simultaneous legs (Butterfly=3, VolSpread=2); otherwise fall back to the
    // legacy call/put sibling spread path.
    _ctg->setScannerExecuteFn([this](const wt_option::ScannerHitEvent& event) {
        if (!_grid) return;
        if (event.legs.size() >= 2) {
            std::vector<wt_option::SimulComboOrder::LegInput> legs;
            legs.reserve(event.legs.size());
            for (const wt_option::OptionData* legOd : event.legs) {
                if (!legOd || legOd->getBid() <= 0 || legOd->getAsk() <= 0) return;
                const double theo = legOd->values(0).theo();
                const double mid = legOd->getMid();
                if (theo <= 0 || mid <= 0) return;
                const bool buyLeg = theo > mid;   // underpriced -> buy
                legs.push_back({legOd->getCode(), buyLeg,
                                buyLeg ? legOd->getAsk() : legOd->getBid(), 1});
            }
            auto combo = std::make_shared<wt_option::SimulComboOrder>(
                event.reason, std::move(legs), &_comboCtx);
            if (combo->sendOrders() == wt_option::ComboOrder::SendResult::Success) {
                _activeCombos.push_back(combo);
                WTSLogger::log_by_cat("strategy", LL_INFO,
                    "Scanner simultaneous combo sent: {} legs={} edge={:.4f}",
                    event.reason, event.legs.size(), event.signal);
            }
            return;
        }

        const wt_option::OptionData* od = event.option;
        if (!od) return;
        auto sd = od->getStrikeData();
        if (!sd) return;
        const auto& sibling = (od->getRight() == wt_option::OR_Call) ? sd->put() : sd->call();
        if (!sibling) return;
        const double theo = od->values(0).theo();
        const double mid = od->getMid();
        if (theo <= 0 || mid <= 0) return;
        const bool buyMain = (theo > mid);
        auto combo = std::make_shared<wt_option::SpreadComboOrder>(
            "SpreadScanner", 1, od->getTickSize() > 0 ? od->getTickSize() : 1.0, &_comboCtx);
        combo->setupLegs(const_cast<wt_option::OptionData*>(od), buyMain,
                         const_cast<wt_option::OptionData*>(sibling.get()), !buyMain,
                         buyMain ? od->getAsk() : od->getBid(),
                         buyMain ? sibling->getBid() : sibling->getAsk());
        if (combo->sendOrders() == wt_option::ComboOrder::SendResult::Success)
            _activeCombos.push_back(combo);
    });
}

// ============================================================================
// on_tick — enqueue for async processing
// ============================================================================
void HftOptionStrategy::on_tick(IHftStraCtx* ctx, const char* code, WTSTickData* newTick)
{
    if (!_initialized) return;
    _tickCount.fetch_add(1, std::memory_order_relaxed);
    _async->enqueue_tick(code, newTick);

    // 路径1: 定时 doFit 触发（与 quantbox ClockMonitor 等价）
    // R5 fix: 原实现每 tick enqueue 一条 Timer——行情爆发时 worker 每 batch
    // 要执行 N 次 sections 扫描 + drainPendingQuotes。改为源头合并到 100ms
    // 节奏（跨日/时钟回拨立即放行），timer 流量下降两个数量级。
    uint32_t curDate = ctx->stra_get_date();
    double secOfDay = wt_option::ctxTimeSeconds(ctx);
    if (curDate != _lastTimerDate ||
        secOfDay < _lastTimerSecOfDay ||
        (secOfDay - _lastTimerSecOfDay) >= 0.1)
    {
        _async->enqueue_timer(curDate, ctx->stra_get_time());
        _lastTimerDate = curDate;
        _lastTimerSecOfDay = secOfDay;
    }
}

// ============================================================================
// on_trade — HFT signature already has isBuy (no conversion needed)
// ============================================================================
void HftOptionStrategy::on_trade(IHftStraCtx* ctx, uint32_t localid, const char* stdCode,
                                  bool isBuy, double vol, double price, const char* userTag)
{
    if (!_initialized) return;
    _async->enqueue_trade(localid, stdCode, isBuy, vol, price);
}

// ============================================================================
// on_order — HFT signature already has isBuy (no conversion needed)
// ============================================================================
void HftOptionStrategy::on_order(IHftStraCtx* ctx, uint32_t localid, const char* stdCode,
                                  bool isBuy, double totalQty, double leftQty,
                                  double price, bool isCanceled, const char* userTag)
{
    if (!_initialized) return;
    _async->enqueue_order(localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled);
}

// ============================================================================
// on_position
// ============================================================================
void HftOptionStrategy::on_position(IHftStraCtx* ctx, const char* stdCode, bool isLong,
                                     double prevol, double preavail, double newvol, double newavail)
{
    // A1+A2: Enqueue to async processor instead of directly accessing
    // _positions / _pnlPendingInit (owned by worker thread, avoid data race).
    // A1: full four-tuple preserved (prevol/preavail were dropped before).
    if (_async)
        _async->enqueue_position(stdCode, isLong, prevol, preavail, newvol, newavail);
}

// ============================================================================
// Channel events
// ============================================================================
// Channel events
// ============================================================================
void HftOptionStrategy::on_channel_ready(IHftStraCtx* ctx)
{
    // A3: _channelReady is atomic<bool> - safe to set from callback thread.
    _channelReady = true;
    // A1: Enqueue channel event to async processor for worker-thread processing.
    if (_async)
        _async->enqueue_channel(true);
}

void HftOptionStrategy::on_channel_lost(IHftStraCtx* ctx)
{
    _channelReady = false;
    if (_async)
        _async->enqueue_channel(false);
}

void HftOptionStrategy::on_entrust(uint32_t localid, bool bSuccess, const char* message, const char* userTag)
{
    // P1-1: route the failure to the worker thread.  OQM order state (and
    // hedge/combo latches) are worker-owned; WT guarantees NO terminal
    // on_order after a failed entrust, so the worker must clear the phantom
    // order itself.  Success needs no action (same as before).
    if (!bSuccess) {
        if (!_initialized) return;
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "HftOptionStrategy entrust rejected: id={} msg={}", localid, message);
        if (_async)
            _async->enqueue_entrust(localid, false);
    }
}

// ============================================================================
// Session events
// ============================================================================
void HftOptionStrategy::on_session_begin(IHftStraCtx* ctx, uint32_t uTDate)
{
    if (!_initialized || !_async) return;
    _async->enqueue_session(uTDate, true);
}

void HftOptionStrategy::on_session_end(IHftStraCtx* ctx, uint32_t uTDate)
{
    if (!_initialized) return;
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "HftOptionStrategy[{}] session_end: {} total_ticks={}", id(), uTDate,
        _tickCount.load(std::memory_order_relaxed));
    _async->enqueue_session(uTDate, false);
}

// ============================================================================
// Executor bridges — IHftStraCtx order placement
// ============================================================================

// R1: exchange price-limit guard for paths submitted through the strategy
// executor.  OQM/scanner still call IHftStraCtx directly today; their localids
// are mirrored via OrderIssuedFn, while full pre-send unification belongs to
// the OrderGateway refactor.
bool HftOptionStrategy::withinPriceLimits(const std::string& code, double price)
{
    if (price <= 0) return true;
    auto it = _priceLimits.find(code);
    if (it == _priceLimits.end()) return true;   // no limits seen yet — pass

    const auto& pl = it->second;
    auto rep = _riskLimitsEx.checkPriceLimit(price, pl.upper, pl.lower);
    if (rep.result == wt_option::RiskLimitsEx::CheckResult::REJECT) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "PriceLimit REJECT {}: {} ({})", code, price, rep.reason);
        return false;
    }
    return true;
}

void HftOptionStrategy::registerIssuedOrderIds(const std::string& code,
                                                const OrderIDs& ids,
                                                double price, uint32_t qty)
{
    // B14 fix: record issued prices so fill-deviation monitoring is live.
    if (_fillPriceChecker && price > 0) {
        for (uint32_t id : ids) {
            if (id != 0)
                _fillPriceChecker->onOrderSent(code, id, price);
        }
    }

    // B2: register EVERY framework localid.  A split action policy can return
    // multiple ids; tracking only ids[0] makes the remaining fills "Unknown".
    for (uint32_t id : ids) {
        if (id != 0)
            _anomalyGuard.onIssued(id, code, qty);
    }
}

std::pair<uint32_t, uint32_t> HftOptionStrategy::executeQuoteIds(
    const std::string& code, double bidP, uint32_t bidQ,
    double askP, uint32_t askQ)
{
    if (!_ctx || !_channelReady) return {0, 0};
    // R1: reject out-of-limit quotes before hitting the wire
    if (!withinPriceLimits(code, bidP) || !withinPriceLimits(code, askP)) return {0, 0};

    auto ids = _ctx->stra_quote(code.c_str(), bidP, bidQ, askP, askQ, "OptionMM");
    WTSLogger::log_by_cat("strategy", LL_DEBUG,
        "Quote: {} bid={}x{} ask={}x{} → ids={},{}",
        code, bidP, bidQ, askP, askQ, ids.first, ids.second);

    if (ids.first != 0)
        registerIssuedOrderIds(code, {ids.first}, bidP, bidQ);
    if (ids.second != 0)
        registerIssuedOrderIds(code, {ids.second}, askP, askQ);
    return ids;
}

int32_t HftOptionStrategy::executeQuote(const std::string& code, double bidP, uint32_t bidQ,
                                         double askP, uint32_t askQ)
{
    const auto ids = executeQuoteIds(code, bidP, bidQ, askP, askQ);
    return static_cast<int32_t>(ids.first + ids.second > 0 ? 1 : 0);
}

OrderIDs HftOptionStrategy::executeOrderIds(const std::string& code, bool isBuy,
                                             double price, uint32_t qty)
{
    if (!_ctx || !_channelReady) return {};
    // R1
    if (!withinPriceLimits(code, price)) return {};

    auto ids = isBuy
        ? _ctx->stra_buy(code.c_str(), price, qty, "OptionMM")
        : _ctx->stra_sell(code.c_str(), price, qty, "OptionMM");

    registerIssuedOrderIds(code, ids, price, qty);
    return ids;
}

int32_t HftOptionStrategy::executeOrder(const std::string& code, bool isBuy,
                                         double price, uint32_t qty)
{
    // Preserve the legacy success-count contract for CTG/manual callers.
    const OrderIDs ids = executeOrderIds(code, isBuy, price, qty);
    return static_cast<int32_t>(ids.size());
}

bool HftOptionStrategy::executeCancel(uint32_t localid)
{
    if (!_ctx || !_channelReady) return false;
    return _ctx->stra_cancel(localid);
}

int32_t HftOptionStrategy::executeCancel(const std::string& code)
{
    if (!_ctx || !_channelReady) return 0;

    auto ids = _ctx->stra_cancel_all(code.c_str());
    return static_cast<int32_t>(ids.size());
}

// ============================================================================
// checkRiskLimitsEx — A2: periodic Greeks / daily-loss limits (post-trade).
// Runs once per compute cycle; a REJECT latches _limitsBreached which feeds
// the CTG panic OR-chain (auto-resolves when the breach clears).
// ============================================================================
void HftOptionStrategy::checkRiskLimitsEx()
{
    if (!_risk) { _limitsBreached = false; return; }

    auto pg = _risk->getPositionGreeks();
    if (!pg) { _limitsBreached = false; return; }

    double pnlToday = 0;
    for (const auto& [pcode, pos] : _positions) {
        if (_otg) {
            auto utd = _otg->getUnderlyingTradingData(pcode);
            if (utd) { pnlToday += utd->getPnlTracker()->getCurPnl(); continue; }
            auto otd = _otg->getTradingData(pcode);
            if (otd) { pnlToday += otd->getPnlTracker()->getCurPnl(); }
        }
    }

    auto rep = _riskLimitsEx.checkGreeks(pg->delta(), pg->gamma(), pg->vega(), pnlToday);
    bool breach = (rep.result == wt_option::RiskLimitsEx::CheckResult::REJECT);
    if (breach && !_limitsBreached.load()) {
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "RiskLimitsEx GREEKS/DAILYLOSS BREACH: {} (value={:.2f} limit={:.2f})",
            rep.reason, rep.value, rep.limit);
    }
    _limitsBreached = breach;
}

// ============================================================================
// AsyncVolFitter: worker builds an immutable snapshot from the last IV pass;
// background thread fits fresh curves; worker publishes them at a later batch.
// ============================================================================
// ----------------------------------------------------------------------------
// P1-a: good-points threshold lookup, ported from
// PeriodicCurveFitter::evalThresh (linear interpolation over the
// {days -> min points} table; defaults to 15 when unconfigured).
// ----------------------------------------------------------------------------
static size_t volFitGoodPointsNeeded(int32_t days, const std::map<int32_t, double>& table)
{
    if (table.empty()) return 15;
    auto it = table.lower_bound(days);
    if (it == table.end())   return static_cast<size_t>(table.rbegin()->second);
    if (it == table.begin()) return static_cast<size_t>(it->second);
    auto lo = std::prev(it);
    const double x0 = lo->first, x1 = it->first;
    const double y0 = lo->second, y1 = it->second;
    if (x1 == x0) return static_cast<size_t>(y1);
    return static_cast<size_t>(y0 + (y1 - y0) * (days - x0) / (x1 - x0));
}

bool HftOptionStrategy::submitAsyncVolFit()
{
    if (!_grid || !_blackPricer) return false;

    // P1-a: fit window + period throttle (ports of PeriodicCurveFitter
    // doFit/onTimer semantics so fitter.period regains its meaning).
    // NOTE: nowSeconds carries a cumulative day offset across midnight
    // (night sessions) — the window is a time-of-day range, so compare
    // seconds-of-day, not the raw clock.  Without fmod the whole day
    // session (now > 86400) was silently excluded from fitting.
    const auto& fcfg = _blackPricer->config();
    const double now = _timeService.nowSeconds(_ctx);
    const double sod = std::fmod(now, 86400.0);
    if (sod < fcfg.vol_fitting_start_time || sod > fcfg.vol_fitting_end_time)
        return false;
    if (now - _lastVolFitSubmitTime < fcfg.vol_fitting_period)
        return false;

    const double thr = fcfg.vol_fitting_threshold;   // min option mid to qualify

    std::vector<wt_option::VolFitExpirySnapshot> snapshots;
    for (const auto& [exp, ed] : _grid->expiries()) {
        if (!ed || !ed->isForwardReady()) continue;
        const double fwd = ed->getForward();
        const double maturity = _blackPricer->getMaturity(exp);
        const double atmVol = _blackPricer->getATMVol(exp);
        if (!(fwd > 0) || !(maturity > 0) || !(atmVol > 0)) continue;

        wt_option::VolFitExpirySnapshot s;
        s.expiry = exp;
        s.forward = fwd;
        s.maturity = maturity;
        s.atmVol = atmVol;
        s.version = _blackPricer->getCurveVersion(exp) + 1;
        s.timestamp = now;

        // Quality gates ported from PeriodicCurveFitter::fitToExpiry:
        // threshold (min mid), and per-side coverage is validated below.
        bool hasUpside = false, hasDownside = false;
        for (const auto& sd : _grid->getStrikesByExpiry(exp)) {
            if (!sd) continue;
            const wt_option::OptionData* pricing = nullptr;
            if (sd->call() && sd->call()->getMid() > 0)
                pricing = sd->call().get();
            if (sd->put() && sd->put()->getMid() > 0 &&
                (!pricing || sd->put()->getMid() < pricing->getMid()))
                pricing = sd->put().get();
            if (!pricing) continue;
            if (pricing->getMid() <= thr) continue;      // threshold gate
            const double iv = pricing->values(0).impliedVol();
            if (iv > 0) {
                s.points.push_back({sd->getStrikePrice() - fwd, iv / atmVol});
                if (sd->getStrikePrice() > fwd) hasUpside = true;
                else hasDownside = true;
            }
        }
        // good-points gate (days-aware) + ups/downs quadrant gate: a
        // single-sided smile must not be fitted — it converges to a fake wing.
        const size_t need = std::max<size_t>(4,
            volFitGoodPointsNeeded(ed->daysToExpiry(), fcfg.vol_fitting_good_points_thresh));
        if (s.points.size() < need || !hasUpside || !hasDownside) {
            WTSLogger::log_by_cat("strategy", LL_DEBUG,
                "AsyncVolFitter exp={} skipped: pts={} need={} up={} down={}",
                exp, s.points.size(), need, hasUpside, hasDownside);
            continue;
        }
        snapshots.push_back(std::move(s));
    }
    if (snapshots.empty()) return false;
    if (!_asyncVolFitter.submit(std::move(snapshots))) return false;
    _lastVolFitSubmitTime = now;
    return true;
}

void HftOptionStrategy::publishAsyncVolFit()
{
    if (_blackPricer)
        _asyncVolFitter.tryPublish(*_blackPricer);
}

// ============================================================================
// Market freshness gate — stale feeds must not keep refreshing forward or keep
// old quotes working.  When the feed recovers, normal quoting resumes on the
// next batch.
// ============================================================================
bool HftOptionStrategy::isMarketFresh(uint64_t nowMs) const
{
    if (_marketStaleTimeoutMs == 0) return true;
    if (!_grid) return false;
    if (!_grid->isUnderlyingFresh(nowMs)) return false;
    if (!_grid->hasFreshOptionMarket(nowMs)) return false;

    if (_otg) {
        for (const auto& pair : _otg->getAllUnderlyingTradingData()) {
            const auto& utd = pair.second;
            if (utd && utd->isActive() &&
                !utd->isMarketFresh(nowMs, _marketStaleTimeoutMs)) {
                return false;
            }
        }
    }
    return true;
}

// ============================================================================
// checkMarginLimits — B1: estimated account margin vs configured ceiling.
// Simplified CN-exchange convention:
//   futures leg : qty × underlyingPx × volScale × futRate
//   short option: |qty| × (premium + optShortRate × underlyingPx) × volScale
// Rates are config-calibrated estimates — logs say "estimated"; the guard only
// warns / latches panic, never blocks individual orders.
// ============================================================================
void HftOptionStrategy::checkMarginLimits(double nowSec)
{
    if (!_marginCfg.enabled || _marginCfg.maxMargin <= 0) return;
    if ((nowSec - _lastMarginCheckTime) < _marginCfg.checkPeriodSec) return;
    _lastMarginCheckTime = nowSec;

    double udlPx = _grid ? _grid->getUnderlyingPrice() : 0;
    if (udlPx <= 0) return;

    double estMargin = 0;
    for (const auto& [mcode, pos] : _positions) {
        double apos = std::fabs(pos);
        if (apos < 1e-9) continue;
        bool isOpt = CodeHelper::isStdChnFutOptCode(mcode.c_str());
        if (!isOpt) {
            // Futures leg — need contract size from comm info
            auto ci = CodeHelper::extractStdCode(mcode.c_str(), nullptr);
            std::string prodKey = fmt::format("{}.{}", ci._exchg, ci._product);
            WTSCommodityInfo* cinfo = _ctx ? _ctx->stra_get_comminfo(prodKey.c_str()) : nullptr;
            double volScale = (cinfo && cinfo->getVolScale() > 0) ? cinfo->getVolScale() : 1.0;
            estMargin += apos * udlPx * volScale * _marginCfg.futRate;
        } else if (pos < 0) {
            // Short option: premium received at risk + rate on underlying notional
            auto od = _grid ? _grid->get(mcode) : nullptr;
            double premium = od ? od->getLast() : 0;
            if (premium <= 0) continue;
            double volScale = od ? od->getMultiplier() : 1.0;
            estMargin += apos * (premium + _marginCfg.optShortRate * udlPx) * volScale;
        }
    }

    if (estMargin > _marginCfg.maxMargin) {
        if (!_limitsBreached.load()) {
            WTSLogger::log_by_cat("strategy", LL_ERROR,
                "MARGIN GUARD BREACH (estimated): {:.0f} > max {:.0f} → panic latch",
                estMargin, _marginCfg.maxMargin);
        }
        _limitsBreached = true;
    } else if (estMargin > _marginCfg.maxMargin * _marginCfg.warnRatio) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "MARGIN GUARD WARNING (estimated): {:.0f} > {:.0f}% of max",
            estMargin, _marginCfg.warnRatio * 100);
    }
}

// ============================================================================
// R8: unified trading-state writers
// ============================================================================
void HftOptionStrategy::applyTradingState()
{
    if (!_traderCtx) return;
    const bool en = _tstate.enabledFlag();
    const bool pn = _tstate.panickedFlag();
    if (en != _traderCtx->enabled.load() || pn != _traderCtx->panicked.load()) {
        WTSLogger::log_by_cat("strategy", pn ? LL_WARN : LL_INFO,
            "TradingState: {} -> {} ({})  panicked={}",
            wt_option::toString(_tstate.type), _tstate.reason,
            en ? "ON" : "OFF", pn ? "Y" : "N");
    }
    _traderCtx->enabled = en;
    _traderCtx->panicked = pn;
}

bool HftOptionStrategy::_tradingStateEnabled() const
{
    return _tstate.enabledFlag();
}

void HftOptionStrategy::setState(wt_option::TradingStateType t, const std::string& reason)
{
    const wt_option::TradingStateType prev = _tstate.type;
    _tstate.type = t;
    _tstate.reason = reason;
    applyTradingState();

    // A STOP/Disabled transition is an order-management action, not merely a
    // flag change.  Refresh after enabled=false clears desired markets and
    // drains the resulting cancels unconditionally.
    if (t == wt_option::TradingStateType::Disabled
        && prev != wt_option::TradingStateType::Disabled
        && reason == "hotcmd_stop"
        && _ctg && _channelReady.load()) {
        _ctg->refresh();
        _ctg->drainPendingQuotes();
    }
}

// ============================================================================
// R9: hot-command bus registration
// ============================================================================
void HftOptionStrategy::registerCommands()
{
    wt_option::OptionCommandHandlers::Hooks hooks;
    hooks.setState = [this](wt_option::TradingStateType t, const std::string& reason) {
        setState(t, reason);
    };
    hooks.channelReady = [this]() { return _channelReady.load(); };
    hooks.setQModeAll = [this](const std::string& mode, std::string& err) -> bool {
        if (!_otg || !_ctg || !_grid) { err = "not initialized"; return false; }
        if (mode != "on" && mode != "off" && mode != "close") {
            err = "QMODE needs <on|off|close>";
            return false;
        }
        for (const auto& od : _grid->getAllOptions()) {
            if (od && od->getTradingData())
                _ctg->onSetQMode(od->getCode(), mode);
        }
        return true;
    };
    hooks.executeOrder = [this](const std::string& code, bool isBuy,
                                double px, uint32_t qty) {
        return executeOrder(code, isBuy, px, qty);
    };
    hooks.cancelAll = [this](const std::string& code) {
        executeCancel(code);
    };
    _cmdHandlers.registerAll(_cmdBus, std::move(hooks));
}

// ============================================================================
// R10: portfolio delta-hedge executor tick
// ============================================================================
double HftOptionStrategy::resolveDeltaPerLot()
{
    if (_otg && !_primaryHedgeCode.empty()) {
        auto utd = _otg->getUnderlyingTradingData(_primaryHedgeCode);
        if (utd && utd->getContractSize() > 0)
            return utd->getContractSize();
    }
    if (_ctx && !_primaryHedgeCode.empty()) {
        auto ci = CodeHelper::extractStdCode(_primaryHedgeCode.c_str(), nullptr);
        std::string prodKey = fmt::format("{}.{}", ci._exchg, ci._product);
        WTSCommodityInfo* cinfo = _ctx->stra_get_comminfo(prodKey.c_str());
        if (cinfo && cinfo->getVolScale() > 0)
            return cinfo->getVolScale();
    }
    if (!_deltaHedgeLotWarned) {
        _deltaHedgeLotWarned = true;
        WTSLogger::log_by_cat("strategy", LL_ERROR,
            "DeltaHedger: cannot resolve delta_per_lot for hedge code '{}'; hedge disabled until configured",
            _primaryHedgeCode);
    }
    return 0.0;
}

void HftOptionStrategy::checkDeltaHedge(double nowSec)
{
    if (!_deltaHedgeEnable || !_risk) return;

    if (_hedgeCfg.deltaPerLot <= 0) {
        _hedgeCfg.deltaPerLot = resolveDeltaPerLot();
        if (_hedgeCfg.deltaPerLot <= 0) return;
    }
    _hedgeCoord.setHedgeCode(_primaryHedgeCode);
    _hedgeCoord.setDeltaPerLot(_hedgeCfg.deltaPerLot);
    _hedgeCoord.check(nowSec, _risk->getPortfolioDelta());
}

// ============================================================================
// on_params_updated — P10: hot-update callback (实盘模式, 回测不触发)
// ============================================================================
void HftOptionStrategy::on_params_updated()
{
    // Framework callback thread: only signal; the worker owns grid/CTG/OQM.
    _hotUpdatePending.store(true, std::memory_order_release);
    // Wake the worker even in a completely quiet market.  Without a reliable
    // control event, STOP/PANIC could sit behind an empty event loop forever.
    if (_async)
        _async->enqueue_control();
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "HftOptionStrategy[{}] PARAMS HOT UPDATE queued", id());
}

void HftOptionStrategy::applyHotParams()
{
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "HftOptionStrategy[{}] === PARAMS HOT UPDATE ===", id());

    // 1. Read hot values into the Composite MM pricer config. GammaScalp is a
    // separate strategy pricer (dynamic hedging), not this alpha-weight path.
    if (_compositePricer) {
        const double vega = hotVal(_hot.wgt_vegaflow, _wgt_vegaflow);
        const double front = hotVal(_hot.wgt_frontfut_skew, _wgt_frontfut_skew);
        const double delta = hotVal(_hot.wgt_deltaflow, _wgt_deltaflow);
        const double atm = hotVal(_hot.wgt_atmsig, _wgt_atmsig);
        const double roll = hotVal(_hot.wgt_rollema, _wgt_rollema);
        _compositePricer->setAlphaWeights(vega, front, delta, atm, roll);
        _wgt_vegaflow = vega;
        _wgt_frontfut_skew = front;
        _wgt_deltaflow = delta;
        _wgt_atmsig = atm;
        _wgt_rollema = roll;
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "  alpha weights: vega={} frontfut={} delta={} atmsig={} rollema={}",
            vega, front, delta, atm, roll);
    }

    // 2. Update TPS
    int32_t newTps = hotVal(_hot.max_tps, _maxTPS);
    if (newTps != _maxTPS && _ctg) {
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "  maxTPS: {} → {}", _maxTPS, newTps);
        _maxTPS = newTps;
        _ctg->setMaxTransactionsPerSec(_maxTPS);
    }

    // 3. Runtime control via command param
    int32_t cmd = hotVal(_hot.command, 0);
    if (cmd != 0) {
        switch (cmd) {
            case 1: // Stop trading
                setState(wt_option::TradingStateType::Disabled, "hotcmd_stop");
                break;
            case 2: // Panic — stays enabled so risk-reducing cancels flow
                setState(wt_option::TradingStateType::Panic, "hotcmd_panic");
                break;
            case 3: // Resume
                setState(_channelReady.load() ? wt_option::TradingStateType::Normal
                                              : wt_option::TradingStateType::Disabled,
                         "hotcmd_resume");
                // B13: manual resume also clears the latched fill-price panic
                if (_fillPriceChecker) _fillPriceChecker->clearPanic();
                break;
        }
        if (_hot.command) *_hot.command = 0;
    }

    // 4. QuoteMode override
    int32_t qmode = hotVal(_hot.qmode_override, 0);
    if (qmode != 0 && _otg && _ctg) {
        std::string modeStr = (qmode == 1) ? "on" : (qmode == -1) ? "off" : "close";
        for (const auto& od : _grid->getAllOptions()) {
            if (od && od->getTradingData()) {
                _ctg->onSetQMode(od->getCode(), modeStr);
            }
        }
        if (_hot.qmode_override) *_hot.qmode_override = 0;
    }

    // 5. B8: Manual order via hot-param string
    if (_hot.manual_order && _hot.manual_order[0] != '\0') {
        std::string cmd(_hot.manual_order);
        // Parse: "B,code,price,qty" / "S,code,price,qty" / "C,code"
        std::vector<std::string> tokens;
        size_t start = 0, end;
        while ((end = cmd.find(',', start)) != std::string::npos) {
            tokens.push_back(cmd.substr(start, end - start));
            start = end + 1;
        }
        tokens.push_back(cmd.substr(start));

        // R9: structured dispatch via the command bus (ORDER/CANCEL/STOP/...)
        std::string err;
        if (!_cmdBus.dispatch(cmd, err)) {
            WTSLogger::log_by_cat("strategy", LL_WARN,
                "CommandBus rejected '{}': {}", cmd, err);
        }
        // Clear the command (reset shared memory string)
        // sync_param returns const char* pointing to shared memory; we can't
        // write through const, so we track via counter to avoid re-processing
        _hot.manual_order_counter++;
        // The ShareManager clears the string externally after reading
    }
}
