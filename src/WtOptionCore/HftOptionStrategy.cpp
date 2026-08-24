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
    _underlyingCode = cfg->getCString("underlyingCode");
    _futuresProduct = cfg->has("futuresProduct") ? cfg->getCString("futuresProduct") : "";
    _exchange = cfg->getCString("exchange");
    _underlyingType = cfg->has("underlyingType") ? cfg->getCString("underlyingType") : "future";
    _riskFreeRate = cfg->getDouble("riskFreeRate");
    _maxTPS = cfg->getInt32("maxTPS");
    _minComputeInterval = cfg->has("minComputeInterval") ? cfg->getDouble("minComputeInterval") : 0.02;

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
        }
        WTSVariant* futOm = omCfg->get("future");
        if (futOm) {
            futureOqmCfg.max_side_orders = futOm->has("max_side_orders") ? futOm->getUInt32("max_side_orders") : 1;
            futureOqmCfg.max_position = futOm->has("max_position") ? futOm->getUInt32("max_position") : 10;
            futureOqmCfg.time_in_force_ms = futOm->has("time_in_force_ms") ? futOm->getDouble("time_in_force_ms") : 45000;
            futureOqmCfg.enable_quote_api = futOm->has("enable_quote_api") ? futOm->getBoolean("enable_quote_api") : true;
            futureOqmCfg.check_potential_position = futOm->has("check_potential_position") ? futOm->getBoolean("check_potential_position") : true;
            futureOqmCfg.avoid_trade = futOm->has("avoid_trade") ? futOm->getBoolean("avoid_trade") : false;
            std::string qs = futOm->has("quote_style") ? futOm->getCString("quote_style") : "paired";
            futureOqmCfg.quote_style = (qs == "buy_sell")
                ? wt_option::OptionQuoteManager::Config::QS_BUYSELL
                : wt_option::OptionQuoteManager::Config::QS_PAIRED;
            futureOqmCfg.enable_offset_guard = futOm->has("enable_offset_guard")
                ? futOm->getBoolean("enable_offset_guard") : false;
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

    // Save config pointer for setupPricer
    cfgPtr_ = cfg;

    // Defaults if not in config
    if (_riskFreeRate == 0.0) _riskFreeRate = 0.03;
    if (_maxTPS == 0) _maxTPS = 50;

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
        if (_traderCtx) _traderCtx->panicked = true;
    });

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "Enhancement modules initialized: RiskLimitsEx FillPriceChecker");
    return true;
}

// ============================================================================
// on_init — create grid, pricer, CTG, wire async callbacks
// ============================================================================
void HftOptionStrategy::on_init(IHftStraCtx* ctx)
{
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
                for (const auto& rawCode : codes) {
                    std::string stdCode = CodeHelper::rawFutOptCodeToStdCode(rawCode.c_str(), _exchange.c_str());
                    _optionCodes.emplace_back(stdCode);
                    _positionGuards[stdCode] = std::make_shared<wt_option::PositionGuard>();
                    _positionOffsets[stdCode] = std::make_shared<wt_option::PositionOffsetMgr>();
                }
                WTSLogger::log_by_cat("strategy", LL_INFO,
                    "Auto-discovered {} option contracts for {}", codes.size(), commKey);
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
    _anomalyGuard.setGetTimeFn([this]() { return _ctx ? wt_option::ctxTimeSeconds(_ctx) : 0.0; });
    _anomalyGuard.setAlertCallback([](const std::string& msg) {
        WTSLogger::log_by_cat("strategy", LL_ERROR, "OrderAnomaly: {}", msg);
    });

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
                    wt_option::ctxTimeSeconds(_ctx) * 1000);
                state.confidence = frontEd->isValuesReady() ? 1.0 : 0.0;
            }
            _predictor->update(state);
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
        if (!hedgeCode.empty() && hedgeCode != _underlyingCode)
            _hedgeCodes.insert(hedgeCode);
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
        _futuresProduct, _underlyingCode, bdMgr, sessInfo);

    // Load holidays from holidays.json (same as longbeach ExchangeCalendar)
    // This is used by ExpiryData::countTradingDays when m_bdMgr is null
    std::string holidayFile = "common/holidays.json";
    std::set<uint32_t> holidays;
    std::ifstream ifs(holidayFile);
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
    p2cfg.enable_fitter = true;
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
    }

    auto pricer2 = std::make_shared<wt_option::OptionPricer2>(p2cfg, _grid.get(), _risk.get());

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
    if (_pricerType == "gammascalp") {
        // --- Gamma Scalping pricer ---
        _gammaPricer = std::make_shared<wt_option::GammaScalpOptionPricer>();
        _gammaPricer->setTheoreticalPricer(pricer2);
        _gammaPricer->setOptionRisk(_risk);

        // Wire order execution for dynamic hedging
        _gammaPricer->setOrderSender([this](const std::string& code, bool isBuy,
                                             double price, int32_t qty) {
            executeOrder(code, isBuy, price, static_cast<uint32_t>(qty));
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
    for (const auto& [exp, ec] : _expiryConfigs) {
        // Per-expiry underlying code registration
        std::string ulCode = ec.underlyingCode.empty() ? _underlyingCode : ec.underlyingCode;
        if (!ulCode.empty()) {
            _grid->setExpiryUnderlying(exp, ulCode);
        }
        // Apply forward calculation config to ExpiryData
        auto ed = _grid->getExpiryData(exp);
        if (ed) {
            ed->setIncludeFuture(ec.include_future);
            ed->setMinStrikesForSynthetic(ec.min_strikes_for_synthetic);
        }
        // Hedge override
        std::string hedgeCode = ec.hedgeCode.empty() ? ulCode : ec.hedgeCode;
        if (!hedgeCode.empty())
            _otg->setHedgeOverride(exp, hedgeCode);
        // B14: Register secondary hedge instruments with OptionRisk
        for (const auto& shc : ec.secondaryHedgeCodes) {
            // B18 fix: pass the real contract multiplier (was hardcoded 1.0,
            // breaking unit consistency vs option greeks scaled by volScale)
            double mult = 1.0;
            if (_ctx) {
                auto ci = CodeHelper::extractStdCode(shc.c_str(), nullptr);
                std::string prodKey = fmt::format("{}.{}", ci._exchg, ci._product);
                WTSCommodityInfo* cinfo = _ctx->stra_get_comminfo(prodKey.c_str());
                if (cinfo && cinfo->getVolScale() > 0)
                    mult = cinfo->getVolScale();
            }
            _risk->registerHedgeInstrument(shc, exp, mult);
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "Secondary hedge registered: {} for expiry {} (mult={})", shc, exp, mult);
        }
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
        return _ctx ? wt_option::ctxTimeSeconds(_ctx) : 0.0;
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
            guard->setGetTimeFn([this]() { return _ctx ? wt_option::ctxTimeSeconds(_ctx) : 0.0; });
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
                utd->setMarket(tick.bid, tick.ask);
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
                _grid->onTick(code, ref);
            }

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
                // Deferred enable: if channel is ready but trading was not enabled
                // (because underlying price was 0 at channel_ready time), enable now.
                if (_channelReady && _traderCtx && !_traderCtx->enabled) {
                    _traderCtx->enabled = true;
                    if (_otg) _otg->enableAll();
                    WTSLogger::log_by_cat("strategy", LL_INFO,
                        "Deferred enable: trading activated on first underlying tick");
                }
            }
        }
        // First tick with preClose -> initialize PnlTracker for overnight positions
        if (tick.preClose > 0 && _pnlPendingInit.count(code)) {
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
    };

    // Batch start: set pricer time (use stra_get_time for FAST/SLOW scheduling)
    cbs.on_tick_batch = [this]() {
        double timeSec = 0;
        if (_ctx) timeSec = wt_option::ctxTimeSeconds(_ctx);
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
            _tickTimestampUs = static_cast<uint64_t>(_ctx->stra_get_secs()) * 1000000ULL;
        }
        // A6: Notify scanners of underlying price update
        if (_grid && !_scanners.empty()) {
            double udlPrice = _grid->getUnderlyingPrice();
            if (udlPrice > 0) {
                for (auto& scanner : _scanners) {
                    if (scanner && scanner->isEnabled())
                        scanner->onUnderlyingUpdate(udlPrice);
                }
            }
        }
    };

    // Batch complete: computeValues + refresh + drainPendingQuotes (single pass per batch)
    cbs.on_batch_complete = [this]() {
        if (!_grid || !_pricer) return;

        static int batchCount = 0;
        batchCount++;
        if (batchCount <= 5 || batchCount % 100 == 0) {
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "HftOptionStrategy batch #{} enabled={} options={} strikes={} underlying={}",
                batchCount, _traderCtx ? _traderCtx->enabled.load() : false,
                _grid->numOptions(), _grid->numStrikes(), _grid->getUnderlyingPrice());
        }
        if (_traderCtx && _traderCtx->enabled) {
            for (const auto& [code, pos] : _positions) {
                auto od = _grid->get(code);
                if (od) od->setPosition(pos);
                // Sync OQM position from broker position (fixes fill-counter divergence)
                if (_otg) {
                    auto otd = _otg->getTradingData(code);
                    if (otd && otd->getQuoteManager())
                        otd->getQuoteManager()->setPosition(static_cast<int32_t>(pos));
                }
            }

            // P5: Fuse multiple getAllOptions/getAllUnderlyingTradingData passes
            // into a single pass each. Previously 4+ separate loops.
            double portfolioPnl = 0;

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

            // Single pass over options: PnlTracker update + portfolio PnL +
            // scanner dispatch + attribute collection
            const auto& allOpts = _grid->getAllOptions();
            for (const auto& od : allOpts) {
                if (!od) continue;
                // PnlTracker
                if (od->isActive() && od->getBid() > 0 && od->getAsk() > 0) {
                    auto otd = od->getTradingData();
                    if (otd) {
                        otd->getPnlTracker()->onPriceUpdate(od->getBid(), od->getAsk());
                        portfolioPnl += otd->getPnlTracker()->getCurPnl();
                    }
                }
                // Scanner dispatch
                if (!_scanners.empty() && od->isActive()) {
                    for (auto& scanner : _scanners) {
                        if (scanner && scanner->isEnabled())
                            scanner->onOptionUpdate(od.get());
                    }
                }
                // Attribute collection
                if (_attrPub) {
                    auto otd = od->getTradingData();
                    _attrPub->collectOption(od->getCode(), otd.get(), od.get());
                }
            }

            // Scanner grid-level dispatch
            if (!_scanners.empty()) {
                for (auto& scanner : _scanners) {
                    if (scanner && scanner->isEnabled())
                        scanner->onTick(_grid.get());
                }
            }

            // Attribute collection for underlyings
            if (_attrPub && _otg) {
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
            double now = wt_option::ctxTimeSeconds(_ctx);
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

                _grid->computeValues(_pricer.get());

                // Propagate pricer panic state to the CTG context.
                // B13 fix: OR-merge with FillPriceChecker's latched panic —
                // the unconditional overwrite used to clear it after 1 cycle.
                if (_traderCtx && _pricer) {
                    bool wasPanicked = _traderCtx->panicked;
                    bool pricerPanic = _pricer->isPanicked();
                    bool checkerPanic = _fillPriceChecker && _fillPriceChecker->isPanicked();
                    _traderCtx->panicked = pricerPanic || checkerPanic
                                           || _limitsBreached.load(std::memory_order_relaxed);
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

            // B10: Write option values to CSV if configured
            if (_valWriter && _grid) {
                if (_valWriter->shouldWrite(now)) {
                    _valWriter->writeValues(_grid.get(), now);
                }
            }

            // B11: Trigger predictor updates
            if (_triggerEngine) {
                double now = wt_option::ctxTimeSeconds(_ctx);
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
                        continue;
                    }
                    auto syn = std::dynamic_pointer_cast<wt_option::SynComboOrder>(combo);
                    if (syn && syn->isActive() && syn->checkTimeout()) {
                        syn->cancelAll();
                        continue;
                    }
                    // Keep if still active
                    bool active = (spread && spread->isActive()) || (syn && syn->isActive());
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

        _positions[code] += (isBuy ? 1 : -1) * vol;

        // Forward to OQM (per-contract order tracker)
        if (_otg && !skipOqm) {
            auto otd = _otg->getTradingData(code);
            if (otd && otd->getQuoteManager()) {
                // Set current time for QuoteStatistics
                if (_ctx) {
                    uint32_t curTime = _ctx->stra_get_time();
                    uint32_t secs = _ctx->stra_get_secs();
                    otd->getQuoteManager()->setCurrentTime(curTime, secs % 60);
                }
                otd->getQuoteManager()->onFill(localid, isBuy, price, fq);
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
            double nowSec = _ctx ? wt_option::ctxTimeSeconds(_ctx) : 0;
            for (auto& sig : _compositePricer->getRiskSignals()) {
                sig->setSignalTime(nowSec);
                sig->onFill(code, isBuy, vol, price);
            }
        }

        // B04 fix: route fills to active combo orders (was an empty loop —
        // leg2/hedge leg was never sent, leaving naked directional exposure)
        if (!_activeCombos.empty()) {
            uint32_t fq = static_cast<uint32_t>(vol);
            for (auto& combo : _activeCombos) {
                auto spread = std::dynamic_pointer_cast<wt_option::SpreadComboOrder>(combo);
                if (spread && spread->onLegFill(localid, fq)) continue;
                auto syn = std::dynamic_pointer_cast<wt_option::SynComboOrder>(combo);
                if (syn && syn->onLegFill(localid, fq)) continue;
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
                (_ctx ? wt_option::ctxTimeSeconds(_ctx) : 0.0) * 1000);
            _expSim->onFill(code, isBuy, price, static_cast<uint32_t>(vol), fee, tsMs);
        }

        // Mark for refresh: trade affects risk/position, need to recompute ourMarket
        _needsRefresh = true;
    };

    // Order callback
    cbs.on_order = [this](const std::string& code, uint32_t localid, bool isBuy,
                           double totalQty, double leftQty, double price, bool isCanceled) {
        if (isCanceled) {
            WTSLogger::log_by_cat("strategy", LL_DEBUG,
                "HftOptionStrategy order canceled: {} id={}", code, localid);
        }

        // B2: record done/cancel in the anomaly guard
        _anomalyGuard.onOrderDone(localid, isCanceled, leftQty);

        // Forward to OQM (order status tracking)
        if (_otg) {
            auto otd = _otg->getTradingData(code);
            if (otd && otd->getQuoteManager()) {
                otd->getQuoteManager()->setTickTimestampUs(_tickTimestampUs);
                // Set current time for QuoteStatistics (HHMM + sec_in_min)
                if (_ctx) {
                    uint32_t curTime = _ctx->stra_get_time();
                    uint32_t hhmm = curTime;
                    uint32_t secs = _ctx->stra_get_secs();
                    otd->getQuoteManager()->setCurrentTime(hhmm, secs % 60);
                }
                otd->getQuoteManager()->onOrderStatusChange(
                    localid, isBuy, totalQty, leftQty, price, isCanceled);
            }
        }

        // Enhancement: PositionOffsetMgr - track sent/cancelled orders
        // for frozen position tracking (close-today/prev availability)
        {
            auto oit = _positionOffsets.find(code);
            if (oit != _positionOffsets.end()) {
                if (isCanceled) {
                    // Order was cancelled - free the frozen position
                    oit->second->onOrderCancelled(isBuy,
                        static_cast<uint32_t>(totalQty - leftQty), false);
                }
                // Note: onOrderSent is called when the order is first placed,
                // not when acknowledged. This happens in the quote/order executor.
                // For now, we track cancellations here; sent tracking is done
                // in the executor path (executeQuote/executeOrder).
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

        // Path 1: periodic doFit (equivalent to quantbox ClockMonitor onClockWakeup)
        if (_compositePricer && _traderCtx && _traderCtx->enabled) {
            auto p2 = _compositePricer->getBlackPricer();
            if (p2 && p2->hasFitter()) {
                p2->triggerDoFit();
            }
        }

        // Pricer timeout retry (equivalent to onComputeValuesTimeout):
        // If the underlying hasn't changed but interval elapsed, force a
        // recompute to catch vol curve / time decay changes.
        if (_grid && _pricer && _traderCtx && _traderCtx->enabled) {
            double now = wt_option::ctxTimeSeconds(_ctx);
            if ((now - _lastComputeTime) >= _minComputeInterval * 5) {
                _lastComputeTime = now;
                _underlyingChanged = false;
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
            if (_traderCtx) _traderCtx->enabled = _channelReady.load();
            if (_grid) {
                _grid->setCurrentDate(tdate);
                // Refresh days-to-expiration for all expiries (fixes stale maturity)
                _grid->refreshExpiryDays();
                // B6: Re-evaluate front month (roll expired contracts)
                _grid->reevaluateFrontMonth();
            }
            // B07 fix: reset OQM lifetime counters — without this, MaxCancel /
            // MaxNewOrders / hard_flat thresholds permanently lock all quoting
            if (_otg) _otg->onSessionBegin();
            // B2: clear anomaly-guard registries for the new session
            _anomalyGuard.reset();
            // B20 fix: clear risk-signal latches (PnlLimit panic used to
            // survive across sessions until process restart)
            if (_compositePricer) {
                for (auto& sig : _compositePricer->getRiskSignals())
                    sig->reset();
            }
            // A6: Start scanners
            for (auto& scanner : _scanners) {
                if (scanner) scanner->onStart();
            }
            // B9: Clear expiration simulator for new session
            if (_expSim) _expSim->clear();
            // B10: Initialize value writer for new session
            if (_valWriter) _valWriter->init(tdate);
            // QuoteStatistics: reset for new session
            _quoteStats.onSessionBegin(tdate);
        } else {
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "HftOptionStrategy session end: {}", tdate);
            if (_traderCtx) _traderCtx->enabled = false;
            // A6: Stop scanners
            for (auto& scanner : _scanners) {
                if (scanner) scanner->onStop();
            }
            // B9: Print expiration simulator summary at session end
            if (_expSim) _expSim->printSummary();
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
        double pos = isLong ? newvol : -newvol;
        _positions[code] = pos;
        if (_risk)
            _risk->setHedgePosition(code, static_cast<int32_t>(pos));
        _pnlPendingInit.insert(code);

        // Enhancement: PositionGuard - update broker position for discrepancy check
        auto git = _positionGuards.find(code);
        if (git != _positionGuards.end())
            git->second->onBrokerPosition(isLong, newvol);

        // A1 fix: pass the REAL four-tuple to PositionOffsetMgr (was newvol×4)
        auto oit = _positionOffsets.find(code);
        if (oit != _positionOffsets.end())
            oit->second->onPositionUpdate(isLong, prevol, preavail, newvol, newavail);

        // Mark for refresh: position changes affect risk/quote sizing
        _needsRefresh = true;
    };

    // A3: Channel callback (processed in worker thread)
    cbs.on_channel = [this](bool isReady) {
        if (isReady) {
            WTSLogger::log_by_cat("strategy", LL_INFO, "channel_ready (worker)");
            // Enable trading only if underlying price is available,
            // otherwise computeValues will fail and produce spurious CANCELs.
            bool canEnable = _grid && _grid->getUnderlyingPrice() > 0;
            if (_traderCtx) _traderCtx->enabled = canEnable;
            if (canEnable && _otg) _otg->enableAll();
            if (!canEnable) {
                WTSLogger::log_by_cat("strategy", LL_WARN,
                    "channel_ready: underlying price not yet available, deferring enable");
            }
        } else {
            WTSLogger::log_by_cat("strategy", LL_WARN, "channel_lost (worker)");
            if (_traderCtx) _traderCtx->enabled = false;
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
    _comboCtx.sendOrder = [this](const std::string& code, bool isBuy, double price, uint32_t qty) -> uint32_t {
        if (!_ctx || !_channelReady.load()) return 0;
        auto ids = isBuy
            ? _ctx->stra_buy(code.c_str(), price, qty, "ScannerCombo")
            : _ctx->stra_sell(code.c_str(), price, qty, "ScannerCombo");
        return ids.empty() ? 0 : ids[0];
    };
    _comboCtx.cancelOrder = [this](uint32_t localid) -> bool {
        if (!_ctx) return false;
        return _ctx->stra_cancel(localid);
    };
    _comboCtx.getTime = [this]() -> double {
        return wt_option::ctxTimeSeconds(_ctx);
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
            _scanners.push_back(scanner);
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

    if (_scanners.empty()) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "Total scanners loaded: 0 (configured entries were skipped — see errors above)");
    } else {
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "Total scanners loaded: {}", _scanners.size());
    }

    // Wire scanner execution callback into CTG
    _ctg->setScannerExecuteFn([this](const std::string& code, double edge,
                                       const wt_option::OptionData* od) {
        if (!od || !_grid) return;

        // Find the strike data for this option to get the sibling
        auto sd = od->getStrikeData();
        if (!sd) return;

        // Get sibling option (call<->put)
        const auto& sibling = (od->getRight() == wt_option::OR_Call) ? sd->put() : sd->call();
        if (!sibling) return;

        // Determine direction: if theo > mid, option is underpriced -> buy
        double theo = od->values(0).theo();
        double mid = od->getMid();
        if (theo <= 0 || mid <= 0) return;

        bool buyMain = (theo > mid);
        uint32_t orderSize = 1;  // Conservative default

        // Create spread combo order (2-leg: main + hedge with sibling)
        auto combo = std::make_shared<wt_option::SpreadComboOrder>(
            "SpreadScanner", orderSize,
            od->getTickSize() > 0 ? od->getTickSize() : 1.0,
            &_comboCtx);

        // Leg1 = main option (mispriced one), Leg2 = sibling (hedge)
        double leg1Price = buyMain ? od->getAsk() : od->getBid();
        double leg2Price = buyMain ? sibling->getBid() : sibling->getAsk();

        combo->setupLegs(const_cast<wt_option::OptionData*>(od), buyMain,
                         const_cast<wt_option::OptionData*>(sibling.get()), !buyMain,
                         leg1Price, leg2Price);

        auto result = combo->sendOrders();
        if (result == wt_option::ComboOrder::SendResult::Success) {
            _activeCombos.push_back(combo);
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "Scanner combo sent: {} edge={:.4f} buy={}",
                code, edge, buyMain ? "main" : "sibling");
        }
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

    // 路径1: 每次 tick enqueue timer 事件, worker 线程处理时触发定时 doFit
    // (与 quantbox ClockMonitor 定时唤醒等价)
    uint32_t curDate = ctx->stra_get_date();
    uint32_t curTime = ctx->stra_get_time();
    _async->enqueue_timer(curDate, curTime);
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
    if (!bSuccess) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "HftOptionStrategy entrust failed: id={} msg={}", localid, message);
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
int32_t HftOptionStrategy::executeQuote(const std::string& code, double bidP, uint32_t bidQ,
                                         double askP, uint32_t askQ)
{
    if (!_ctx || !_channelReady) return 0;

    auto ids = _ctx->stra_quote(code.c_str(), bidP, bidQ, askP, askQ, "OptionMM");
    WTSLogger::log_by_cat("strategy", LL_DEBUG,
        "Quote: {} bid={}x{} ask={}x{} → ids={},{}",
        code, bidP, bidQ, askP, askQ, ids.first, ids.second);
    // B14 fix: record issued prices so fill-deviation monitoring is live
    if (_fillPriceChecker) {
        if (ids.first != 0 && bidP > 0)
            _fillPriceChecker->onOrderSent(code, ids.first, bidP);
        if (ids.second != 0 && askP > 0)
            _fillPriceChecker->onOrderSent(code, ids.second, askP);
    }
    // B2: register issued orders with the anomaly guard
    if (ids.first != 0)  _anomalyGuard.onIssued(ids.first, code, bidQ);
    if (ids.second != 0) _anomalyGuard.onIssued(ids.second, code, askQ);
    return static_cast<int32_t>(ids.first + ids.second > 0 ? 1 : 0);
}

int32_t HftOptionStrategy::executeOrder(const std::string& code, bool isBuy,
                                         double price, uint32_t qty)
{
    if (!_ctx || !_channelReady) return 0;

    auto ids = isBuy
        ? _ctx->stra_buy(code.c_str(), price, qty, "OptionMM")
        : _ctx->stra_sell(code.c_str(), price, qty, "OptionMM");

    // B14 fix
    if (_fillPriceChecker && !ids.empty() && price > 0)
        _fillPriceChecker->onOrderSent(code, ids[0], price);
    // B2
    if (!ids.empty())
        _anomalyGuard.onIssued(ids[0], code, qty);

    return static_cast<int32_t>(!ids.empty() ? 1 : 0);
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
// on_params_updated — P10: hot-update callback (实盘模式, 回测不触发)
// ============================================================================
void HftOptionStrategy::on_params_updated()
{
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "HftOptionStrategy[{}] === PARAMS HOT UPDATE ===", id());

    // 1. Read hot values into pricer config
    if (_pricer) {
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "  wgt_vegaflow: {} → {}", _wgt_vegaflow,
            hotVal(_hot.wgt_vegaflow, _wgt_vegaflow));
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
                if (_traderCtx) _traderCtx->enabled = false;
                WTSLogger::log_by_cat("strategy", LL_WARN, "  CMD: trading STOPPED");
                break;
            case 2: // Panic
                if (_traderCtx) {
                    _traderCtx->panicked = true;
                    // Keep enabled=true so refresh() can run and stage
                    // risk-reduction cancels with enhanced TPS. Options
                    // are force-cancelled; futures are kept for hedging.
                }
                WTSLogger::log_by_cat("strategy", LL_ERROR, "  CMD: PANIC");
                break;
            case 3: // Resume
                if (_traderCtx) {
                    _traderCtx->panicked = false;
                    _traderCtx->enabled = _channelReady.load();
                }
                // B13: manual resume also clears the latched fill-price panic
                if (_fillPriceChecker) _fillPriceChecker->clearPanic();
                WTSLogger::log_by_cat("strategy", LL_INFO, "  CMD: RESUME");
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

        if (tokens.size() >= 2) {
            std::string action = tokens[0];
            std::string code = tokens[1];
            if (action == "B" && tokens.size() >= 4) {
                double price = std::stod(tokens[2]);
                uint32_t qty = static_cast<uint32_t>(std::stoul(tokens[3]));
                if (_ctx) _ctx->stra_buy(code.c_str(), price, qty, "ManualOrder");
                WTSLogger::log_by_cat("strategy", LL_INFO,
                    "ManualOrder BUY {} {}@{}", code, qty, price);
            } else if (action == "S" && tokens.size() >= 4) {
                double price = std::stod(tokens[2]);
                uint32_t qty = static_cast<uint32_t>(std::stoul(tokens[3]));
                if (_ctx) _ctx->stra_sell(code.c_str(), price, qty, "ManualOrder");
                WTSLogger::log_by_cat("strategy", LL_INFO,
                    "ManualOrder SELL {} {}@{}", code, qty, price);
            } else if (action == "C") {
                if (_ctx) _ctx->stra_cancel_all(code.c_str());
                WTSLogger::log_by_cat("strategy", LL_INFO,
                    "ManualOrder CANCEL ALL {}", code);
            }
        }
        // Clear the command (reset shared memory string)
        // sync_param returns const char* pointing to shared memory; we can't
        // write through const, so we track via counter to avoid re-processing
        _hot.manual_order_counter++;
        // The ShareManager clears the string externally after reading
    }
}
