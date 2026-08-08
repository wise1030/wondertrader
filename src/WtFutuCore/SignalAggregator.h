/*!
 * \file SignalAggregator.h
 * \brief Aggregates multiple signal sources into a unified SignalContext
 */
#pragma once

#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <array>
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
#include "ICWeightTracker.h"
#include "../WTSTools/WTSLogger.h"

namespace futu
{

struct SignalAggregatorConfig
{
    // 信号源开关 (由 fromVariant 根据 signals.* presence 自动设置)
    bool use_volatility = true; // 辅助信号, 始终启用
    bool use_ofi = false;
    bool use_trade_flow = false;
    bool use_book_imbalance = false;
    bool use_momentum = false;
    bool use_lead_lag = false;

    // 配置有效性标志 (model.type 校验失败时置 false)
    bool valid = true;

    // 信号源参数
    uint32_t volatility_window = 100;
    uint32_t ofi_window = 50;
    uint32_t trade_flow_window = 100;
    uint32_t momentum_window = 50;
    uint32_t lead_lag_window = 50;

    // 阈值参数
    double vol_elevated = 0.002; // should_widen + vol_tier -> ELEVATED (统一阈值)
    double vol_extreme = 0.004;  // vol_tier -> EXTREME (should_pause)

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

    static SignalAggregatorConfig fromVariant(wtp::WTSVariant* v)
    {
        SignalAggregatorConfig c;
        // 默认所有 alpha 信号禁用, volatility 始终启用 (已在构造函数中设置)

        //------------------------------------------------------------
        // 1. 解析 signals.* (数据层): presence = enabled
        //------------------------------------------------------------
        wtp::WTSVariant* signals = v->get("signals");
        if (signals) {
            // OFI
            wtp::WTSVariant* ofi = signals->get("ofi");
            if (ofi) {
                c.use_ofi = true;
                c.ofi_window = FutuConfig::readUInt32(ofi, "window", 50);
            }
            // TradeFlow
            wtp::WTSVariant* tf = signals->get("trade_flow");
            if (tf) {
                c.use_trade_flow = true;
                c.trade_flow_window = FutuConfig::readUInt32(tf, "window", 100);
                c.large_trade_threshold = FutuConfig::readDouble(tf, "largeTradeThreshold", 50.0);
            }
            // BookImbalance
            wtp::WTSVariant* bi = signals->get("book_imbalance");
            if (bi) {
                c.use_book_imbalance = true;
                c.book_imbalance_threshold = FutuConfig::readDouble(bi, "threshold", 0.2);
            }
            // Momentum
            wtp::WTSVariant* mom = signals->get("momentum");
            if (mom) {
                c.use_momentum = true;
                c.momentum_window = FutuConfig::readUInt32(mom, "window", 50);
                c.momentum_ema_alpha = FutuConfig::readDouble(mom, "emaAlpha", 0.1);
            }
            // LeadLag
            wtp::WTSVariant* ll = signals->get("lead_lag");
            if (ll) {
                c.use_lead_lag = true;
                c.lead_lag_window = FutuConfig::readUInt32(ll, "window", 50);
                c.lead_lag_lag_ms = FutuConfig::readUInt32(ll, "lagMs", 50);
            }
        }

        //------------------------------------------------------------
        // 2. 解析 model.* (模型层): type 校验 + 权重
        //------------------------------------------------------------
        wtp::WTSVariant* model = v->get("model");
        if (model) {
            // type 严格校验
            std::string mtype = FutuConfig::readString(model, "type", "linear");
            if (mtype != "linear") {
                WTSLogger::error("SignalAggregator: unsupported model type: {}, only 'linear' is supported", mtype);
                c.valid = false;
                return c;
            }
            // 权重
            wtp::WTSVariant* weights = model->get("weights");
            if (weights) {
                c.ofi_weight = FutuConfig::readDouble(weights, "ofi", 0.35);
                c.trade_weight = FutuConfig::readDouble(weights, "trade_flow", 0.25);
                c.book_imbalance_weight = FutuConfig::readDouble(weights, "book_imbalance", 0.20);
                c.momentum_weight = FutuConfig::readDouble(weights, "momentum", 0.15);
                c.lead_lag_weight = FutuConfig::readDouble(weights, "lead_lag", 0.05);
            } else {
                WTSLogger::warn("SignalAggregator: model.weights not configured, using default weights");
            }
            c.strong_threshold = FutuConfig::readDouble(model, "strongThreshold", 0.7);
        } else {
            WTSLogger::warn("SignalAggregator: model section missing, using default linear weights");
        }

        //------------------------------------------------------------
        // 3. 交叉校验: weight 配了但 signal 未启用 -> warn
        //------------------------------------------------------------
        if (!c.use_ofi && c.ofi_weight > 0.0)
            WTSLogger::warn("SignalAggregator: orphan weight for ofi (signal not configured)");
        if (!c.use_trade_flow && c.trade_weight > 0.0)
            WTSLogger::warn("SignalAggregator: orphan weight for trade_flow (signal not configured)");
        if (!c.use_book_imbalance && c.book_imbalance_weight > 0.0)
            WTSLogger::warn("SignalAggregator: orphan weight for book_imbalance (signal not configured)");
        if (!c.use_momentum && c.momentum_weight > 0.0)
            WTSLogger::warn("SignalAggregator: orphan weight for momentum (signal not configured)");
        if (!c.use_lead_lag && c.lead_lag_weight > 0.0)
            WTSLogger::warn("SignalAggregator: orphan weight for lead_lag (signal not configured)");

        //------------------------------------------------------------
        // 4. 解析 volatility (辅助信号, 始终启用, 缺省=默认值)
        //------------------------------------------------------------
        wtp::WTSVariant* vol = v->get("volatility");
        if (vol) {
            c.volatility_window = FutuConfig::readUInt32(vol, "window", 100);
            c.vol_elevated = FutuConfig::readDouble(vol, "elevatedThreshold", 0.002);
            c.vol_extreme = FutuConfig::readDouble(vol, "extremeThreshold", 0.004);
        }

        //------------------------------------------------------------
        // 5. 公共参数
        //------------------------------------------------------------
        c.warmup_ticks = FutuConfig::readUInt32(v, "warmupTicks", 50);

        return c;
    }
};

class SignalAggregator
{
public:
    SignalAggregator() = default;

    // 支持配置的构造函数，匹配 make_unique(sig_cfg)
    explicit SignalAggregator(const SignalAggregatorConfig& cfg) { setConfig(cfg); }

    ~SignalAggregator() = default;

    void setConfig(const SignalAggregatorConfig& cfg)
    {
        _cfg = cfg;
        _warmup_ticks = cfg.warmup_ticks;
        initializeSignalSources();
        // Initialize adaptive weight framework
        AdaptiveWeightFramework::Config wcfg;
        wcfg.base_ofi = cfg.ofi_weight;
        wcfg.base_trade = cfg.trade_weight;
        wcfg.base_book = cfg.book_imbalance_weight;
        wcfg.base_mom = cfg.momentum_weight;
        wcfg.base_ll = cfg.lead_lag_weight;
        _weight_framework = std::make_unique<AdaptiveWeightFramework>(wcfg);
    }

    void updateWeights(const SignalAggregatorConfig& cfg)
    {
        _cfg.ofi_weight = cfg.ofi_weight;
        _cfg.trade_weight = cfg.trade_weight;
        _cfg.book_imbalance_weight = cfg.book_imbalance_weight;
        _cfg.momentum_weight = cfg.momentum_weight;
        _cfg.lead_lag_weight = cfg.lead_lag_weight;
        _cfg.strong_threshold = cfg.strong_threshold;
        // 穿透到权重框架的 Layer1 base 权重 — 否则热更新对实际 alpha 计算无效
        if (_weight_framework) {
            _weight_framework->updateBaseWeights(
                cfg.ofi_weight, cfg.trade_weight, cfg.book_imbalance_weight, cfg.momentum_weight, cfg.lead_lag_weight);
        }
    }

    const SignalAggregatorConfig& getConfig() const { return _cfg; }

    const SignalContext& update(const MarketDataContext& book)
    {
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
        if (ll_it != _sources.end() && ll_it->second) {
            auto* ll = dynamic_cast<LeadLagSignalSource*>(ll_it->second.get());
            if (ll) {
                ll->updateLeadContract(code, mid, timestamp);
            }
        }
    }

    /// Add a lead contract to the LeadLag signal source.
    /// Must be called before ticks arrive (during initialization).
    void addLeadContract(const std::string& code, double correlation = 1.0)
    {
        auto ll_it = _sources.find(SignalType::LEAD_LAG);
        if (ll_it != _sources.end() && ll_it->second) {
            auto* ll = dynamic_cast<LeadLagSignalSource*>(ll_it->second.get());
            if (ll) {
                ll->addLeadContract(code, correlation);
            }
        }
    }

    bool is_ready() const { return _tick_count >= _warmup_ticks; }

    void reset()
    {
        _tick_count = 0;
        _ctx.reset();
        for (auto& pair : _sources)
            pair.second->reset();
        _prev_alpha = 0.0;
        _tick_counter = 0;
        _mid_history_for_ic.clear();
        _mid_ma_short.clear();
        _mid_ma_long.clear();
        _ma_short_sum = 0.0;
        _ma_long_sum = 0.0;
        _dynamic_weights.fill(0.0);
        _weights_valid = false;
        _scale_trackers_initialized = false;
    }

private:
    uint32_t _tick_count = 0;
    uint32_t _warmup_ticks = 50; // default warm-up period

    void initializeSignalSources()
    {
        _sources.clear();
        _signal_slots.clear();

        // Volatility signal source (always enabled, 辅助信号不参与加权)
        if (_cfg.use_volatility) {
            auto vol = std::make_unique<RealizedVolSignalSource>();
            vol->setWindowSize(_cfg.volatility_window);
            vol->setVolThresholds(_cfg.vol_elevated, _cfg.vol_extreme);
            _vol_source = vol.get();
            _sources[SignalType::VOLATILITY] = std::move(vol);
        }

        // OFI signal source
        if (_cfg.use_ofi) {
            OFISignalSource::Config ofi_cfg;
            ofi_cfg.window = _cfg.ofi_window;
            auto src = std::make_unique<OFISignalSource>(ofi_cfg);
            registerSlot(
                SignalType::OFI,
                WeightedSignalType::OFI,
                src.get(),
                &SignalAggregatorConfig::ofi_weight,
                true,
                [](const SignalContext& ctx, const ISignalSource*, bool& ok) {
                    ok = ctx.ofi.valid;
                    return ok ? ctx.ofi.ofi : 0.0;
                },
                [](SignalContext& ctx, double v) { ctx.alpha.ofi_component = v; });
            _sources[SignalType::OFI] = std::move(src);
        }

        // Trade flow signal source
        if (_cfg.use_trade_flow) {
            TradeFlowSignalSource::Config flow_cfg;
            flow_cfg.window = _cfg.trade_flow_window;
            flow_cfg.large_trade_threshold = _cfg.large_trade_threshold;
            auto src = std::make_unique<TradeFlowSignalSource>(flow_cfg);
            registerSlot(
                SignalType::TRADE_FLOW,
                WeightedSignalType::TRADE_FLOW,
                src.get(),
                &SignalAggregatorConfig::trade_weight,
                true,
                [](const SignalContext& ctx, const ISignalSource*, bool& ok) {
                    ok = ctx.trade_flow.valid;
                    return ok ? ctx.trade_flow.net_flow_normalized : 0.0;
                },
                [](SignalContext& ctx, double v) { ctx.alpha.trade_component = v; });
            _sources[SignalType::TRADE_FLOW] = std::move(src);
        }

        // Book imbalance signal source
        if (_cfg.use_book_imbalance) {
            BookImbalanceSignalSource::Config imb_cfg;
            imb_cfg.dominant_threshold = _cfg.book_imbalance_threshold;
            auto src = std::make_unique<BookImbalanceSignalSource>(imb_cfg);
            registerSlot(
                SignalType::BOOK_IMBALANCE,
                WeightedSignalType::BOOK_IMBALANCE,
                src.get(),
                &SignalAggregatorConfig::book_imbalance_weight,
                true,
                [](const SignalContext& ctx, const ISignalSource*, bool& ok) {
                    ok = ctx.book_imbalance.valid;
                    return ok ? ctx.book_imbalance.simple_imbalance : 0.0;
                },
                [](SignalContext& ctx, double v) { ctx.alpha.book_imbalance_component = v; });
            _sources[SignalType::BOOK_IMBALANCE] = std::move(src);
        }

        // Momentum signal source
        if (_cfg.use_momentum) {
            MomentumSignalSource::Config mom_cfg;
            mom_cfg.window = _cfg.momentum_window;
            mom_cfg.ema_alpha = _cfg.momentum_ema_alpha;
            auto src = std::make_unique<MomentumSignalSource>(mom_cfg);
            registerSlot(
                SignalType::MOMENTUM,
                WeightedSignalType::MOMENTUM,
                src.get(),
                &SignalAggregatorConfig::momentum_weight,
                true,
                [](const SignalContext&, const ISignalSource* s, bool& ok) {
                    ok = s->result().valid;
                    return ok ? s->getAlphaValue() : 0.0;
                },
                [](SignalContext& ctx, double v) { ctx.alpha.momentum_component = v; });
            _sources[SignalType::MOMENTUM] = std::move(src);
        }

        if (_cfg.use_lead_lag) {
            LeadLagSignalSource::Config ll_cfg;
            ll_cfg.window = _cfg.lead_lag_window;
            ll_cfg.lag_ms = _cfg.lead_lag_lag_ms;
            auto src = std::make_unique<LeadLagSignalSource>(ll_cfg);
            // LL 不做幅度归一化 — LL 信号特性与其他信号不同:
            // 大部分 tick 是重复值(anchor tick 频率低), p95 归一化会爆炸.
            // LL 的 IC 虽然最高(0.09)但绝对值仍小, 放大幅度=放大噪声.
            registerSlot(
                SignalType::LEAD_LAG,
                WeightedSignalType::LEAD_LAG,
                src.get(),
                &SignalAggregatorConfig::lead_lag_weight,
                false,
                [](const SignalContext&, const ISignalSource* s, bool& ok) {
                    ok = s->result().valid;
                    return ok ? s->getAlphaValue() : 0.0;
                },
                [](SignalContext& ctx, double v) { ctx.alpha.lead_lag_component = v; });
            _sources[SignalType::LEAD_LAG] = std::move(src);
        }
    }

    /// 注册一个加权信号槽位 — 新增信号源只需在 initializeSignalSources 中
    /// 加一段注册, computeAlpha/IC 记录/归一化/权重全部自动生效
    void registerSlot(SignalType type,
                      WeightedSignalType wtype,
                      ISignalSource* source,
                      double SignalAggregatorConfig::*weight_member,
                      bool normalize,
                      std::function<double(const SignalContext&, const ISignalSource*, bool&)> extract,
                      std::function<void(SignalContext&, double)> set_component)
    {
        SignalSlot slot;
        slot.type = type;
        slot.wtype = wtype;
        slot.source = source;
        slot.weight_member = weight_member;
        slot.normalize = normalize;
        slot.extract = std::move(extract);
        slot.set_component = std::move(set_component);
        _signal_slots.push_back(std::move(slot));
    }

    void computeMarketState()
    {
        // 修正字段名：volatility -> realized_vol
        _ctx.market_state.vol_estimate = _ctx.volatility.realized_vol;
        _ctx.market_state.should_widen = (_ctx.volatility.realized_vol > _cfg.vol_elevated);
        // should_pause每tick重算，不复位锁存
        // 原代码只在vol_tier==EXTREME时设true，无else分支复位false
        // 导致should_pause一旦被设就永久锁死，报价永远被阻止
        _ctx.market_state.should_pause = (_ctx.volatility.vol_tier == VolTier::EXTREME);
    }

    /// Extract signal results from signal sources
    void extractSignalResults()
    {
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
    void computeAlpha()
    {
        // Reset alpha
        _ctx.alpha = AlphaSignalResult();

        double alpha_sum = 0.0;
        double weight_sum = 0.0;

        // 收集有效信号用于计算一致性 (使用成员变量避免热路径内存分配)
        _valid_signals.clear();
        _valid_weights.clear();

        //==================================================================
        // Adaptive Weight Framework (三层权重)
        // 在固定权重逻辑前计算动态权重, 替代后续的 _cfg.xxx_weight
        //==================================================================
        // 提取各信号当前值 (槽位驱动, 替代 5 段硬编码提取)
        double slot_vals[5] = {0.0, 0.0, 0.0, 0.0, 0.0}; // 按 WeightedSignalType 索引
        bool slot_ok[5] = {false, false, false, false, false};
        for (auto& slot : _signal_slots) {
            bool ok = false;
            double v = slot.extract(_ctx, slot.source, ok);
            size_t idx = static_cast<size_t>(slot.wtype);
            slot_vals[idx] = v;
            slot_ok[idx] = ok;
        }

        // 记录信号值用于 IC 跟踪
        _tick_counter++;
        if (_weight_framework) {
            for (auto& slot : _signal_slots) {
                _weight_framework->recordSignal(slot.wtype, slot_vals[static_cast<size_t>(slot.wtype)]);
            }

            // 记录 horizon-tick 前的信号对应的未来回报
            // 口径: future_return = mid[t] - mid[t-horizon]
            // 旧代码用 1-tick 收益(mid[t]-mid[t-1])冒充 horizon 收益, IC 度量失真.
            const uint32_t ic_horizon = _weight_framework->getConfig().ic_horizon;
            _mid_history_for_ic.push_back(_ctx.mid_price);
            if (_mid_history_for_ic.size() > static_cast<size_t>(ic_horizon) + 1)
                _mid_history_for_ic.pop_front();
            if (_tick_counter > ic_horizon && _mid_history_for_ic.size() > ic_horizon) {
                double future_return = _ctx.mid_price - _mid_history_for_ic.front();
                _weight_framework->recordReturn(future_return);
            }

            // 定期更新 IC
            if (_tick_counter % _weight_framework->getConfig().ic_update_interval == 0) {
                _weight_framework->updateIC();
            }

            // 计算动态权重
            double signal_array[5] = {slot_vals[0], slot_vals[1], slot_vals[2], slot_vals[3], slot_vals[4]};
            // EC 跨期品种 → LeadLag regime factor 升权
            bool is_cross_term = (_sources.find(SignalType::LEAD_LAG) != _sources.end());
            // Regime trend 检测: 维护短/长窗口 mid 滚动均值 (O(1))
            // 旧代码 short_ma==long_ma=mid → ratio=0 → trend 恒 RANGING,
            // 动量权重被 mom_ranging_factor(0.5) 永久减半.
            _mid_ma_short.push_back(_ctx.mid_price);
            _ma_short_sum += _ctx.mid_price;
            if (_mid_ma_short.size() > MA_SHORT_WINDOW) {
                _ma_short_sum -= _mid_ma_short.front();
                _mid_ma_short.pop_front();
            }
            _mid_ma_long.push_back(_ctx.mid_price);
            _ma_long_sum += _ctx.mid_price;
            if (_mid_ma_long.size() > MA_LONG_WINDOW) {
                _ma_long_sum -= _mid_ma_long.front();
                _mid_ma_long.pop_front();
            }
            double short_ma = _ma_short_sum / _mid_ma_short.size();
            double long_ma = _ma_long_sum / _mid_ma_long.size();
            auto regime = MarketRegime::detect(
                _ctx.volatility.vol_percentile, short_ma, long_ma, (_ctx.bid_depth + _ctx.ask_depth) / 2.0);
            _dynamic_weights = _weight_framework->computeWeights(regime, signal_array, is_cross_term);
            _weights_valid = true;
        }

        //==================================================================
        // 加权求和 (槽位驱动, 替代 5 段硬编码分量)
        // 每个槽位: 提取 → 幅度归一化(LL 除外) → 动态权重 → 累加
        //==================================================================
        for (auto& slot : _signal_slots) {
            size_t idx = static_cast<size_t>(slot.wtype);
            if (!slot_ok[idx])
                continue;
            double raw = slot_vals[idx];
            double norm = slot.normalize ? normalizeSignal(slot.wtype, raw) : raw;
            slot.set_component(_ctx, norm);
            double w = getDynamicWeight(slot.wtype, _cfg.*(slot.weight_member));
            alpha_sum += w * norm;
            weight_sum += w;
            _valid_signals.push_back(norm);
            _valid_weights.push_back(w);
        }

        // Fallback Mechanism: If no primary signals are valid, try falling back to just book imbalance
        // 修复：使用 EWMA 衰减而非直接跳转，避免 alpha 值瞬间跳变导致报价震荡
        if (weight_sum <= 0.0 && _ctx.book_imbalance.valid) {
            double prev_alpha = _prev_alpha; // 上一次的 alpha 值
            double target = _ctx.book_imbalance.simple_imbalance;
            double ewma_decay = 0.3; // 衰减因子，越小越平滑

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

        // IC 验证: 输出各信号源归一化后的值 + 最终 alpha (debug 级)
        // 注意: ofi/trade/book/mom/ll 打的是归一化后的 _ctx.alpha.*_component
        // 这些值才是真正参与 alpha_sum 的值
        WTSLogger::debug("[SIGNAL_DECOMP] {} mid={:.2f} | "
                         "ofi={:.4f} trade={:.4f} book={:.4f} mom={:.4f} ll={:.4f} | "
                         "alpha={:.4f} conf={:.4f} valid={}",
                         _ctx.code,
                         _ctx.mid_price,
                         _ctx.alpha.ofi_component,
                         _ctx.alpha.trade_component,
                         _ctx.alpha.book_imbalance_component,
                         _ctx.alpha.momentum_component,
                         _ctx.alpha.lead_lag_component,
                         _ctx.alpha.alpha,
                         _ctx.alpha.confidence,
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

    //==========================================================================
    // 加权信号槽位 (表驱动, 替代 computeAlpha 中的 5 段硬编码分量)
    // 顺序固定: OFI / TradeFlow / BookImbalance / Momentum / LeadLag
    //==========================================================================
    struct SignalSlot
    {
        SignalType type;
        WeightedSignalType wtype;
        ISignalSource* source;                         // 缓存指针, 消除每 tick map find
        double SignalAggregatorConfig::*weight_member; // 静态回退权重(指向 _cfg 成员)
        bool normalize;                                // 是否做 p95 幅度归一化
        std::function<double(const SignalContext&, const ISignalSource*, bool&)> extract;
        std::function<void(SignalContext&, double)> set_component;
    };
    std::vector<SignalSlot> _signal_slots;

    // Pre-allocated vectors for zero-allocation hotpath
    std::vector<double> _valid_signals;
    std::vector<double> _valid_weights;

    // 上一次的 alpha 值，用于 EWMA 衰减（Alpha 跳跃修复）
    double _prev_alpha = 0.0;

    //==========================================================================
    // Adaptive Weight Framework members
    //==========================================================================
    std::unique_ptr<AdaptiveWeightFramework> _weight_framework;
    // perf#2/#6: array 替代 unordered_map, 消除每 tick 堆分配 + getDynamicWeight 的 hash find
    std::array<double, static_cast<size_t>(WeightedSignalType::COUNT)> _dynamic_weights{};
    bool _weights_valid = false;
    uint64_t _tick_counter = 0;
    // IC horizon 回报: mid[t-horizon] 历史 (长度 horizon+1)
    std::deque<double> _mid_history_for_ic;
    // Regime trend 检测: 短/长窗口 mid 均值 (O(1) 滚动和)
    std::deque<double> _mid_ma_short;
    std::deque<double> _mid_ma_long;
    double _ma_short_sum = 0.0;
    double _ma_long_sum = 0.0;
    static constexpr size_t MA_SHORT_WINDOW = 20;
    static constexpr size_t MA_LONG_WINDOW = 60;

    // Signal amplitude normalization (rolling p95 scale)
    // 确保 Mom/LL 等小幅信号不被 OFI/Trade 饱和信号淹没
    std::unordered_map<WeightedSignalType, RollingScaleTracker> _scale_trackers;
    bool _scale_trackers_initialized = false;

    void initScaleTrackers()
    {
        if (_scale_trackers_initialized)
            return;
        for (uint8_t i = 0; i < static_cast<uint8_t>(WeightedSignalType::COUNT); i++) {
            auto type = static_cast<WeightedSignalType>(i);
            _scale_trackers.emplace(type, RollingScaleTracker(500, 20, 0.95, 0.01));
        }
        _scale_trackers_initialized = true;
    }

    double normalizeSignal(WeightedSignalType type, double raw_value)
    {
        initScaleTrackers();
        auto it = _scale_trackers.find(type);
        if (it == _scale_trackers.end())
            return raw_value;
        it->second.record(raw_value);
        return it->second.normalize(raw_value);
    }

    /// Get dynamic weight (falls back to fixed weight if framework not active)
    double getDynamicWeight(WeightedSignalType type, double fallback) const
    {
        if (!_weight_framework || !_weights_valid)
            return fallback;
        return _dynamic_weights[static_cast<size_t>(type)];
    }
};

} // namespace futu
