/*!
 * \file MicroAlphaEngine.h
 * \brief Micro-Alpha Prediction Engine for High-Frequency Market Making
 * 
 * Implements the Alpha prediction component of the GLFT+Alpha framework:
 *   - OFI (Order Flow Imbalance): measures order book pressure
 *   - Trade Imbalance: aggressive buy/sell flow
 *   - Lead-Lag Signal: cross-contract predictive signals
 * 
 * Output: Composite alpha signal α ∈ [-1, 1]
 *   α > 0: Bullish pressure, skew quotes up
 *   α < 0: Bearish pressure, skew quotes down
 *   α ≈ 0: No directional signal, pure market making
 * 
 * Formula for fair value adjustment:
 *   Fair Value: ŝ = s + η * α
 * 
 * Support for markets without transaction data:
 *   - REAL_TRANSACTION: Use real transaction data (Level 2)
 *   - SYNTHETIC: Use fused synthetic transaction data
 *   - TICK_INFERENCE: Use tick-level inference only
 */
#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include "../Share/RingBuffer.hpp"
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN
class WTSTickData;
class WTSOrdQueData;
class WTSOrdDtlData;
class WTSTransData;
NS_WTP_END

// Forward declaration for synthetic signal types
namespace futu {
struct SyntheticTransactionData;
struct FusedTradeImbalance;
struct InferredTradeImbalance;
}

namespace futu {

//==============================================================================
// Alpha Configuration
//==============================================================================

/// Micro-Alpha Engine Configuration
struct AlphaConfig
{
    // OFI parameters
    uint32_t    ofi_window;         ///< OFI calculation window (ticks)
    double      ofi_weight;         ///< Weight of OFI in composite alpha
    
    // Trade imbalance parameters
    uint32_t    trade_window;       ///< Trade imbalance window
    double      trade_weight;       ///< Weight of trade imbalance
    
    // Lead-lag parameters
    double      lead_lag_weight;    ///< Weight of lead-lag signal
    uint32_t    lead_lag_lag;       ///< Lag in ms for lead contract
    
    // Signal smoothing
    double      ema_alpha;          ///< EMA smoothing factor
    double      alpha_decay;        ///< Alpha decay rate per second
    
    // Thresholds
    double      strong_alpha_threshold; ///< Threshold for strong signal (single-side quoting)
    
    AlphaConfig()
        : ofi_window(20)
        , ofi_weight(0.4)
        , trade_window(50)
        , trade_weight(0.3)
        , lead_lag_weight(0.3)
        , lead_lag_lag(50)
        , ema_alpha(0.3)
        , alpha_decay(0.5)
        , strong_alpha_threshold(0.7)
    {}
};

//==============================================================================
// Trade Imbalance Data Source
//==============================================================================

/// Data source for trade imbalance calculation
enum class TradeImbalanceSource : uint8_t
{
    REAL_TRANSACTION = 0,   ///< Use real transaction data (Level 2)
    SYNTHETIC = 1,          ///< Use fused synthetic transaction data
    TICK_INFERENCE = 2      ///< Use tick-level inference only
};

//==============================================================================
// OFI (Order Flow Imbalance) Calculator
//==============================================================================

/// OFI calculation result
struct OFIResult
{
    double      ofi;                ///< Raw OFI value
    double      normalized_ofi;     ///< Normalized OFI (-1 to 1)
    double      bid_pressure;       ///< Buy-side pressure (0 to 1)
    double      ask_pressure;       ///< Sell-side pressure (0 to 1)
    uint64_t    timestamp;
    
    OFIResult()
        : ofi(0), normalized_ofi(0)
        , bid_pressure(0.5), ask_pressure(0.5)
        , timestamp(0)
    {}
};

/// Order Flow Imbalance Calculator
class OFICalculator
{
public:
    OFICalculator();
    
    void setWindow(uint32_t window) { _window = window; }
    
    /// Update with tick data
    void onTick(double bidPrice, double askPrice, 
                double bidVol, double askVol,
                uint64_t timestamp);
    
    /// Get current OFI
    OFIResult getOFI() const;
    
    void reset();
    
private:
    uint32_t _window;
    
    // Previous best bid/ask
    double _prev_bid_price;
    double _prev_ask_price;
    double _prev_bid_vol;
    double _prev_ask_vol;
    
    // OFI history for rolling sum
    struct OFISample {
        double ofi;
        uint64_t timestamp;
    };
    RingBuffer<OFISample, 128> _ofi_history;  // capacity must be power of 2
    
    double _cumulative_ofi;
    uint64_t _last_timestamp;
};

//==============================================================================
// Trade Imbalance Calculator
//==============================================================================

/// Trade imbalance result
struct TradeImbalanceResult
{
    double      net_flow;           ///< Net trade flow (buy - sell volume)
    double      imbalance_ratio;    ///< Normalized imbalance (-1 to 1)
    double      large_trade_ratio;  ///< Ratio of large trades
    uint64_t    timestamp;
    
    TradeImbalanceResult()
        : net_flow(0), imbalance_ratio(0)
        , large_trade_ratio(0), timestamp(0)
    {}
};

/// Trade Imbalance Calculator
class TradeImbalanceCalculator
{
public:
    TradeImbalanceCalculator();
    
    void setWindow(uint32_t window) { _window = window; }
    void setLargeTradeThreshold(double threshold) { _large_threshold = threshold; }
    
    /// Record a trade
    void onTrade(double price, double qty, bool isBuy, uint64_t timestamp);
    
    /// Get current trade imbalance
    TradeImbalanceResult getImbalance() const;
    
    void reset();
    
private:
    uint32_t _window;
    double _large_threshold;
    
    struct TradeSample {
        double signed_qty;  // Positive = buy, negative = sell
        bool is_large;
        uint64_t timestamp;
    };
    RingBuffer<TradeSample, 256> _trade_history;  // capacity must be power of 2
    
    double _net_flow;
    double _total_volume;
    double _large_volume;
};

//==============================================================================
// Lead-Lag Signal
//==============================================================================

/// Lead-lag signal result
struct LeadLagResult
{
    double      signal;             ///< Lead-lag signal (-1 to 1)
    double      correlation;        ///< Correlation between contracts
    uint64_t    timestamp;
    
    LeadLagResult()
        : signal(0), correlation(0), timestamp(0)
    {}
};

//==============================================================================
// Composite Alpha Result
//==============================================================================

/// Composite alpha result
struct AlphaResult
{
    double      alpha;              ///< Composite alpha (-1 to 1)
    double      ofi_component;      ///< OFI contribution
    double      trade_component;    ///< Trade imbalance contribution
    double      lead_lag_component; ///< Lead-lag contribution
    bool        is_strong_signal;   ///< Strong signal flag
    uint64_t    timestamp;
    
    AlphaResult()
        : alpha(0), ofi_component(0)
        , trade_component(0), lead_lag_component(0)
        , is_strong_signal(false), timestamp(0)
    {}
};

//==============================================================================
// Micro Alpha Engine
//==============================================================================

/// Micro-Alpha Prediction Engine
class MicroAlphaEngine
{
public:
    MicroAlphaEngine();
    ~MicroAlphaEngine() = default;
    
    //==========================================================================
    // Configuration
    //==========================================================================
    
    void setConfig(const AlphaConfig& config) { _config = config; }
    const AlphaConfig& getConfig() const { return _config; }
    
    void setContract(const std::string& code) { _code = code; }
    const std::string& getContract() const { return _code; }
    
    /// Add lead contract for lead-lag signal
    void addLeadContract(const std::string& leadCode);
    
    //==========================================================================
    // Data Input
    //==========================================================================
    
    /// Update with tick data
    void onTick(wtp::WTSTickData* tick);
    
    /// Update with tick data (unpacked)
    void onTick(const std::string& code,
                double bidPrice, double askPrice,
                double bidVol, double askVol,
                double lastPrice, uint64_t timestamp);
    
    /// Update with transaction (Level 2)
    void onTransaction(wtp::WTSTransData* trans);
    void onTransaction(const std::string& code,
                       double price, double qty, bool isBuy, uint64_t timestamp);
    
    /// Update lead contract tick (for lead-lag)
    void onLeadTick(const std::string& leadCode,
                    double bidPrice, double askPrice, uint64_t timestamp);
    
    //==========================================================================
    // Synthetic Signal Support (for markets without L2 transaction data)
    //==========================================================================
    
    /// Set trade imbalance data source
    void setTradeImbalanceSource(TradeImbalanceSource source) { _trade_source = source; }
    TradeImbalanceSource getTradeImbalanceSource() const { return _trade_source; }
    
    /// Update with synthetic transaction data (fused signal)
    void onSyntheticTransaction(const SyntheticTransactionData& synth_trans);
    
    /// Update with fused trade imbalance
    void onFusedTradeImbalance(const FusedTradeImbalance& fused_imb);
    
    /// Update with inferred trade imbalance (from tick inference)
    void onInferredTradeImbalance(const InferredTradeImbalance& inferred_imb);
    
    /// Check if using synthetic/inferred data
    bool isUsingSyntheticData() const { 
        return _trade_source != TradeImbalanceSource::REAL_TRANSACTION; 
    }
    
    //==========================================================================
    // Alpha Calculation
    //==========================================================================
    
    /// Get current composite alpha
    AlphaResult getAlpha() const;
    
    /// Get raw alpha (without EMA smoothing)
    double getRawAlpha() const;
    
    /// Get smoothed alpha (with EMA)
    double getSmoothedAlpha() const;
    
    //==========================================================================
    // Component Access
    //==========================================================================
    
    const OFICalculator& getOFI() const { return _ofi; }
    const TradeImbalanceCalculator& getTradeImbalance() const { return _trade_imb; }
    
    //==========================================================================
    // Data Input (for external updates)
    //==========================================================================
    
    /// Update trade data for imbalance calculation
    void onTrade(double price, double qty, bool isBuy, uint64_t timestamp);
    
    /// Update tick data for OFI calculation
    void onTick(double bidPrice, double askPrice, double bidVol, double askVol, uint64_t timestamp);
    
    //==========================================================================
    // Signal Analysis
    //==========================================================================
    
    /// Check if alpha is strong enough for single-side quoting
    bool isStrongSignal() const;
    
    /// Get recommended quoting side (1 = bid only, -1 = ask only, 0 = both)
    int getQuotingSide() const;
    
    //==========================================================================
    // Reset
    //==========================================================================
    
    void reset();
    
private:
    std::string _code;
    AlphaConfig _config;
    
    // Component calculators
    OFICalculator _ofi;
    TradeImbalanceCalculator _trade_imb;
    
    // Lead-lag tracking
    struct LeadContract {
        std::string code;
        double last_mid;
        double mid_change;
        uint64_t last_timestamp;
    };
    wtp::wt_hashmap<std::string, LeadContract> _lead_contracts;
    
    // Alpha state
    double _raw_alpha;
    double _smoothed_alpha;
    uint64_t _last_update;
    
    // Trade imbalance data source
    TradeImbalanceSource _trade_source = TradeImbalanceSource::REAL_TRANSACTION;
    
    // Calculate lead-lag signal
    double calculateLeadLagSignal() const;
};

} // namespace futu
