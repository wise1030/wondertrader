/*!
 * \file WtOptionStrategy.cpp
 * \brief Complete option trading strategy implementation
 */

#include "WtOptionStrategy.h"
#include "WtOptContext.h"
#include "StandardOptionPricer.h"
#include "CompositeOptionPricer.h"
#include "GammaScalpOptionPricer.h"
#include <iostream>
#include <algorithm>
#include <cmath>

#include "../WTSTools/WTSLogger.h"
#include <fstream>
#include <sstream>
#include <sstream>

#include "Scanners/ButterflyScanner.h"
#include "Scanners/GarchScanner.h" 
#include "Scanners/LowBidsScanner.h"
#include "Scanners/MMScanner.h"
#include "Scanners/OpenScanner.h"
#include "Scanners/SimplexScanner.h"
#include "Scanners/SpreadScanner.h"
#include "Scanners/StrikeSpreadScanner.h"
#include "Scanners/SyntheticFutureScanner.h"
#include "Scanners/VolSpreadScanner.h"

namespace wt_option {

using wtp::IOptStraCtx;
using wtp::WtOptContext;

//=============================================================================
// OptionTraderContext implementation
//=============================================================================

OptionTraderContext::OptionTraderContext()
    : m_state(StrategyState::Idle)
    , m_traceLevel(0)
    , m_currentTime(0)
    , m_currentDate(0)
    , m_prevDate(0)
    , m_isNewDay(false)
{
}

void OptionTraderContext::setState(StrategyState state) {
    if (m_state != state) {
        m_state = state;
    }
}

void OptionTraderContext::setCurrentDate(uint32_t date) {
    if (m_currentDate != date) {
        m_prevDate = m_currentDate;
        m_currentDate = date;
        m_isNewDay = (m_prevDate > 0 && date > m_prevDate);
    }
}

//=============================================================================
// WtOptionStrategy implementation
//=============================================================================

WtOptionStrategy::WtOptionStrategy(const std::string& name, const OptionStrategyConfig& config)
    : OptStrategy(name.c_str())
    , m_config(config)
    , m_initialized(false)
    , m_lastComputeTime(0)
    , m_lastRiskCheckTime(0)
    , m_lastPanicPrice(0)
    , m_grid(nullptr)
    , m_risk(nullptr)
    , m_orderManager(nullptr)
    , m_curveFitter(nullptr)
{
    // Strategy logic state
    m_tradeContext = std::make_shared<OptionTraderContext>();
    m_tradeContext->setTraceLevel(config.traceLevel);
}

WtOptionStrategy::WtOptionStrategy(const OptionStrategyConfig& config)
    : OptStrategy(config.underlyingCode.c_str())
    , m_config(config)
    , m_initialized(false)
    , m_lastComputeTime(0)
    , m_lastRiskCheckTime(0)
    , m_lastPanicPrice(0)
    , m_grid(nullptr)
    , m_risk(nullptr)
    , m_orderManager(nullptr)
    , m_curveFitter(nullptr)
{
    m_tradeContext = std::make_shared<OptionTraderContext>();
    m_tradeContext->setTraceLevel(config.traceLevel);
}

WtOptionStrategy::~WtOptionStrategy() {
    shutdown();
}

void WtOptionStrategy::on_init(IOptStraCtx* ctx)
{
    if (m_initialized) return;
    
    _ctx = ctx;
    
    // Initialize components from Context
    auto wtCtx = dynamic_cast<WtOptContext*>(ctx);
    if (wtCtx) {
        m_grid = wtCtx->stra_get_grid();
        m_risk = wtCtx->stra_get_risk();
        m_orderManager = wtCtx->stra_get_order_manager();
        
        // Bind Executors from Strategy into OrderManager
        if (m_orderManager) {
            m_orderManager->setOrderExecutor([this](const BaseOrder& order) {
                 return this->sendOrder(order.getCode(), order.getDirection() == OrderDir::Buy, order.getPrice(), order.getQuantity()) > 0;
            });
            m_orderManager->setCancelExecutor([this](uint32_t orderId) {
                 return this->cancelOrder(orderId);
            });
            m_orderManager->setQuoteExecutor([this](const std::string& code, double bidPrice, uint32_t bidQty, double askPrice, uint32_t askQty) {
                 return this->sendQuote(code, bidPrice, bidQty, askPrice, askQty);
            });
        }
        
        // Initialize CurveFitter (Owned by Strategy)
        CurveFitterConfig fitCfg;
        fitCfg.fitPeriod = m_config.curveFitPeriod;
        
        // CurveFitter requires shared_ptr<OptionGrid>
        m_curveFitter = std::make_shared<CurveFitter>(wtCtx->get_grid(), fitCfg);
    }
    
    if (!m_grid || !m_risk || !m_orderManager) {
        return;
    }
    
    // Create Pricer Hierarchy: Composite -> Standard
    // Strategy logic resides in CompositePricer
    auto theoPricer = std::make_shared<StandardOptionPricer>();
    
    if (m_config.pricerType == "gammascalp") {
        auto gammaPricer = std::make_shared<GammaScalpOptionPricer>();
        gammaPricer->setTheoreticalPricer(theoPricer);
        gammaPricer->setOptionRisk(wtCtx->get_risk());
        gammaPricer->setStrategy(this);
        
        // Push config (we map RiskLimits to GammaScalpConfig as simplified approach)
        GammaScalpConfig gConfig;
        gConfig.enable = true;
        gConfig.targetGamma = m_config.riskLimits.maxGamma; 
        gConfig.maxOrderSize = static_cast<int32_t>(m_config.riskLimits.maxPositionPerOption);
        gammaPricer->setExpiryConfig(0, gConfig); // Default config
        
        m_pricer = gammaPricer;
    } else {
        auto compPricer = std::make_shared<CompositeOptionPricer>();
        compPricer->setTheoreticalPricer(theoPricer);
        // CompositePricer needs Risk component for market making logic
        compPricer->setOptionRisk(wtCtx->get_risk()); 
        
        // Strategy owns the pricer
        m_pricer = compPricer;

        // Inject strategy into pricer if it's a CompositeOptionPricer
        auto compositePricer = std::dynamic_pointer_cast<CompositeOptionPricer>(m_pricer);
        if(compositePricer) {
            compositePricer->setStrategy(this);
        }
    }
    
    // We also set it on the grid so grid methods work, but Context doesn't own it
    m_grid->setOptionPricer(m_pricer);
    
    initialize();
    m_tradeContext->setState(StrategyState::Running);
    
    // Start all scanners
    for (auto& scanner : m_scanners) {
        scanner->onStart();
    }
    
    onStrategyStart();
}

bool WtOptionStrategy::initialize() {
    // Component initialization is now done by Context
    // We only need to setup Strategy specific things like Scanners
    
    // Create scanners from config
    for (const auto& scannerCfg : m_config.scannerConfigs) {
        auto scanner = ScannerFactory::instance().createScanner(
            scannerCfg->name, *scannerCfg);
        if (scanner) {
            addScanner(scanner);
        }
    }
    
    onStrategyInit();
    
    m_initialized = true;
    return true;
}

void WtOptionStrategy::shutdown() {
    if (!m_initialized) return;
    
    m_tradeContext->setState(StrategyState::Shutdown);
    
    // Cancel all orders
    cancelAllOrders();
    
    // Stop all scanners
    for (auto& scanner : m_scanners) {
        scanner->onStop();
    }
    m_scanners.clear();
    
    onStrategyStop();
    
    m_initialized = false;
}

void WtOptionStrategy::on_session_begin(IOptStraCtx* ctx, uint32_t uTDate) {
    if (!m_initialized) return;
    m_tradeContext->setCurrentDate(uTDate);
    m_tradeContext->setState(StrategyState::Running);
    onBeginOfDay(uTDate);
}

void WtOptionStrategy::on_session_end(IOptStraCtx* ctx, uint32_t uTDate) {
    if (!m_initialized) return;
    onEndOfDay(uTDate);
    m_tradeContext->setState(StrategyState::Idle);
}

void WtOptionStrategy::on_calculate(IOptStraCtx* ctx, uint32_t curDate, uint32_t curTime) {
    if (!m_initialized) return;
    onTimer(curTime);
}

void WtOptionStrategy::on_tick(IOptStraCtx* ctx, const char* stdCode, wtp::WTSTickData* newTick) {
    if (!m_initialized) return;

    // 1. Update Market Data
    updateMarketData(stdCode, newTick->price(), newTick->bidprice(0), newTick->askprice(0), newTick->actiontime());

    // 2. Run Strategy Logic
    runStrategyLogic(stdCode, newTick->price(), newTick->actiontime());
}

void WtOptionStrategy::updateMarketData(const char* code, double price, double bid, double ask, uint64_t timestamp) {
    if (!m_initialized || !m_grid) return;
    
    // Update the option in the grid
    auto option = m_grid->getOption(code);
    if (option) {
        OptionMarket mkt;
        mkt.bid = bid;
        mkt.ask = ask;
        mkt.last = price;
        mkt.updateTime = timestamp;
        option->updateMarket(mkt);
    } else {
        // May be underlying
        m_grid->setUnderlyingPrice(price);
    }
}

void WtOptionStrategy::onFill(const char* code, uint32_t orderId, double price, uint32_t qty, bool isBuy) {
    if (!m_initialized) return;
    
    // Track Last Fill for Anti-Ping (Trade Shock) Protection
    std::string strCode(code);
    if (isBuy) {
        m_lastBuyFillPrice[strCode] = price;
    } else {
        m_lastSellFillPrice[strCode] = price;
    }
    m_lastFillTime[strCode] = m_tradeContext->getCurrentTime();
    
    // Route fill to OrderManager
    if (m_orderManager) {
        m_orderManager->onFill(code, orderId, price, qty, m_tradeContext->getCurrentTime());
    }
    
    // Update PnL calculation is handled via Position updates triggered by OrderManager
}

void WtOptionStrategy::onOrderResponse(uint32_t orderId, const char* code, bool success, const char* message) {
    if (!m_initialized || !m_orderManager) return;
    if (!success) {
        m_orderManager->onReject(code ? code : "", orderId, message ? message : "");
    }
}

void WtOptionStrategy::addScanner(IScanModulePtr scanner) {
    if (scanner) {
        m_scanners.push_back(scanner);
    }
}

void WtOptionStrategy::on_tick_batch(IOptStraCtx* ctx) {
    if (!m_initialized || !m_pricer || !m_grid) return;
    
    // Drive pricing logic via Strategy
    m_pricer->computeValues(m_grid);
}

void WtOptionStrategy::runStrategyLogic(const char* code, double price, uint64_t time) {
    if (!m_initialized) return;
    
    m_tradeContext->setCurrentTime(time);
    
    // Check for new day
    if (m_tradeContext->isNewDay()) {
        onBeginOfDay(m_tradeContext->getCurrentDate());
        m_tradeContext->clearNewDay();
    }
    
    // Check if it's underlying update
    std::string strCode = code;
    bool isUnderlying = (strCode == m_config.underlyingCode || strCode.find(m_config.underlyingCode) == 0);
    
    if (isUnderlying) {
        // Notify scanners
        for (auto& scanner : m_scanners) {
            if (scanner->isEnabled()) {
                scanner->onUnderlyingUpdate(price);
            }
        }
        
        // Check panic
        if (m_tradeContext->getState() == StrategyState::Running) {
            checkPanic();
        }
    } else {
        // Check if it's an option update
        auto option = m_grid->getOption(code);
        if (option) {
            // Notify scanners
            for (auto& scanner : m_scanners) {
                if (scanner->isEnabled()) {
                    scanner->onOptionUpdate(option.get());
                }
            }
        }
    }
    
    // Periodic Risk checks
    // Note: Pricing is handled by WtOptContext worker loop
    if (time - m_lastRiskCheckTime > 5000000) {  // 5 seconds
        // Risk update is also handled by Context, but we check limits here to potentially pause strategy
        checkRiskLimits();
        updatePnL(); // PnL might need explicit update if not fully automatic
        m_lastRiskCheckTime = time;
    }
}

void WtOptionStrategy::onTick(const char* code, double price, double bid, double ask, uint64_t timestamp) {
    // Wrapper for standalone usage
    if (!m_initialized) return;
    
    updateMarketData(code, price, bid, ask, timestamp);
    runStrategyLogic(code, price, timestamp);
}


void WtOptionStrategy::onTimer(uint64_t time) {
    if (!m_initialized) return;
    
    m_tradeContext->setCurrentTime(time);
    
    // Process scanners
    for (auto& scanner : m_scanners) {
        if (scanner->isEnabled() && m_tradeContext->canTrade()) {
            scanner->onTick(m_grid);
        }
    }
    
    // Curve fitting
    if (m_curveFitter && m_tradeContext->getState() == StrategyState::Running) {
        m_curveFitter->onTimer(time);
    }
    
    // Process orders
    processOrders();
}

void WtOptionStrategy::onBeginOfDay(uint32_t date) {
    m_tradeContext->setCurrentDate(date);
    m_pnl.onNewDay();
    
    onDayStart(date);
}

void WtOptionStrategy::onEndOfDay(uint32_t date) {
    // Cancel all orders
    cancelAllOrders();
    
    // Update final P&L
    updatePnL();
    
    onDayEnd(date);
}

//=============================================================================
// Order Interface
//=============================================================================

uint32_t WtOptionStrategy::sendOrder(const std::string& code, bool isBuy,
                                      double price, uint32_t qty)
{
    if (!isEnabled() || !_ctx) return 0;
    
    // Submit
    if (isBuy) {
        return _ctx->stra_buy(code.c_str(), price, qty, "WtOpt");
    } else {
        return _ctx->stra_sell(code.c_str(), price, qty, "WtOpt");
    }
}

uint32_t WtOptionStrategy::sendQuote(const std::string& code, double bidPrice, uint32_t bidQty, double askPrice, uint32_t askQty)
{
    if (!isEnabled() || !_ctx) return 0;
    
    return _ctx->stra_quote(code.c_str(), bidPrice, bidQty, askPrice, askQty, "WtOpt");
}    

bool WtOptionStrategy::cancelOrder(uint32_t orderId) {
    if (_ctx) {
        return _ctx->stra_cancel(orderId);
    }
    return false;
}

void WtOptionStrategy::cancelAllOrders() {
    if (_ctx && m_grid) {
        for (const auto& opt : m_grid->getAllOptions()) {
            if (opt) {
                _ctx->stra_cancel_all(opt->getCode().c_str());
            }
        }
        // Also cancel underlying
        _ctx->stra_cancel_all(m_config.underlyingCode.c_str());
    }
}

//=============================================================================
// State Control
//=============================================================================

void WtOptionStrategy::start() {
    if (m_tradeContext->getState() == StrategyState::Idle ||
        m_tradeContext->getState() == StrategyState::Paused) {
        m_tradeContext->setState(StrategyState::Running);
        
        for (auto& scanner : m_scanners) {
            scanner->setEnabled(true);
        }
        
        onStrategyStart();
    }
}

void WtOptionStrategy::pause() {
    if (m_tradeContext->getState() == StrategyState::Running) {
        m_tradeContext->setState(StrategyState::Paused);
        
        for (auto& scanner : m_scanners) {
            scanner->setEnabled(false);
        }
        
        // Cancel all orders
        cancelAllOrders();
    }
}

void WtOptionStrategy::resume() {
    if (m_tradeContext->getState() == StrategyState::Paused) {
        m_tradeContext->setState(StrategyState::Running);
        
        for (auto& scanner : m_scanners) {
            scanner->setEnabled(true);
        }
    }
}

void WtOptionStrategy::panic() {
    m_tradeContext->setState(StrategyState::Panicked);
    
    // Cancel all orders
    cancelAllOrders();
    
    // Notify scanners
    for (auto& scanner : m_scanners) {
        scanner->onPanic();
    }
    
    onPanicTriggered();
}

void WtOptionStrategy::recover() {
    if (m_tradeContext->getState() == StrategyState::Panicked) {
        m_tradeContext->setState(StrategyState::Running);
        
        for (auto& scanner : m_scanners) {
            scanner->setEnabled(true);
        }
    }
}

StrategyState WtOptionStrategy::getState() const {
    return m_tradeContext->getState();
}

bool WtOptionStrategy::isEnabled() const {
    return m_tradeContext->isEnabled();
}

//=============================================================================
// Scanner Management
//=============================================================================

// ... addScanner ... (unchanged)
// ... removeScanner ... (unchanged)

void WtOptionStrategy::onScannerHit(const ScannerHitEvent& event) {
    if (event.option && m_tradeContext->canTrade()) {
        onOptionHit(event.option, event.signal, event.reason);
    }
}

//=============================================================================
// IOrderListener Implementation
//=============================================================================

void WtOptionStrategy::onOrderSent(const OptionOrder& order) {
    if (m_config.traceLevel > 0) {
        WTSLogger::log_by_cat("strategy", LL_INFO, 
            fmt::format("[Strategy] Order sent: {} {} {} @ {}",
                order.getCode(),
                (order.getDirection() == OrderDir::Buy ? "BUY" : "SELL"),
                order.getQuantity(),
                order.getPrice()
            ).c_str()
        );
    }
}

void WtOptionStrategy::onOrderFill(const OptionOrder& order, const FillEvent& fill) {
    if (m_config.traceLevel > 0) {
        WTSLogger::log_by_cat("strategy", LL_INFO, 
            fmt::format("[Strategy] Fill: {} {} @ {} P&L={}",
                order.getCode(),
                fill.fillQty,
                fill.fillPrice,
                order.getRealizedPnL()
            ).c_str()
        );
    }
    
    // Update risk
    m_risk->addFill(order.getCode(), 
                    order.getDirection() == OrderDir::Buy ? fill.fillQty : -fill.fillQty,
                    fill.fillPrice);
}

void WtOptionStrategy::onOrderCancelled(const OptionOrder& order) {
    if (m_config.traceLevel > 0) {
        WTSLogger::log_by_cat("strategy", LL_INFO, fmt::format("[Strategy] Cancelled: {}", order.getCode()).c_str());
    }
}

void WtOptionStrategy::onOrderRejected(const OptionOrder& order, const std::string& reason) {
    WTSLogger::log_by_cat("strategy", LL_ERROR, 
        fmt::format("[Strategy] Rejected: {} reason={}", order.getCode(), reason).c_str()
    );
}

//=============================================================================
// Protected Override Points
//=============================================================================

void WtOptionStrategy::onOptionHit(OptionData* option, double signal,
                                    const std::string& reason) {
    // Default implementation: log the hit
    if (m_config.traceLevel > 0) {
        WTSLogger::log_by_cat("strategy", LL_INFO, 
            fmt::format("[Scanner Hit] {} signal={} reason={}",
                option->getCode(), signal, reason
            ).c_str()
        );
    }
    
    // Override this to implement actual trading logic
}

void WtOptionStrategy::onRiskLimitBreached(const std::string& limitName,
                                            double value, double limit) {
    WTSLogger::log_by_cat("strategy", LL_ERROR, 
        fmt::format("[RISK] Limit breached: {} value={} limit={}", limitName, value, limit).c_str()
    );
    
    // Pause trading when risk limit breached
    pause();
}

//=============================================================================
// Internal Methods
//=============================================================================

void WtOptionStrategy::computeAllValues() {
    m_grid->computeValues();
}

void WtOptionStrategy::updateRisk() {
    m_risk->update();
}

void WtOptionStrategy::checkRiskLimits() {
    const auto& greeks = m_risk->getPositionGreeks();
    const auto& limits = m_config.riskLimits;
    
    // Check delta
    if (std::abs(greeks.delta()) > limits.maxDelta) {
        onRiskLimitBreached("Delta", greeks.delta(), limits.maxDelta);
    }
    
    // Check gamma
    if (std::abs(greeks.gamma()) > limits.maxGamma) {
        onRiskLimitBreached("Gamma", greeks.gamma(), limits.maxGamma);
    }
    
    // Check vega
    if (std::abs(greeks.vega()) > limits.maxVega) {
        onRiskLimitBreached("Vega", greeks.vega(), limits.maxVega);
    }
    
    // Check daily loss
    if (m_pnl.dayPnL < -limits.maxLossPerDay) {
        onRiskLimitBreached("DailyLoss", m_pnl.dayPnL, -limits.maxLossPerDay);
    }
}

void WtOptionStrategy::checkPanic() {
    // Check for large underlying move
    double undPrice = m_grid->getUnderlyingPrice();
    if (undPrice <= 0) return;
    
    if (m_lastPanicPrice > 0) {
        double move = std::abs(undPrice / m_lastPanicPrice - 1.0);
        if (move > m_config.riskLimits.panicThreshold) {
            panic();
            return;  // Don't update reference price on panic
        }
    }
    
    m_lastPanicPrice = undPrice;
}

void WtOptionStrategy::updatePnL() {
    // Calculate realized P&L from order manager
    auto stats = m_orderManager->getAggregateStats();
    double realized = stats.realizedPnL;
    
    // Calculate unrealized P&L from positions (mark-to-market)
    double unrealized = 0;
    for (const auto& riskData : m_risk->getNonZeroPositions()) {
        // Use per-position pnl which tracks cost basis correctly
        unrealized += riskData->netPnl;
    }
    
    m_pnl.update(realized, unrealized);
    m_pnl.optionPnL = realized + unrealized;
    
    // Include hedge P&L
    double hedgePnl = 0;
    for (const auto& hedge : m_risk->getHedgeInstruments()) {
        hedgePnl += hedge->getDeltaPosition() * m_grid->getUnderlyingPrice();
    }
    m_pnl.hedgePnL = hedgePnl;
}

void WtOptionStrategy::processOrders() {
    // Update all order managers
    m_orderManager->updateAllOrders(m_tradeContext->isPanicked());
}

//=============================================================================
// Configuration Loading
//=============================================================================

OptionStrategyConfig WtOptionStrategy::loadConfig(const std::string& filepath) {
    OptionStrategyConfig config;
    
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open config file: " << filepath << std::endl;
        return config;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    // Simple JSON parsing helper
    auto getValue = [&content](const std::string& key) -> std::string {
        size_t pos = content.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        
        size_t colonPos = content.find(":", pos);
        if (colonPos == std::string::npos) return "";
        
        size_t startPos = content.find_first_not_of(" \t\n\"", colonPos + 1);
        if (startPos == std::string::npos) return "";
        
        size_t endPos = content.find_first_of(",}\n\"", startPos);
        if (endPos == std::string::npos) endPos = content.length();
        
        return content.substr(startPos, endPos - startPos);
    };
    
    // Parse configuration
    std::string val;
    
    val = getValue("underlyingCode");
    if (!val.empty()) config.underlyingCode = val;
    
    val = getValue("exchangeCode");
    if (!val.empty()) config.exchangeCode = val;
    
    val = getValue("hedgeCode");
    if (!val.empty()) config.hedgeCode = val;
    
    val = getValue("defaultVolatility");
    if (!val.empty()) config.defaultVolatility = std::stod(val);
    
    val = getValue("riskFreeRate");
    if (!val.empty()) config.riskFreeRate = std::stod(val);
    
    val = getValue("traceLevel");
    if (!val.empty()) config.traceLevel = std::stoi(val);
    
    val = getValue("pricerType");
    if (!val.empty()) config.pricerType = val;
    
    val = getValue("enableCurveFitting");
    config.enableCurveFitting = (val == "true" || val == "1");
    
    // Load risk limits
    config.riskLimits = loadRiskLimits(filepath);
    
    // Load scanners
    config.scannerConfigs = loadScannerConfigs(filepath);
    
    return config;
}

std::vector<ScannerConfigPtr> WtOptionStrategy::loadScannerConfigs(const std::string& filepath) {
    std::vector<ScannerConfigPtr> configs;
    
    std::ifstream file(filepath);
    if (!file.is_open()) return configs;
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    // Find scanners array
    size_t arrayStart = content.find("\"scanners\"");
    if (arrayStart == std::string::npos) return configs;
    
    arrayStart = content.find("[", arrayStart);
    if (arrayStart == std::string::npos) return configs;
    
    // Helper: get string value for a key in a JSON fragment
    auto getValue = [](const std::string& json, const std::string& key) -> std::string {
        size_t pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        size_t colon = json.find(":", pos);
        if (colon == std::string::npos) return "";
        size_t start = json.find_first_not_of(" \t\n\"", colon + 1);
        size_t end = json.find_first_of(",}\n\"", start);
        if (end == std::string::npos) end = json.length();
        return json.substr(start, end - start);
    };

    auto getBool = [](const std::string& val) -> bool {
        return val == "true" || val == "1";
    };
    
    auto safeStod = [](const std::string& val, double def) -> double {
        if (val.empty()) return def;
        try { return std::stod(val); } catch (...) { return def; }
    };
    
    auto safeStoi = [](const std::string& val, int32_t def) -> int32_t {
        if (val.empty()) return def;
        try { return std::stoi(val); } catch (...) { return def; }
    };
    
    // Helper: find matching brace from position
    auto findMatchingBrace = [](const std::string& s, size_t openPos) -> size_t {
        int count = 0;
        for (size_t i = openPos; i < s.length(); i++) {
            if (s[i] == '{') count++;
            else if (s[i] == '}') { count--; if (count == 0) return i; }
        }
        return std::string::npos;
    };
    
    // Helper: parse expiryOverrides block from scanner JSON
    auto parseExpiryOverrides = [&](const std::string& json, ScannerConfig* config) {
        size_t ovPos = json.find("\"expiryOverrides\"");
        if (ovPos == std::string::npos) return;
        
        size_t ovObjStart = json.find("{", ovPos);
        if (ovObjStart == std::string::npos) return;
        size_t ovObjEnd = findMatchingBrace(json, ovObjStart);
        if (ovObjEnd == std::string::npos) return;
        
        std::string ovJson = json.substr(ovObjStart + 1, ovObjEnd - ovObjStart - 1);
        
        // Iterate expiry keys (e.g. "202505": { ... })
        size_t pos = 0;
        while (pos < ovJson.length()) {
            // Find next quoted key
            size_t keyStart = ovJson.find("\"", pos);
            if (keyStart == std::string::npos) break;
            size_t keyEnd = ovJson.find("\"", keyStart + 1);
            if (keyEnd == std::string::npos) break;
            
            std::string expiryStr = ovJson.substr(keyStart + 1, keyEnd - keyStart - 1);
            uint32_t expiry = 0;
            try { expiry = static_cast<uint32_t>(std::stoul(expiryStr)); } catch (...) { pos = keyEnd + 1; continue; }
            
            // Find the override object
            size_t valStart = ovJson.find("{", keyEnd);
            if (valStart == std::string::npos) break;
            size_t valEnd = findMatchingBrace(ovJson, valStart);
            if (valEnd == std::string::npos) break;
            
            std::string valJson = ovJson.substr(valStart, valEnd - valStart + 1);
            
            ScannerExpiryOverrides ov;
            std::string v;
            
            v = getValue(valJson, "enabled");
            if (!v.empty()) ov.enabled = getBool(v);
            
            v = getValue(valJson, "maxPosOpt");
            if (!v.empty()) ov.maxPosOpt = safeStoi(v, -1);
            
            v = getValue(valJson, "maxPosFut");
            if (!v.empty()) ov.maxPosFut = safeStoi(v, -1);
            
            v = getValue(valJson, "maxOrderSize");
            if (!v.empty()) ov.maxOrderSize = safeStoi(v, -1);
            
            v = getValue(valJson, "minProfit");
            if (!v.empty()) ov.minProfit = safeStod(v, -1);
            
            v = getValue(valJson, "minProfitVol");
            if (!v.empty()) ov.minProfitVol = safeStod(v, -1);
            
            config->expiryOverrides[expiry] = ov;
            pos = valEnd + 1;
        }
    };

    // Iterate scanner objects in array
    size_t curr = arrayStart + 1;
    while (curr < content.length()) {
        size_t objStart = content.find("{", curr);
        if (objStart == std::string::npos) break;
        
        size_t objEnd = findMatchingBrace(content, objStart);
        if (objEnd == std::string::npos) break;
        
        std::string objJson = content.substr(objStart, objEnd - objStart + 1);
        curr = objEnd + 1;
        
        // Parse Common Fields
        std::string name = getValue(objJson, "name");
        if (name.empty()) name = getValue(objJson, "type"); 
        
        std::string enabledStr = getValue(objJson, "enabled");
        bool enabled = enabledStr.empty() ? true : getBool(enabledStr);
        
        // Create Specific Config
        ScannerConfigPtr config = nullptr;
        
        if (name == "SpreadScanner") {
            auto sCfg = std::make_shared<SpreadScannerConfig>();
            sCfg->minSpread = safeStod(getValue(objJson, "minSpread"), sCfg->minSpread);
            sCfg->maxSpread = safeStod(getValue(objJson, "maxSpread"), sCfg->maxSpread);
            sCfg->minProfitPct = safeStod(getValue(objJson, "minProfitPct"), sCfg->minProfitPct);
            sCfg->scanCalls = getBool(getValue(objJson, "scanCalls"));
            sCfg->scanPuts = getBool(getValue(objJson, "scanPuts"));
            sCfg->maxOrderSize = safeStoi(getValue(objJson, "maxOrderSize"), sCfg->maxOrderSize);
            sCfg->maxPosOpt = safeStoi(getValue(objJson, "maxPosOpt"), sCfg->maxPosOpt);
            config = sCfg;
        }
        else if (name == "VolSpreadScanner") {
            auto vCfg = std::make_shared<VolSpreadScannerConfig>();
            vCfg->minVolSpread = safeStod(getValue(objJson, "minVolSpread"), vCfg->minVolSpread);
            vCfg->maxVolSpread = safeStod(getValue(objJson, "maxVolSpread"), vCfg->maxVolSpread);
            vCfg->maxOpenPositions = safeStoi(getValue(objJson, "maxOpenPositions"), vCfg->maxOpenPositions);
            vCfg->minDaysToExpiry = safeStoi(getValue(objJson, "minDaysToExpiry"), vCfg->minDaysToExpiry);
            vCfg->maxDaysToExpiry = safeStoi(getValue(objJson, "maxDaysToExpiry"), vCfg->maxDaysToExpiry);
            config = vCfg;
        }
        else if (name == "SyntheticFutureScanner") {
            auto synCfg = std::make_shared<SyntheticFutureScannerConfig>();
            synCfg->minProfitTicks = safeStod(getValue(objJson, "minProfitTicks"), synCfg->minProfitTicks);
            synCfg->buyThreshold = safeStod(getValue(objJson, "buyThreshold"), synCfg->buyThreshold);
            synCfg->sellThreshold = safeStod(getValue(objJson, "sellThreshold"), synCfg->sellThreshold);
            synCfg->maxOpenPositions = safeStoi(getValue(objJson, "maxOpenPositions"), synCfg->maxOpenPositions);
            synCfg->maxOrderSize = safeStoi(getValue(objJson, "maxOrderSize"), synCfg->maxOrderSize);
            synCfg->minDaysToExpiry = safeStoi(getValue(objJson, "minDaysToExpiry"), synCfg->minDaysToExpiry);
            synCfg->maxDaysToExpiry = safeStoi(getValue(objJson, "maxDaysToExpiry"), synCfg->maxDaysToExpiry);
            config = synCfg;
        }
        
        if (config) {
            config->name = name;
            config->enabled = enabled;
            // Parse per-expiry overrides
            parseExpiryOverrides(objJson, config.get());
            configs.push_back(config);
        }
        
        // Check for end of array
        size_t nextChar = content.find_first_not_of(" \t\n,", curr);
        if (nextChar != std::string::npos && content[nextChar] == ']') break;
    }
    
    return configs;
}

RiskLimits WtOptionStrategy::loadRiskLimits(const std::string& filepath) {
    RiskLimits limits;
    
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return limits;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    auto getValue = [&content](const std::string& key) -> std::string {
        size_t pos = content.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        
        size_t colonPos = content.find(":", pos);
        if (colonPos == std::string::npos) return "";
        
        size_t startPos = content.find_first_not_of(" \t\n\"", colonPos + 1);
        if (startPos == std::string::npos) return "";
        
        size_t endPos = content.find_first_of(",}\n\"", startPos);
        if (endPos == std::string::npos) endPos = content.length();
        
        return content.substr(startPos, endPos - startPos);
    };
    
    std::string val;
    
    val = getValue("maxDelta");
    if (!val.empty()) limits.maxDelta = std::stod(val);
    
    val = getValue("maxGamma");
    if (!val.empty()) limits.maxGamma = std::stod(val);
    
    val = getValue("maxVega");
    if (!val.empty()) limits.maxVega = std::stod(val);
    
    val = getValue("maxPositionPerOption");
    if (!val.empty()) limits.maxPositionPerOption = std::stod(val);
    
    val = getValue("maxTotalPosition");
    if (!val.empty()) limits.maxTotalPosition = std::stod(val);
    
    val = getValue("maxLossPerDay");
    if (!val.empty()) limits.maxLossPerDay = std::stod(val);
    
    val = getValue("panicThreshold");
    if (!val.empty()) limits.panicThreshold = std::stod(val);
    
    return limits;
}



} // namespace wt_option
