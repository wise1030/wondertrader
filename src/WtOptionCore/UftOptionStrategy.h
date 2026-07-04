/*!
 * \file UftOptionStrategy.h
 * \brief Option MM Strategy — UFT full integration (Stage 5)
 *
 * Connects: OptionGrid → CompositeOptionPricer → ControllableTradingGrid → IUftStraCtx
 * Full chain: on_tick → grid.onTick(discovery) → computeValues → CTG.refresh → quote/buy/sell
 */
#pragma once

#include "../Includes/UftStrategyDefs.h"
#include "../Includes/IUftStraCtx.h"
#include "../Includes/WTSVariant.hpp"
#include "OptionAsyncEventProcessor.h"
#include "OptionQuoteManager.h"

#include <string>
#include <memory>
#include <atomic>
#include <unordered_map>
#include <map>

namespace wt_option {
class OptionGrid;
class OptionTradingGrid;
class CompositeOptionPricer;
class ControllableTradingGrid;
class OptionRisk;
class OptionQuoteManager;
struct OptionTraderContext;
using OptionGridPtr = std::shared_ptr<OptionGrid>;
using OptionTradingGridPtr = std::shared_ptr<OptionTradingGrid>;
using CompositeOptionPricerPtr = std::shared_ptr<CompositeOptionPricer>;
using ControllableTradingGridPtr = std::shared_ptr<ControllableTradingGrid>;
using OptionRiskPtr = std::shared_ptr<OptionRisk>;
class OptionPricer2;
class IScanModule;

using OptionGridPtr = std::shared_ptr<OptionGrid>;
using CompositeOptionPricerPtr = std::shared_ptr<CompositeOptionPricer>;
using ControllableTradingGridPtr = std::shared_ptr<ControllableTradingGrid>;
}

class UftOptionStrategy : public UftStrategy
{
public:
    UftOptionStrategy(const char* name);
    virtual ~UftOptionStrategy();

    virtual const char* getName() override { return "OptionMM"; }
    virtual const char* getFactName() override { return "OptionStraFact"; }

    virtual bool init(WTSVariant* cfg) override;
    virtual void on_init(IUftStraCtx* ctx) override;
    virtual void on_tick(IUftStraCtx* ctx, const char* stdCode, WTSTickData* newTick) override;
    virtual void on_trade(IUftStraCtx* ctx, uint32_t localid, const char* stdCode,
                          bool isLong, uint32_t offset, double vol, double price) override;
    virtual void on_order(IUftStraCtx* ctx, uint32_t localid, const char* stdCode,
                          bool isLong, uint32_t offset, double totalQty, double leftQty,
                          double price, bool isCanceled) override;
    virtual void on_position(IUftStraCtx* ctx, const char* stdCode, bool isLong,
                             double prevol, double preavail, double newvol, double newavail) override;
    virtual void on_channel_ready(IUftStraCtx* ctx) override;
    virtual void on_channel_lost(IUftStraCtx* ctx) override;
    virtual void on_entrust(uint32_t localid, bool bSuccess, const char* message) override;
    virtual void on_session_begin(IUftStraCtx* ctx, uint32_t uTDate) override;
    virtual void on_session_end(IUftStraCtx* ctx, uint32_t uTDate) override;
    virtual void on_params_updated() override;

private:
    // Async event processor
    wt_option::OptionAsyncEventProcessorPtr _async;

    // Core components
    wt_option::OptionGridPtr _grid;
    wt_option::OptionTradingGridPtr _otg;       // NEW: middle layer
    wt_option::CompositeOptionPricerPtr _pricer;
    wt_option::ControllableTradingGridPtr _ctg;
    wt_option::OptionRiskPtr _risk;              // NEW: risk instance
    std::shared_ptr<wt_option::OptionTraderContext> _traderCtx;

    // Config
    std::string _underlyingCode;
    std::string _optionProduct;
    std::string _exchange;
    double _riskFreeRate = 0.03;
    int32_t _maxTPS = 50;
    // Phase 6: alpha + pricing params from config
    double _wgt_vegaflow = 0, _wgt_frontfut_skew = 0, _wgt_deltaflow = 0;
    double _wgt_atmsig = 0, _wgt_rollema = 0;
    double _sticky_base = 0.5, _improve_retreat = 3.0;

    // P11: Pricer strategy type (strategy-level decision)
    std::string _pricerType = "composite_mm";

    // P11: Config pointer saved for setupPricer (avoids member variable transit)
    wtp::WTSVariant* cfgPtr_ = nullptr;

    // P11: OQM configs (per instrument type)
    wt_option::OptionQuoteManager::Config _optionOqmCfg;
    wt_option::OptionQuoteManager::Config _futureOqmCfg;

    // P10: Hot-update params (sync_param pointers, nullptr in backtest)
    struct ExpiryConfig {
        bool enable = true;
        double delta_min = 0.05, delta_max = 0.95;
        double sprd_fwd = 0.01, sprd_atmvol = 0.1, sprd_corr = 0.0;
        int32_t max_pos_fut = 1, max_pos_stk = 50, max_pos_opt = 50, max_qsize = 5;
        bool enable_auto_close = false;
        double close_pos_thresh = 0;
    };
    std::map<uint32_t, ExpiryConfig> _expiryConfigs;

    struct HotParams {
        double* wgt_vegaflow = nullptr;
        double* wgt_frontfut_skew = nullptr;
        double* wgt_deltaflow = nullptr;
        double* wgt_atmsig = nullptr;
        double* wgt_rollema = nullptr;
        double* sticky_base = nullptr;
        double* improve_retreat_ratio = nullptr;
        double* trade_shock_ticks = nullptr;
        int32_t* max_tps = nullptr;
        // Runtime control
        int32_t* command = nullptr;   // 0=normal, 1=stop, 2=panic, 3=resume
        int32_t* qmode_override = nullptr;  // 0=no override, 1=ON, -1=OFF, 2=CLOSE
    } _hot;

    double hotVal(double* ptr, double fallback) const {
        return ptr ? *ptr : fallback;
    }
    int32_t hotVal(int32_t* ptr, int32_t fallback) const {
        return ptr ? *ptr : fallback;
    }

    // State
    IUftStraCtx* _ctx = nullptr;
    std::atomic<bool> _initialized{false};
    std::atomic<bool> _channelReady{false};

    // Position tracking (strategy local)
    std::unordered_map<std::string, double> _positions;

    // Tick counter (for backtest diagnostics)
    std::atomic<uint64_t> _tickCount{0};

    // Option contracts to subscribe (from config)
    std::vector<std::string> _optionCodes;

    // Setup helpers
    void setupGrid();
    void setupPricer();
    void setupCTG();
    void setupAsyncCallbacks();

    // Order executors (bridge to IUftStraCtx)
    int32_t executeQuote(const std::string& code, double bidP, uint32_t bidQ,
                         double askP, uint32_t askQ);
    int32_t executeOrder(const std::string& code, bool isBuy, double price, uint32_t qty);
    int32_t executeCancel(const std::string& code);
};
