/*!
 * \file UftOptionStrategy.cpp
 * \brief Option MM Strategy — UFT full integration (Stage 5)
 *
 * Full chain: on_tick → async → grid.onTick(discovery) → computeValues → CTG.refresh → IUftStraCtx
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
// UftStraContext.h only included in live mode (not backtest)

#include "UftOptionStrategy.h"
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
#include "IScanModule.h"

namespace wt_option {
// Forward-declared in header, full definition here for OptionTraderContext
// Already defined in ControllableTradingGrid.h
}

UftOptionStrategy::UftOptionStrategy(const char* name)
    : UftStrategy(name)
{
}

UftOptionStrategy::~UftOptionStrategy()
{
    if (_async) _async->stop();
}

// ============================================================================
// init — read config
// ============================================================================
bool UftOptionStrategy::init(WTSVariant* cfg)
{
    _underlyingCode = cfg->getCString("underlyingCode");
    _optionProduct = cfg->getCString("optionProduct");
    _exchange = cfg->getCString("exchange");
    _riskFreeRate = cfg->getDouble("riskFreeRate");
    _maxTPS = cfg->getInt32("maxTPS");

    // Phase 6: alpha weights from config
    _wgt_vegaflow      = cfg->has("wgt_vegaflow") ? cfg->getDouble("wgt_vegaflow") : 0.0;
    _wgt_frontfut_skew = cfg->has("wgt_frontfut_skew") ? cfg->getDouble("wgt_frontfut_skew") : 0.0;
    _wgt_deltaflow     = cfg->has("wgt_deltaflow") ? cfg->getDouble("wgt_deltaflow") : 0.0;
    _wgt_atmsig        = cfg->has("wgt_atmsig") ? cfg->getDouble("wgt_atmsig") : 0.0;
    _wgt_rollema       = cfg->has("wgt_rollema") ? cfg->getDouble("wgt_rollema") : 0.0;
    _sticky_base       = cfg->has("sticky_base") ? cfg->getDouble("sticky_base") : 0.5;
    _improve_retreat   = cfg->has("improve_retreat_ratio") ? cfg->getDouble("improve_retreat_ratio") : 3.0;

    // Defaults if not in config
    if (_riskFreeRate == 0.0) _riskFreeRate = 0.03;
    if (_maxTPS == 0) _maxTPS = 50;

    // Read option contract list for subscription
    WTSVariant* optCfg = cfg->get("optionContracts");
    if (optCfg && optCfg->isArray()) {
        for (uint32_t i = 0; i < optCfg->size(); i++) {
            const char* code = optCfg->get(i)->asCString();
            if (strlen(code) > 0) _optionCodes.emplace_back(code);
        }
    }

    if (_underlyingCode.empty()) {
        // Try to derive from exchange + product
        _underlyingCode = fmt::format("{}.{}.main", _exchange, _optionProduct);
    }
    if (_optionProduct.empty()) {
        // Extract from underlying code
        auto pos1 = _underlyingCode.find('.');
        if (pos1 != std::string::npos) {
            auto pos2 = _underlyingCode.find('.', pos1 + 1);
            _optionProduct = (pos2 != std::string::npos)
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
        "UftOptionStrategy[{}] init: underlying={} product={} exchange={} rate={} tps={}",
        id(), _underlyingCode, _optionProduct, _exchange, _riskFreeRate, _maxTPS);
    return true;
}

// ============================================================================
// on_init — create grid, pricer, CTG, wire async callbacks
// ============================================================================
void UftOptionStrategy::on_init(IUftStraCtx* ctx)
{
    _ctx = ctx;
    WTSLogger::log_by_cat("strategy", LL_INFO, "UftOptionStrategy[{}] on_init", id());

    _async = std::make_shared<wt_option::OptionAsyncEventProcessor>();

    setupGrid();
    setupPricer();
    setupCTG();
    setupAsyncCallbacks();

    _async->start();
    _initialized = true;

    // Inject ctx into all OptionQuoteManagers created by OTG
    // (OTG creates OQM with nullptr ctx on first tick; we inject here)
    // This will be called on every on_init, but only matters the first time.
    // The actual injection happens in on_tick when OTD is created.

    // Subscribe to underlying ticks
    if (!_underlyingCode.empty()) {
        ctx->stra_sub_ticks(_underlyingCode.c_str());
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "UftOptionStrategy subscribed underlying: {}", _underlyingCode);
    }

    // Subscribe to ALL listed option contracts
    int optCount = 0;
    for (const auto& code : _optionCodes) {
        ctx->stra_sub_ticks(code.c_str());
        optCount++;
    }
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "UftOptionStrategy subscribed {} option contracts", optCount);

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "UftOptionStrategy[{}] initialized, grid={} pricer={} ctg={}",
        id(), (bool)_grid, (bool)_pricer, (bool)_ctg);
}

// ============================================================================
// setupGrid — create OptionGrid with IBaseDataMgr from UftStraContext
// ============================================================================
void UftOptionStrategy::setupGrid()
{
    // In backtest mode, _ctx is UftMocker (not UftStraContext)
    // IBaseDataMgr is only available in live mode via UftStraContext
    // For backtest, pass nullptr — grid will use holidays from holidays.json
    wtp::IBaseDataMgr* bdMgr = nullptr;
    wtp::WTSSessionInfo* sessInfo = nullptr;

    _grid = std::make_shared<wt_option::OptionGrid>(
        _optionProduct, _underlyingCode, bdMgr, sessInfo);

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

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "UftOptionStrategy grid created: product={} underlying={} holidays={}",
        _optionProduct, _underlyingCode, _grid->numHolidays());
}

// ============================================================================
// setupPricer — create CompositeOptionPricer + OptionPricer2
// ============================================================================
void UftOptionStrategy::setupPricer()
{
    if (!_grid) return;

    // Create OptionRisk (Phase 1: wire risk)
    _risk = std::make_shared<wt_option::OptionRisk>(_grid);
    _risk->registerHedgeInstrument(_underlyingCode, 0);
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
    _otg->setUftCtx(_ctx);            // OTG creates OQM with real ctx

    // Create OptionPricer2 (theoretical pricing)
    wt_option::OptionPricer2Config p2cfg;
    auto pricer2 = std::make_shared<wt_option::OptionPricer2>(p2cfg, _grid.get(), _risk.get());

    // Create CompositeOptionPricer (market making pricing)
    wt_option::CompositeOptionPricerConfig copCfg;
    // Phase 6: alpha weights from config (stored in init)
    copCfg.wgt_vegaflow      = _wgt_vegaflow;
    copCfg.wgt_frontfut_skew = _wgt_frontfut_skew;
    copCfg.wgt_deltaflow     = _wgt_deltaflow;
    copCfg.wgt_atmsig        = _wgt_atmsig;
    copCfg.wgt_rollema       = _wgt_rollema;
    copCfg.sticky_base       = _sticky_base;
    copCfg.improve_retreat_ratio = _improve_retreat;

    _pricer = std::make_shared<wt_option::CompositeOptionPricer>(copCfg, _grid.get(), _risk.get());
    _pricer->setBlackPricer(pricer2);

    // Configure expiry risk: enable quoting for all expiries
    _pricer->enableExpiry(202608, 0.0, 1.0);
    _pricer->enableExpiry(202610, 0.0, 1.0);
    _pricer->setMaxPosQty(202608, 5, 50, 50);
    _pricer->setMaxPosQty(202610, 5, 50, 50);

    _grid->setOptionPricer(_pricer);
    _grid->addListener(_pricer.get());

    WTSLogger::log_by_cat("strategy", LL_INFO,
        "UftOptionStrategy pricer+risk+OTG created");
}

// ============================================================================
// setupCTG — create ControllableTradingGrid + wire executors
// ============================================================================
void UftOptionStrategy::setupCTG()
{
    if (!_grid) return;

    _traderCtx = std::make_shared<wt_option::OptionTraderContext>();
    _traderCtx->enabled = false; // Enable on channel ready
    _traderCtx->panicked = false;
    // Phase 6: getTime from stra_get_time (fixes TPS limit)
    _traderCtx->getTimeFn = [this]() {
        return _ctx ? static_cast<double>(_ctx->stra_get_time()) : 0;
    };

    _ctg = std::make_shared<wt_option::ControllableTradingGrid>(_grid, _traderCtx);
    _ctg->setOTG(_otg.get());
    _ctg->setMaxTransactionsPerSec(_maxTPS);

    // Wire executors to IUftStraCtx
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

    WTSLogger::log_by_cat("strategy", LL_INFO, "UftOptionStrategy CTG created, tps={}", _maxTPS);
}

// ============================================================================
// setupAsyncCallbacks — wire async processor to grid/CTG
// ============================================================================
void UftOptionStrategy::setupAsyncCallbacks()
{
    wt_option::AsyncCallbacks cbs;

    // Per-code tick: update grid market data (worker thread, no WTSTickData* needed)
    cbs.on_tick = [this](const std::string& code, const wt_option::TickData& tick) {
        if (_grid) {
            wt_option::OptionGrid::TickDataRef ref;
            ref.price = tick.price;
            ref.bid = tick.bid;
            ref.ask = tick.ask;
            ref.bidQty = tick.bidQty;
            ref.askQty = tick.askQty;
            _grid->onTick(code, ref);
        }
    };

    // Batch start: set pricer time
    cbs.on_tick_batch = [this]() {
        if (_pricer) {
            double timeSec = TimeUtils::getLocalTimeNow() / 1000000.0;
            _pricer->setTime(timeSec);
        }
    };

    // Batch complete: computeValues + refresh + drainPendingQuotes (single pass per batch)
    cbs.on_batch_complete = [this]() {
        if (!_grid || !_pricer) return;
        static int batchCount = 0;
        batchCount++;
        if (batchCount <= 5 || batchCount % 100 == 0) {
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "UftOptionStrategy batch #{} enabled={} options={} strikes={} underlying={}",
                batchCount, _traderCtx ? _traderCtx->enabled : false,
                _grid->numOptions(), _grid->numStrikes(), _grid->getUnderlyingPrice());
        }
        if (_traderCtx && _traderCtx->enabled) {
            _grid->computeValues(_pricer.get());
            if (_ctg) _ctg->drainPendingQuotes();
        }
    };

    // Trade callback
    cbs.on_trade = [this](const std::string& code, uint32_t localid,
                           bool isBuy, double vol, double price) {
        WTSLogger::log_by_cat("strategy", LL_INFO,
            "UftOptionStrategy trade: {} {} {}@{}", code, isBuy?"BUY":"SELL", vol, price);
        _positions[code] += (isBuy ? 1 : -1) * vol;

        // Phase 5: Forward to OQM (per-contract order tracker)
        if (_otg) {
            auto otd = _otg->getTradingData(code);
            if (otd && otd->getQuoteManager()) {
                otd->getQuoteManager()->onFill(localid, isBuy, price, static_cast<uint32_t>(vol));
            }
        }

        // Phase 5: Forward to Pricer onFill (trade-shock back-away + riskShift)
        if (_pricer) {
            auto stub = std::make_shared<wt_option::OrderStub>();
            stub->code = code;
            stub->dir = isBuy ? 0 : 1;  // 0=buy, 1=sell
            stub->fillPrice = price;
            _pricer->triggerOnFill(stub, price, static_cast<uint32_t>(vol));
        }

        // Phase 5: Update OptionRisk position
        if (_risk) {
            auto rd = _risk->get(code);
            if (rd) {
                rd->addFill((isBuy ? 1 : -1) * static_cast<int32_t>(vol), price);
            }
        }
    };

    // Order callback
    cbs.on_order = [this](const std::string& code, uint32_t localid, bool isBuy,
                           double totalQty, double leftQty, double price, bool isCanceled) {
        if (isCanceled) {
            WTSLogger::log_by_cat("strategy", LL_DEBUG,
                "UftOptionStrategy order canceled: {} id={}", code, localid);
        }

        // Phase 5: Forward to OQM (order status tracking)
        if (_otg) {
            auto otd = _otg->getTradingData(code);
            if (otd && otd->getQuoteManager()) {
                otd->getQuoteManager()->onOrderStatusChange(
                    localid, isBuy, totalQty, leftQty, price, isCanceled);
            }
        }
    };

    // Timer callback — trigger SLOW compute
    cbs.on_timer = [this](uint32_t curDate, uint32_t curTime) {
        // SLOW compute handled by batch_complete + pricer's FAST/SLOW scheduling
    };

    // Session callback
    cbs.on_session = [this](uint32_t tdate, bool isBegin) {
        if (isBegin) {
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "UftOptionStrategy session begin: {}", tdate);
            if (_traderCtx) _traderCtx->enabled = _channelReady;
        } else {
            WTSLogger::log_by_cat("strategy", LL_INFO,
                "UftOptionStrategy session end: {}", tdate);
            if (_traderCtx) _traderCtx->enabled = false;
        }
    };

    _async->setCallbacks(cbs);
}

// ============================================================================
// on_tick — enqueue for async processing + sync grid update
// ============================================================================
void UftOptionStrategy::on_tick(IUftStraCtx* ctx, const char* stdCode, WTSTickData* newTick)
{
    if (!_initialized) return;
    _tickCount.fetch_add(1, std::memory_order_relaxed);
    if (_tickCount.load() == 1) {
        fprintf(stderr, "[OPT] first on_tick: code=%s price=%f\n", stdCode, newTick->price());
    }
    _async->enqueue_tick(stdCode, newTick);
}

// ============================================================================
// on_trade — update position + notify CTG
// ============================================================================
void UftOptionStrategy::on_trade(IUftStraCtx* ctx, uint32_t localid, const char* stdCode,
                                  bool isLong, uint32_t offset, double vol, double price)
{
    if (!_initialized) return;
    bool isBuy = isLong ? (offset == 0) : (offset != 0);
    _async->enqueue_trade(localid, stdCode, isBuy, vol, price);
}

// ============================================================================
// on_order
// ============================================================================
void UftOptionStrategy::on_order(IUftStraCtx* ctx, uint32_t localid, const char* stdCode,
                                  bool isLong, uint32_t offset, double totalQty, double leftQty,
                                  double price, bool isCanceled)
{
    if (!_initialized) return;
    bool isBuy = isLong ? (offset == 0) : (offset != 0);
    _async->enqueue_order(localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled);
}

// ============================================================================
// on_position
// ============================================================================
void UftOptionStrategy::on_position(IUftStraCtx* ctx, const char* stdCode, bool isLong,
                                     double prevol, double preavail, double newvol, double newavail)
{
    WTSLogger::log_by_cat("strategy", LL_DEBUG,
        "UftOptionStrategy[{}] position: {} {} vol={}", id(), stdCode, isLong?"L":"S", newvol);
    // Update strategy-local position tracking
    if (isLong) {
        _positions[stdCode] = newvol;
    } else {
        _positions[stdCode] = -newvol;
    }
}

// ============================================================================
// Channel events
// ============================================================================
void UftOptionStrategy::on_channel_ready(IUftStraCtx* ctx)
{
    WTSLogger::log_by_cat("strategy", LL_INFO, "UftOptionStrategy[{}] channel_ready", id());
    _channelReady = true;
    if (_traderCtx) _traderCtx->enabled = true;
    // Enable all OTD through OTG
    if (_otg) _otg->enableAll();
}

void UftOptionStrategy::on_channel_lost(IUftStraCtx* ctx)
{
    WTSLogger::log_by_cat("strategy", LL_WARN, "UftOptionStrategy[{}] channel_lost", id());
    _channelReady = false;
    if (_traderCtx) _traderCtx->enabled = false;
    // Disable all OTD through OTG
    if (_otg) _otg->disableAll();
}

void UftOptionStrategy::on_entrust(uint32_t localid, bool bSuccess, const char* message)
{
    if (!bSuccess) {
        WTSLogger::log_by_cat("strategy", LL_WARN,
            "UftOptionStrategy entrust failed: id={} msg={}", localid, message);
    }
}

// ============================================================================
// Session events
// ============================================================================
void UftOptionStrategy::on_session_begin(IUftStraCtx* ctx, uint32_t uTDate)
{
    if (!_initialized || !_async) return;
    _async->enqueue_session(uTDate, true);
}

void UftOptionStrategy::on_session_end(IUftStraCtx* ctx, uint32_t uTDate)
{
    if (!_initialized) return;
    WTSLogger::log_by_cat("strategy", LL_INFO,
        "UftOptionStrategy[{}] session_end: {} total_ticks={}", id(), uTDate,
        _tickCount.load(std::memory_order_relaxed));
    _async->enqueue_session(uTDate, false);
}

// ============================================================================
// Executor bridges — IUftStraCtx order placement
// ============================================================================
int32_t UftOptionStrategy::executeQuote(const std::string& code, double bidP, uint32_t bidQ,
                                         double askP, uint32_t askQ)
{
    if (!_ctx || !_channelReady) return 0;

    auto ids = _ctx->stra_quote(code.c_str(), bidP, bidQ, askP, askQ, "OptionMM");
    WTSLogger::log_by_cat("strategy", LL_DEBUG,
        "Quote: {} bid={}x{} ask={}x{} → ids={},{},",
        code, bidP, bidQ, askP, askQ, ids.first, ids.second);
    return static_cast<int32_t>(ids.first + ids.second > 0 ? 1 : 0);
}

int32_t UftOptionStrategy::executeOrder(const std::string& code, bool isBuy,
                                         double price, uint32_t qty)
{
    if (!_ctx || !_channelReady) return 0;

    auto ids = isBuy
        ? _ctx->stra_buy(code.c_str(), price, qty)
        : _ctx->stra_sell(code.c_str(), price, qty);

    return static_cast<int32_t>(!ids.empty() ? 1 : 0);
}

int32_t UftOptionStrategy::executeCancel(const std::string& code)
{
    if (!_ctx || !_channelReady) return 0;

    auto ids = _ctx->stra_cancel_all(code.c_str());
    return static_cast<int32_t>(ids.size());
}
