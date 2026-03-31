/*!
 * \file MicroAlphaEngine.cpp
 * \brief Micro-Alpha Prediction Engine Implementation
 * 
 * Implements three alpha components:
 * 1. OFI (Order Flow Imbalance) - measures order book pressure
 * 2. Trade Imbalance - aggressive buy/sell flow
 * 3. Lead-Lag - cross-contract predictive signals
 * 
 * Composite Alpha: α = w_ofi * α_ofi + w_trade * α_trade + w_ll * α_ll
 * 
 * Support for markets without L2 transaction data:
 * - Synthetic transaction data from multi-source fusion
 * - Tick-level inference for trade imbalance
 */
#include "MicroAlphaEngine.h"
#include "SyntheticSignalFusion.h"
#include "TickTransactionInferer.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/TimeUtils.hpp"
#include <algorithm>
#include <cmath>

namespace futu {

//==============================================================================
// OFI Calculator Implementation
//==============================================================================

OFICalculator::OFICalculator()
    : _window(20)
    , _prev_bid_price(0)
    , _prev_ask_price(0)
    , _prev_bid_vol(0)
    , _prev_ask_vol(0)
    , _cumulative_ofi(0)
    , _last_timestamp(0)
{
}

void OFICalculator::onTick(double bidPrice, double askPrice,
                           double bidVol, double askVol,
                           uint64_t timestamp)
{
    if (_prev_bid_price <= 0 || _prev_ask_price <= 0)
    {
        // First tick, just store values
        _prev_bid_price = bidPrice;
        _prev_ask_price = askPrice;
        _prev_bid_vol = bidVol;
        _prev_ask_vol = askVol;
        _last_timestamp = timestamp;
        return;
    }
    
    // Calculate OFI using the standard formulation:
    // OFI = e_bid - e_ask
    // where e_bid = bid volume change if bid price unchanged
    //              = bid volume if bid price moved up
    //              = -bid volume if bid price moved down
    // similarly for e_ask
    
    double e_bid = 0, e_ask = 0;
    
    // Bid side
    if (bidPrice > _prev_bid_price)
    {
        // Bid price moved up - new volume at new level
        e_bid = bidVol;
    }
    else if (bidPrice < _prev_bid_price)
    {
        // Bid price moved down - volume removed
        e_bid = -_prev_bid_vol;
    }
    else
    {
        // Bid price unchanged - net volume change
        e_bid = bidVol - _prev_bid_vol;
    }
    
    // Ask side (negative because ask volume increase is bearish)
    if (askPrice > _prev_ask_price)
    {
        // Ask price moved up - volume removed from previous level
        e_ask = -_prev_ask_vol;
    }
    else if (askPrice < _prev_ask_price)
    {
        // Ask price moved down - new volume at new level
        e_ask = askVol;
    }
    else
    {
        // Ask price unchanged - net volume change (negative for bearish)
        e_ask = -(askVol - _prev_ask_vol);
    }
    
    // OFI = e_bid - e_ask
    double ofi = e_bid - e_ask;
    
    // Store in history
    OFISample sample;
    sample.ofi = ofi;
    sample.timestamp = timestamp;
    _ofi_history.push(sample);
    
    // Update cumulative OFI (rolling sum)
    _cumulative_ofi += ofi;
    
    // Remove old samples
    uint64_t cutoff = timestamp - 5000;  // 5 second window
    while (_ofi_history.size() > 0 && _ofi_history[0].timestamp < cutoff)
    {
        _cumulative_ofi -= _ofi_history[0].ofi;
        _ofi_history.pop();
    }
    
    // Store current values
    _prev_bid_price = bidPrice;
    _prev_ask_price = askPrice;
    _prev_bid_vol = bidVol;
    _prev_ask_vol = askVol;
    _last_timestamp = timestamp;
}

OFIResult OFICalculator::getOFI() const
{
    OFIResult result;
    result.ofi = _cumulative_ofi;
    result.timestamp = _last_timestamp;
    
    // Normalize OFI
    // Typical OFI ranges from -500 to +500 for liquid contracts
    // We want to map this to [-1, 1]
    double abs_ofi = std::abs(_cumulative_ofi);
    double normalization = 200.0;  // Scale factor
    result.normalized_ofi = std::tanh(_cumulative_ofi / normalization);
    
    // Calculate bid/ask pressure
    if (_cumulative_ofi > 0)
    {
        result.bid_pressure = 0.5 + 0.5 * result.normalized_ofi;
        result.ask_pressure = 0.5 - 0.5 * result.normalized_ofi;
    }
    else
    {
        result.bid_pressure = 0.5 + 0.5 * result.normalized_ofi;
        result.ask_pressure = 0.5 - 0.5 * result.normalized_ofi;
    }
    
    return result;
}

void OFICalculator::reset()
{
    _prev_bid_price = 0;
    _prev_ask_price = 0;
    _prev_bid_vol = 0;
    _prev_ask_vol = 0;
    _cumulative_ofi = 0;
    _last_timestamp = 0;
    _ofi_history.clear();
}

//==============================================================================
// Trade Imbalance Calculator Implementation
//==============================================================================

TradeImbalanceCalculator::TradeImbalanceCalculator()
    : _window(50)
    , _large_threshold(10.0)
    , _net_flow(0)
    , _total_volume(0)
    , _large_volume(0)
{
}

void TradeImbalanceCalculator::onTrade(double price, double qty, 
                                        bool isBuy, uint64_t timestamp)
{
    TradeSample sample;
    sample.signed_qty = isBuy ? qty : -qty;
    sample.is_large = (qty >= _large_threshold);
    sample.timestamp = timestamp;
    
    _trade_history.push(sample);
    
    // Update running totals
    _net_flow += sample.signed_qty;
    _total_volume += qty;
    if (sample.is_large)
        _large_volume += qty;
    
    // Remove old samples
    uint64_t cutoff = timestamp - 5000;  // 5 second window
    while (_trade_history.size() > 0 && _trade_history[0].timestamp < cutoff)
    {
        const TradeSample& old = _trade_history[0];
        _net_flow -= old.signed_qty;
        _total_volume -= std::abs(old.signed_qty);
        if (old.is_large)
            _large_volume -= std::abs(old.signed_qty);
        _trade_history.pop();
    }
}

TradeImbalanceResult TradeImbalanceCalculator::getImbalance() const
{
    TradeImbalanceResult result;
    result.net_flow = _net_flow;
    result.timestamp = 0;  // Will be set by caller
    
    // Normalize imbalance
    if (_total_volume > 0)
    {
        result.imbalance_ratio = _net_flow / _total_volume;
        result.large_trade_ratio = _large_volume / _total_volume;
    }
    
    return result;
}

void TradeImbalanceCalculator::reset()
{
    _net_flow = 0;
    _total_volume = 0;
    _large_volume = 0;
    _trade_history.clear();
}

//==============================================================================
// Micro Alpha Engine Implementation
//==============================================================================

MicroAlphaEngine::MicroAlphaEngine()
    : _raw_alpha(0)
    , _smoothed_alpha(0)
    , _last_update(0)
{
}

void MicroAlphaEngine::addLeadContract(const std::string& leadCode)
{
    LeadContract lc;
    lc.code = leadCode;
    lc.last_mid = 0;
    lc.mid_change = 0;
    lc.last_timestamp = 0;
    _lead_contracts[leadCode] = lc;
}

void MicroAlphaEngine::onTick(wtp::WTSTickData* tick)
{
    if (!tick) return;
    
    std::string code = tick->code();
    onTick(code, 
           tick->bidprice(0), tick->askprice(0),
           tick->bidqty(0), tick->askqty(0),
           tick->price(), tick->actiontime());
}

void MicroAlphaEngine::onTick(const std::string& code,
                               double bidPrice, double askPrice,
                               double bidVol, double askVol,
                               double lastPrice, uint64_t timestamp)
{
    // Update OFI
    _ofi.onTick(bidPrice, askPrice, bidVol, askVol, timestamp);
    
    // Update timestamp
    _last_update = timestamp;
    
    // Recalculate alpha
    AlphaResult result = getAlpha();
    _raw_alpha = result.alpha;
    
    // Apply EMA smoothing
    _smoothed_alpha = _config.ema_alpha * _raw_alpha + 
                      (1 - _config.ema_alpha) * _smoothed_alpha;
}

void MicroAlphaEngine::onTransaction(wtp::WTSTransData* trans)
{
    if (!trans) return;
    
    // Note: WTS trans data has buy/sell flag
    // Implementation depends on WTS data structure
}

void MicroAlphaEngine::onTransaction(const std::string& code,
                                      double price, double qty, 
                                      bool isBuy, uint64_t timestamp)
{
    // Only process trades for our contract
    if (code != _code) return;
    
    _trade_imb.onTrade(price, qty, isBuy, timestamp);
}

void MicroAlphaEngine::onLeadTick(const std::string& leadCode,
                                   double bidPrice, double askPrice,
                                   uint64_t timestamp)
{
    auto it = _lead_contracts.find(leadCode);
    if (it == _lead_contracts.end()) return;
    
    LeadContract& lc = it->second;
    double mid = (bidPrice + askPrice) / 2.0;
    
    if (lc.last_mid > 0)
    {
        lc.mid_change = (mid - lc.last_mid) / lc.last_mid;
    }
    
    lc.last_mid = mid;
    lc.last_timestamp = timestamp;
}

double MicroAlphaEngine::calculateLeadLagSignal() const
{
    if (_lead_contracts.empty()) return 0;
    
    // Sum up signals from all lead contracts
    double total_signal = 0;
    for (const auto& kv : _lead_contracts)
    {
        const LeadContract& lc = kv.second;
        total_signal += lc.mid_change;
    }
    
    // Normalize to [-1, 1]
    return std::tanh(total_signal * 100);  // Scale for reasonable range
}

AlphaResult MicroAlphaEngine::getAlpha() const
{
    AlphaResult result;
    result.timestamp = _last_update;
    
    // Get OFI component
    OFIResult ofi = _ofi.getOFI();
    result.ofi_component = ofi.normalized_ofi;
    
    // Get trade imbalance component
    TradeImbalanceResult trade = _trade_imb.getImbalance();
    result.trade_component = trade.imbalance_ratio;
    
    // Get lead-lag component
    result.lead_lag_component = calculateLeadLagSignal();
    
    // Composite alpha (weighted sum)
    result.alpha = _config.ofi_weight * result.ofi_component +
                   _config.trade_weight * result.trade_component +
                   _config.lead_lag_weight * result.lead_lag_component;
    
    // Clamp to [-1, 1]
    result.alpha = std::clamp(result.alpha, -1.0, 1.0);
    
    // Check for strong signal
    result.is_strong_signal = std::abs(result.alpha) >= _config.strong_alpha_threshold;
    
    return result;
}

double MicroAlphaEngine::getRawAlpha() const
{
    return _raw_alpha;
}

double MicroAlphaEngine::getSmoothedAlpha() const
{
    return _smoothed_alpha;
}

bool MicroAlphaEngine::isStrongSignal() const
{
    return std::abs(_smoothed_alpha) >= _config.strong_alpha_threshold;
}

int MicroAlphaEngine::getQuotingSide() const
{
    if (!isStrongSignal()) return 0;  // Quote both sides
    
    if (_smoothed_alpha > 0) return 1;   // Strong bullish - bid only
    return -1;  // Strong bearish - ask only
}

void MicroAlphaEngine::reset()
{
    _ofi.reset();
    _trade_imb.reset();
    
    for (auto& kv : _lead_contracts)
    {
        kv.second.last_mid = 0;
        kv.second.mid_change = 0;
        kv.second.last_timestamp = 0;
    }
    
    _raw_alpha = 0;
    _smoothed_alpha = 0;
    _last_update = 0;
}

//==========================================================================
// Data Input Methods
//==========================================================================

void MicroAlphaEngine::onTrade(double price, double qty, bool isBuy, uint64_t timestamp)
{
    _trade_imb.onTrade(price, qty, isBuy, timestamp);
}

void MicroAlphaEngine::onTick(double bidPrice, double askPrice, double bidVol, double askVol, uint64_t timestamp)
{
    _ofi.onTick(bidPrice, askPrice, bidVol, askVol, timestamp);
}

//==========================================================================
// Synthetic Signal Support Methods
//==========================================================================

void MicroAlphaEngine::onSyntheticTransaction(const SyntheticTransactionData& synth_trans)
{
    // Only process if we're using synthetic data source
    if (_trade_source == TradeImbalanceSource::REAL_TRANSACTION) {
        return;
    }
    
    // Convert synthetic transaction to trade imbalance input
    // Use the confidence-weighted volume
    double effective_vol = synth_trans.volume * synth_trans.confidence;
    
    _trade_imb.onTrade(
        synth_trans.price,
        effective_vol,
        synth_trans.is_buy_initiated,
        synth_trans.timestamp
    );
    
    _last_update = synth_trans.timestamp;
}

void MicroAlphaEngine::onFusedTradeImbalance(const FusedTradeImbalance& fused_imb)
{
    // Only process if we're using synthetic data source
    if (_trade_source == TradeImbalanceSource::REAL_TRANSACTION) {
        return;
    }
    
    // Update internal trade imbalance state from fused result
    // This is a direct update without going through individual trades
    
    // Use a synthetic approach: create a virtual trade that represents the imbalance
    // The imbalance_ratio tells us the direction and magnitude
    if (fused_imb.sample_count > 0 && fused_imb.confidence > 0.1) {
        // Calculate effective volume based on net flow
        double effective_vol = std::abs(fused_imb.net_flow) * fused_imb.confidence;
        bool is_buy = fused_imb.net_flow > 0;
        
        // This updates the internal state of TradeImbalanceCalculator
        // Note: This is a simplified approach; for more accuracy,
        // we would need to expose a direct update method in TradeImbalanceCalculator
        _trade_imb.onTrade(0, effective_vol, is_buy, fused_imb.timestamp);
    }
    
    _last_update = fused_imb.timestamp;
}

void MicroAlphaEngine::onInferredTradeImbalance(const InferredTradeImbalance& inferred_imb)
{
    // Only process if we're using tick inference data source
    if (_trade_source == TradeImbalanceSource::REAL_TRANSACTION) {
        return;
    }
    
    // Similar to fused imbalance, create a virtual trade
    if (inferred_imb.confidence > 0.1) {
        double effective_vol = std::abs(inferred_imb.net_flow) * inferred_imb.confidence;
        bool is_buy = inferred_imb.net_flow > 0;
        
        _trade_imb.onTrade(0, effective_vol, is_buy, inferred_imb.timestamp);
    }
    
    _last_update = inferred_imb.timestamp;
}

} // namespace futu
