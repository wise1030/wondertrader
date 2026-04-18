/*!
 * \file SignalAggregator.h
 * \brief Aggregates multiple signal sources into a unified SignalContext
 */
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include "ISignalSource.h"
#include "MarketDataContext.h"
#include "VolatilitySignalSource.h"
#include "OFISignalSource.h"
#include "TradeFlowSignalSource.h"
#include "BookImbalanceSignalSource.h"
#include "MomentumSignalSource.h"
#include "LeadLagSignalSource.h"
#include "../WTSTools/WTSLogger.h"

namespace futu {

struct SignalAggregatorConfig
{
    // 信号源开关
    bool use_volatility = true;
    bool use_ofi = true;
    bool use_trade_flow = true;
    bool use_book_imbalance = true;    // 新增：订单簿不平衡
    bool use_momentum = true;
    bool use_lead_lag = false;
    
    // 信号源参数
    uint32_t volatility_window = 100;
    uint32_t ofi_window = 50;
    uint32_t trade_flow_window = 100;
    uint32_t momentum_window = 20;
    uint32_t lead_lag_window = 100;
    
    // 阈值参数
    double vol_threshold = 0.003;
    double spread_threshold = 5.0;
    double large_trade_threshold = 50.0;
    double momentum_ema_alpha = 0.1;
    double lead_lag_lag_ms = 500;
    double book_imbalance_threshold = 0.2;  // 新增：主导阈值
    
    // Alpha 权重参数（用于信号整合）
    double ofi_weight = 0.35;           // OFI 信号权重
    double trade_weight = 0.25;         // 交易流信号权重
    double book_imbalance_weight = 0.20;// 新增：订单簿不平衡权重
    double momentum_weight = 0.15;      // 动量信号权重
    double lead_lag_weight = 0.05;      // 领先滞后信号权重
    double strong_threshold = 0.7;      // 强信号阈值
};

class SignalAggregator
{
public:
    SignalAggregator() = default;
    
    // 支持配置的构造函数，匹配 make_unique(sig_cfg)
    explicit SignalAggregator(const SignalAggregatorConfig& cfg) {
        setConfig(cfg);
    }
    
    ~SignalAggregator() = default;
    
    void setConfig(const SignalAggregatorConfig& cfg) {
        _cfg = cfg;
        initializeSignalSources();
    }
    
    const SignalContext& update(const MarketDataContext& book) {
        _tick_count++;
        
        _ctx.code = book.getCode();
        _ctx.timestamp = book.getTimestamp();
        _ctx.mid_price = book.getMidPrice();
        _ctx.spread = book.getSpread();
        _ctx.spread_ticks = book.getSpreadTicks();
        _ctx.tick_size = book.getTickSize();
        
        // 修正方法名：match MarketDataContext.h
        _ctx.bid_price = book.getBidPrice();
        _ctx.ask_price = book.getAskPrice();
        _ctx.bid_vol = book.getBidVol();
        _ctx.ask_vol = book.getAskVol();
        
        _ctx.imbalance = book.getImbalance();
        _ctx.depth_imbalance = book.getDepthImbalance();
        _ctx.bid_depth = book.getBidDepth();
        _ctx.ask_depth = book.getAskDepth();
        _ctx.liquidity_score = book.estimateLiquidity();
        
        // Update all sources
        for (auto& pair : _sources) {
            pair.second->update(book);
        }
        
        // Extract results from signal sources
        extractSignalResults();
        
        // Compute alpha integration
        computeAlpha();
        
        // Update secondary market state
        computeMarketState();
        
        return _ctx;
    }
    
    const SignalContext& getContext() const { return _ctx; }
    
    bool is_ready() const { return _tick_count >= _warmup_ticks; }
    
    void reset() {
        _tick_count = 0;
        _ctx.reset();
        for (auto& pair : _sources) pair.second->reset();
    }

private:
    uint32_t _tick_count = 0;
    uint32_t _warmup_ticks = 50; // default warm-up period
    
    void initializeSignalSources() {
        _sources.clear();
        
        // Volatility signal source (always enabled)
        if (_cfg.use_volatility) {
            auto vol = std::make_unique<RealizedVolSignalSource>();
            vol->setWindowSize(_cfg.volatility_window);
            _vol_source = vol.get();
            _sources[SignalType::VOLATILITY] = std::move(vol);
        }
        
        // OFI signal source
        if (_cfg.use_ofi) {
            OFISignalSource::Config ofi_cfg;
            ofi_cfg.window = _cfg.ofi_window;
            _sources[SignalType::OFI] = std::make_unique<OFISignalSource>(ofi_cfg);
        }
        
        // Trade flow signal source
        if (_cfg.use_trade_flow) {
            TradeFlowSignalSource::Config flow_cfg;
            flow_cfg.window = _cfg.trade_flow_window;
            flow_cfg.large_trade_threshold = _cfg.large_trade_threshold;
            _sources[SignalType::TRADE_FLOW] = std::make_unique<TradeFlowSignalSource>(flow_cfg);
        }
        
        // Book imbalance signal source
        if (_cfg.use_book_imbalance) {
            BookImbalanceSignalSource::Config imb_cfg;
            imb_cfg.dominant_threshold = _cfg.book_imbalance_threshold;
            _sources[SignalType::BOOK_IMBALANCE] = std::make_unique<BookImbalanceSignalSource>(imb_cfg);
        }
        
        // Momentum signal source
        if (_cfg.use_momentum) {
            MomentumSignalSource::Config mom_cfg;
            mom_cfg.window = _cfg.momentum_window;
            mom_cfg.ema_alpha = _cfg.momentum_ema_alpha;
            _sources[SignalType::CUSTOM] = std::make_unique<MomentumSignalSource>(mom_cfg);
        }
        
        // Lead-lag signal source
        if (_cfg.use_lead_lag) {
            LeadLagSignalSource::Config ll_cfg;
            ll_cfg.window = _cfg.lead_lag_window;
            ll_cfg.lag_ms = _cfg.lead_lag_lag_ms;
            _sources[SignalType::CUSTOM] = std::make_unique<LeadLagSignalSource>(ll_cfg);
        }
    }
    
    void computeMarketState() {
        // 修正字段名：volatility -> realized_vol
        _ctx.market_state.vol_estimate = _ctx.volatility.realized_vol;
        _ctx.market_state.should_widen = (_ctx.volatility.realized_vol > _cfg.vol_threshold);
        if (_ctx.volatility.vol_tier == VolTier::EXTREME) {
            _ctx.market_state.should_pause = true;
        }
    }
    
    /// Extract signal results from signal sources
    void extractSignalResults() {
        // Volatility
        if (_vol_source) {
            _ctx.volatility = _vol_source->getVolatility();
        }
        
        // OFI
        auto ofi_it = _sources.find(SignalType::OFI);
        if (ofi_it != _sources.end() && ofi_it->second) {
            const auto& result = ofi_it->second->result();
            if (result.valid && result.type == SignalType::OFI) {
                _ctx.ofi = static_cast<const OFISignalResult&>(result);
            }
        }
        
        // TradeFlow
        auto flow_it = _sources.find(SignalType::TRADE_FLOW);
        if (flow_it != _sources.end() && flow_it->second) {
            const auto& result = flow_it->second->result();
            if (result.valid && result.type == SignalType::TRADE_FLOW) {
                _ctx.trade_flow = static_cast<const TradeFlowSignalResult&>(result);
            }
        }
        
        // BookImbalance
        auto imb_it = _sources.find(SignalType::BOOK_IMBALANCE);
        if (imb_it != _sources.end() && imb_it->second) {
            const auto& result = imb_it->second->result();
            if (result.valid && result.type == SignalType::BOOK_IMBALANCE) {
                _ctx.book_imbalance = static_cast<const BookImbalanceSignalResult&>(result);
            }
        }
    }
    
    /// Compute alpha by integrating multiple signals
    /// alpha = Σ(weight_i × signal_i) / Σ(weights)
    /// confidence = signal_consistency * signal_strength * warmup_factor
    void computeAlpha() {
        // Reset alpha
        _ctx.alpha = AlphaSignalResult();
        
        double alpha_sum = 0.0;
        double weight_sum = 0.0;
        
        // 收集有效信号用于计算一致性 (使用成员变量避免热路径内存分配)
        _valid_signals.clear();
        _valid_weights.clear();
        
        // OFI component
        if (_cfg.use_ofi && _ctx.ofi.valid) {
            _ctx.alpha.ofi_component = _ctx.ofi.ofi;  // ofi is normalized [-1, 1]
            alpha_sum += _cfg.ofi_weight * _ctx.ofi.ofi;
            weight_sum += _cfg.ofi_weight;
            _valid_signals.push_back(_ctx.ofi.ofi);
            _valid_weights.push_back(_cfg.ofi_weight);
        }
        
        // Trade flow component
        if (_cfg.use_trade_flow && _ctx.trade_flow.valid) {
            _ctx.alpha.trade_component = _ctx.trade_flow.net_flow_normalized;
            alpha_sum += _cfg.trade_weight * _ctx.trade_flow.net_flow_normalized;
            weight_sum += _cfg.trade_weight;
            _valid_signals.push_back(_ctx.trade_flow.net_flow_normalized);
            _valid_weights.push_back(_cfg.trade_weight);
        }
        
        // Book imbalance component
        if (_cfg.use_book_imbalance && _ctx.book_imbalance.valid) {
            _ctx.alpha.book_imbalance_component = _ctx.book_imbalance.simple_imbalance;
            alpha_sum += _cfg.book_imbalance_weight * _ctx.book_imbalance.simple_imbalance;
            weight_sum += _cfg.book_imbalance_weight;
            _valid_signals.push_back(_ctx.book_imbalance.simple_imbalance);
            _valid_weights.push_back(_cfg.book_imbalance_weight);
        }
        
        // Momentum component (if enabled)
        if (_cfg.use_momentum) {
            auto mom_it = _sources.find(SignalType::CUSTOM);
            if (mom_it != _sources.end() && mom_it->second) {
                const auto& result = mom_it->second->result();
                if (result.valid) {
                    // Momentum signal source returns AlphaSignalResult
                    const auto* alpha_result = dynamic_cast<const AlphaSignalResult*>(&result);
                    if (alpha_result) {
                        _ctx.alpha.lead_lag_component = alpha_result->alpha;
                        alpha_sum += _cfg.momentum_weight * alpha_result->alpha;
                        weight_sum += _cfg.momentum_weight;
                        _valid_signals.push_back(alpha_result->alpha);
                        _valid_weights.push_back(_cfg.momentum_weight);
                    }
                }
            }
        }
        
        // Lead-lag component
        if (_cfg.use_lead_lag) {
            auto ll_it = _sources.find(SignalType::CUSTOM); // Adjust if CUSTOM is used by LeadLag
            if (ll_it != _sources.end() && ll_it->second) {
                const auto& result = ll_it->second->result();
                if (result.valid) {
                    const auto* alpha_result = dynamic_cast<const AlphaSignalResult*>(&result);
                    if (alpha_result) {
                        alpha_sum += _cfg.lead_lag_weight * alpha_result->alpha;
                        weight_sum += _cfg.lead_lag_weight;
                        _valid_signals.push_back(alpha_result->alpha);
                        _valid_weights.push_back(_cfg.lead_lag_weight);
                    }
                }
            }
        }
        
        // Fallback Mechanism: If no primary signals are valid, try falling back to just book imbalance
        if (weight_sum <= 0.0 && _ctx.book_imbalance.valid) {
            alpha_sum = _ctx.book_imbalance.simple_imbalance;
            weight_sum = 1.0;
            _valid_signals.push_back(_ctx.book_imbalance.simple_imbalance);
            _valid_weights.push_back(1.0);
        }

        // Normalize and set valid flag
        _ctx.alpha.valid = is_ready() && (weight_sum > 0);
        if (_ctx.alpha.valid) {
            _ctx.alpha.alpha = alpha_sum / weight_sum;
            
            //======================================================================
            // 计算置信度 (Confidence)
            //======================================================================
            double confidence = 0.0;
            
            if (_valid_signals.size() > 0) {
                // 1. 信号一致性：计算各信号与加权平均方向的一致程度
                double consistency = 1.0;
                if (_valid_signals.size() > 1) {
                    int consistent_count = 0;
                    double avg_direction = (_ctx.alpha.alpha >= 0) ? 1.0 : -1.0;
                    for (size_t i = 0; i < _valid_signals.size(); ++i) {
                        double sig_direction = (_valid_signals[i] >= 0) ? 1.0 : -1.0;
                        if (sig_direction == avg_direction) {
                            consistent_count++;
                        }
                    }
                    consistency = static_cast<double>(consistent_count) / _valid_signals.size();
                }
                
                // 2. 信号强度：加权平均绝对值
                double strength = 0.0;
                double total_weight = 0.0;
                for (size_t i = 0; i < _valid_signals.size(); ++i) {
                    strength += _valid_weights[i] * std::abs(_valid_signals[i]);
                    total_weight += _valid_weights[i];
                }
                strength = (total_weight > 0) ? strength / total_weight : 0.0;
                
                // 3. 预热完成度：线性增长
                double warmup_factor = std::min(1.0, static_cast<double>(_tick_count) / _warmup_ticks);
                
                // 综合置信度
                confidence = consistency * strength * warmup_factor;
            }
            
            _ctx.alpha.confidence = confidence;
        }
        
        // Determine strong signal
        _ctx.alpha.is_strong_signal = std::abs(_ctx.alpha.alpha) > _cfg.strong_threshold;
    }

    SignalAggregatorConfig _cfg;
    SignalContext _ctx;
    std::unordered_map<SignalType, std::unique_ptr<ISignalSource>> _sources;
    VolatilitySignalSource* _vol_source = nullptr;
    
    // Pre-allocated vectors for zero-allocation hotpath
    std::vector<double> _valid_signals;
    std::vector<double> _valid_weights;
};

} // namespace futu
