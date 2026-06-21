/*!
 * \file SignalAggregator.h
 * \brief Aggregates multiple signal sources into a unified SignalContext
 */
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include "FutuConfig.h"
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
    bool use_lead_lag = true;
    
    // 信号源参数
    uint32_t volatility_window = 100;
    uint32_t ofi_window = 50;
    uint32_t trade_flow_window = 100;
    uint32_t momentum_window = 50;
    uint32_t lead_lag_window = 50;
    
    // 阈值参数
    double vol_threshold = 0.003;
    
    // Alpha 权重配置
    double ofi_weight = 0.35;
    double trade_weight = 0.25;
    double book_imbalance_weight = 0.20;
    double momentum_weight = 0.15;
    double lead_lag_weight = 0.05;
    double strong_threshold = 0.7;
    
    // 交易流/订单簿/动量/领先滞后子参数
    double large_trade_threshold = 50.0;
    double book_imbalance_threshold = 0.2;
    double momentum_ema_alpha = 0.1;
    uint32_t lead_lag_lag_ms = 50;
    
    uint32_t warmup_ticks = 50;
    
    static SignalAggregatorConfig fromVariant(wtp::WTSVariant* v) {
        SignalAggregatorConfig c;
        c.use_volatility = FutuConfig::readBool(v, "useVolatility", true);
        c.use_ofi = FutuConfig::readBool(v, "useOfi", true);
        c.use_trade_flow = FutuConfig::readBool(v, "useTradeFlow", true);
        c.use_book_imbalance = FutuConfig::readBool(v, "useBookImbalance", true);
        c.use_momentum = FutuConfig::readBool(v, "useMomentum", true);
        c.use_lead_lag = FutuConfig::readBool(v, "useLeadLag", true);
        c.volatility_window = FutuConfig::readUInt32(v, "volatilityWindow", 100);
        c.ofi_window = FutuConfig::readUInt32(v, "ofiWindow", 50);
        c.trade_flow_window = FutuConfig::readUInt32(v, "tradeFlowWindow", 100);
        c.momentum_window = FutuConfig::readUInt32(v, "momentumWindow", 50);
        c.lead_lag_window = FutuConfig::readUInt32(v, "leadLagWindow", 50);
c.vol_threshold = FutuConfig::readDouble(v, "volThreshold", 0.003);
        c.ofi_weight = FutuConfig::readDouble(v, "ofiWeight", 0.35);
        c.trade_weight = FutuConfig::readDouble(v, "tradeWeight", 0.25);
        c.book_imbalance_weight = FutuConfig::readDouble(v, "bookImbalanceWeight", 0.20);
        c.momentum_weight = FutuConfig::readDouble(v, "momentumWeight", 0.15);
        c.lead_lag_weight = FutuConfig::readDouble(v, "leadLagWeight", 0.05);
        c.strong_threshold = FutuConfig::readDouble(v, "strongThreshold", 0.7);
        c.large_trade_threshold = FutuConfig::readDouble(v, "largeTradeThreshold", 50.0);
        c.book_imbalance_threshold = FutuConfig::readDouble(v, "bookImbalanceThreshold", 0.2);
        c.momentum_ema_alpha = FutuConfig::readDouble(v, "momentumEmaAlpha", 0.1);
        c.lead_lag_lag_ms = FutuConfig::readUInt32(v, "leadLagLagMs", 50);
        c.warmup_ticks = FutuConfig::readUInt32(v, "warmupTicks", 50);
        return c;
    }
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
        _warmup_ticks = cfg.warmup_ticks;
        initializeSignalSources();
    }
    
    void updateWeights(const SignalAggregatorConfig& cfg) {
        _cfg.ofi_weight = cfg.ofi_weight;
        _cfg.trade_weight = cfg.trade_weight;
        _cfg.book_imbalance_weight = cfg.book_imbalance_weight;
        _cfg.momentum_weight = cfg.momentum_weight;
        _cfg.lead_lag_weight = cfg.lead_lag_weight;
        _cfg.strong_threshold = cfg.strong_threshold;
    }
    
    const SignalAggregatorConfig& getConfig() const { return _cfg; }
    
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
    SignalContext& getContext() { return _ctx; }
    
    //==========================================================================
    // Lead-Lag Cross-Contract Data Feed
    //==========================================================================
    
    /// Update lead contract price data for the LeadLag signal source.
    /// Called when a lead/anchor contract tick arrives, feeding its mid price
    /// to this aggregator's LeadLagSignalSource so it can compute cross-contract
    /// predictive signals.
    void updateLeadContract(const std::string& code, double mid, uint64_t timestamp)
    {
        auto ll_it = _sources.find(SignalType::LEAD_LAG);
        if (ll_it != _sources.end() && ll_it->second)
        {
            auto* ll = dynamic_cast<LeadLagSignalSource*>(ll_it->second.get());
            if (ll)
            {
                ll->updateLeadContract(code, mid, timestamp);
            }
        }
    }
    
    /// Add a lead contract to the LeadLag signal source.
    /// Must be called before ticks arrive (during initialization).
    void addLeadContract(const std::string& code, double correlation = 1.0)
    {
        auto ll_it = _sources.find(SignalType::LEAD_LAG);
        if (ll_it != _sources.end() && ll_it->second)
        {
            auto* ll = dynamic_cast<LeadLagSignalSource*>(ll_it->second.get());
            if (ll)
            {
                ll->addLeadContract(code, correlation);
            }
        }
    }
    
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
            _sources[SignalType::MOMENTUM] = std::make_unique<MomentumSignalSource>(mom_cfg);
        }
        
        if (_cfg.use_lead_lag) {
            LeadLagSignalSource::Config ll_cfg;
            ll_cfg.window = _cfg.lead_lag_window;
            ll_cfg.lag_ms = _cfg.lead_lag_lag_ms;
            _sources[SignalType::LEAD_LAG] = std::make_unique<LeadLagSignalSource>(ll_cfg);
        }
    }
    
    void computeMarketState() {
        // 修正字段名：volatility -> realized_vol
        _ctx.market_state.vol_estimate = _ctx.volatility.realized_vol;
        _ctx.market_state.should_widen = (_ctx.volatility.realized_vol > _cfg.vol_threshold);
        // should_pause每tick重算，不复位锁存
        // 原代码只在vol_tier==EXTREME时设true，无else分支复位false
        // 导致should_pause一旦被设就永久锁死，报价永远被阻止
        _ctx.market_state.should_pause = (_ctx.volatility.vol_tier == VolTier::EXTREME);
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
            auto mom_it = _sources.find(SignalType::MOMENTUM);
            if (mom_it != _sources.end() && mom_it->second) {
                const auto& result = mom_it->second->result();
                if (result.valid) {
                    double alpha = mom_it->second->getAlphaValue();
                    _ctx.alpha.momentum_component = alpha;
                    alpha_sum += _cfg.momentum_weight * alpha;
                    weight_sum += _cfg.momentum_weight;
                    _valid_signals.push_back(alpha);
                    _valid_weights.push_back(_cfg.momentum_weight);
                }
            }
        }
        
        if (_cfg.use_lead_lag) {
            auto ll_it = _sources.find(SignalType::LEAD_LAG);
            if (ll_it != _sources.end() && ll_it->second) {
                const auto& result = ll_it->second->result();
                if (result.valid) {
                    double alpha = ll_it->second->getAlphaValue();
                    alpha_sum += _cfg.lead_lag_weight * alpha;
                    weight_sum += _cfg.lead_lag_weight;
                    _valid_signals.push_back(alpha);
                    _valid_weights.push_back(_cfg.lead_lag_weight);
                }
            }
        }
        
// Fallback Mechanism: If no primary signals are valid, try falling back to just book imbalance
    // 修复：使用 EWMA 衰减而非直接跳转，避免 alpha 值瞬间跳变导致报价震荡
    if (weight_sum <= 0.0 && _ctx.book_imbalance.valid) {
        double prev_alpha = _prev_alpha;  // 上一次的 alpha 值
        double target = _ctx.book_imbalance.simple_imbalance;
        double ewma_decay = 0.3;  // 衰减因子，越小越平滑
        
        alpha_sum = prev_alpha * (1.0 - ewma_decay) + target * ewma_decay;
        weight_sum = 1.0;
        _valid_signals.push_back(alpha_sum);
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
        
        // IC 验证: 输出各信号源独立值 + 最终 alpha (debug 级)
        // 用于离线 IC/IR 分析各信号在 EC 的预测力
        WTSLogger::debug("[SIGNAL_DECOMP] {} mid={:.2f} | "
            "ofi={:.4f} trade={:.4f} book={:.4f} mom={:.4f} ll={:.4f} | "
            "alpha={:.4f} conf={:.4f} valid={}",
            _ctx.code, _ctx.mid_price,
            _ctx.alpha.ofi_component,
            _ctx.alpha.trade_component,
            _ctx.alpha.book_imbalance_component,
            _ctx.alpha.momentum_component,
            _ctx.alpha.lead_lag_component,
            _ctx.alpha.alpha, _ctx.alpha.confidence,
            _ctx.alpha.valid ? 1 : 0);
        
        // 保存当前 alpha 用于下次 EWMA 衰减
        if (_ctx.alpha.valid) {
            _prev_alpha = _ctx.alpha.alpha;
        }
    }

    SignalAggregatorConfig _cfg;
    SignalContext _ctx;
    std::unordered_map<SignalType, std::unique_ptr<ISignalSource>> _sources;
    VolatilitySignalSource* _vol_source = nullptr;
    
    // Pre-allocated vectors for zero-allocation hotpath
    std::vector<double> _valid_signals;
    std::vector<double> _valid_weights;
    
    // 上一次的 alpha 值，用于 EWMA 衰减（Alpha 跳跃修复）
    double _prev_alpha = 0.0;
};

} // namespace futu
