/*!
 * \file ICWeightTracker.h
 * \brief Adaptive signal weight framework — three-layer model
 *
 * Layer 1: Base logic weight (static, by trading logic)
 * Layer 2: Market regime adjustment (vol/trend/liquidity)
 * Layer 3: Online confidence adjustment (rolling IC + consistency)
 *
 * Core principle: IC low ≠ signal invalid. Low IC may be parameter mismatch.
 * Weight has floor (0.05) and cap (0.50) — never zero, never dominate.
 *
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <deque>
#include <unordered_map>
#include "../WTSTools/WTSLogger.h"

namespace futu {

//==========================================================================
// Signal type for weight tracking
//==========================================================================
enum class WeightedSignalType : uint8_t {
    OFI,
    TRADE_FLOW,
    BOOK_IMBALANCE,
    MOMENTUM,
    LEAD_LAG,
    COUNT  // sentinel
};

//==========================================================================
// Market regime classification
//==========================================================================
struct MarketRegime {
    enum class Vol : uint8_t { LOW, NORMAL, HIGH, EXTREME };
    enum class Trend : uint8_t { TRENDING, RANGING, MIXED };
    enum class Liquidity : uint8_t { DEEP, NORMAL, THIN };

    Vol vol = Vol::NORMAL;
    Trend trend = Trend::MIXED;
    Liquidity liquidity = Liquidity::NORMAL;

    /// Detect regime from available data
    static MarketRegime detect(double vol_percentile, double short_ma, double long_ma,
                               double avg_depth) {
        MarketRegime r;

        // Volatility regime
        if (vol_percentile < 20) r.vol = Vol::LOW;
        else if (vol_percentile < 60) r.vol = Vol::NORMAL;
        else if (vol_percentile < 80) r.vol = Vol::HIGH;
        else r.vol = Vol::EXTREME;

        // Trend regime (MA ratio)
        if (long_ma > 0) {
            double ratio = std::abs(short_ma / long_ma - 1.0);
            r.trend = (ratio > 0.002) ? Trend::TRENDING : Trend::RANGING;
        }

        // Liquidity regime
        if (avg_depth > 50) r.liquidity = Liquidity::DEEP;
        else if (avg_depth > 10) r.liquidity = Liquidity::NORMAL;
        else r.liquidity = Liquidity::THIN;

        return r;
    }
};

//==========================================================================
// Rolling IC tracker for one signal
//==========================================================================
struct RollingIC {
    std::deque<double> signal_history;
    std::deque<double> return_history;
    uint32_t window;
    uint32_t horizon;
    std::deque<double> pending_signals;  // signals waiting for future return
    double current_ic = 0.0;
    double current_ir = 0.0;
    uint32_t update_counter = 0;

    explicit RollingIC(uint32_t w = 2000, uint32_t h = 5)
        : window(w), horizon(h) {}

    /// Record a signal value (return will be filled later when horizon expires)
    void recordSignal(double signal_val) {
        pending_signals.push_back(signal_val);

        // When we have enough pending signals, pop oldest and pair with current return
        // Return is filled externally via recordReturn
    }

    /// Record the return for the oldest pending signal
    void recordReturn(double future_return) {
        if (pending_signals.empty()) return;

        double sig = pending_signals.front();
        pending_signals.pop_front();

        if (std::abs(sig) < 1e-6) return;  // skip zero signals

        signal_history.push_back(sig);
        return_history.push_back(future_return);

        if (signal_history.size() > window) {
            signal_history.pop_front();
            return_history.pop_front();
        }
    }

    /// Recalculate IC (call periodically, not every tick)
    void update() {
        if (signal_history.size() < 50) return;

        // Compute IC over full window
        double ic = computeCorr();

        // Compute IR: split window into segments, compute IC per segment
        uint32_t num_segments = 10;
        uint32_t seg_size = signal_history.size() / num_segments;
        if (seg_size < 10) {
            current_ic = ic;
            current_ir = 0.0;
            return;
        }

        std::vector<double> segment_ics;
        for (uint32_t s = 0; s < num_segments; s++) {
            uint32_t start = s * seg_size;
            uint32_t end = (s == num_segments - 1) ? signal_history.size() : start + seg_size;

            double seg_ic = computeCorrRange(start, end);
            segment_ics.push_back(seg_ic);
        }

        double mean_ic = 0;
        for (double v : segment_ics) mean_ic += v;
        mean_ic /= segment_ics.size();

        double std_ic = 0;
        for (double v : segment_ics) std_ic += (v - mean_ic) * (v - mean_ic);
        std_ic = std::sqrt(std_ic / segment_ics.size());

        current_ic = ic;
        current_ir = (std_ic > 1e-6) ? mean_ic / std_ic : 0.0;
    }

private:
    double computeCorr() const {
        return computeCorrRange(0, signal_history.size());
    }

    double computeCorrRange(size_t start, size_t end) const {
        if (end - start < 10) return 0;

        double ma = 0, mb = 0;
        size_t n = end - start;
        for (size_t i = start; i < end; i++) {
            ma += signal_history[i];
            mb += return_history[i];
        }
        ma /= n; mb /= n;

        double cov = 0, va = 0, vb = 0;
        for (size_t i = start; i < end; i++) {
            double da = signal_history[i] - ma;
            double db = return_history[i] - mb;
            cov += da * db;
            va += da * da;
            vb += db * db;
        }
        cov /= n; va /= n; vb /= n;

        if (va < 1e-12 || vb < 1e-12) return 0;
        return cov / (std::sqrt(va) * std::sqrt(vb));
    }
};

//==========================================================================
// Rolling scale tracker — normalize signal amplitudes to comparable range
// Uses rolling p95(|signal|) as scale factor, making all signals contribute
// proportionally to their weight, not their raw amplitude.
//==========================================================================
class RollingScaleTracker {
    std::deque<double> _abs_history;
    std::vector<double> _sorted_cache;  // for percentile computation
    uint32_t _window;
    uint32_t _update_interval;
    uint32_t _tick_count = 0;
    double _cached_scale = 1.0;
    double _min_scale;
    double _percentile;  // e.g. 0.95 for p95
    bool _cache_dirty = true;

public:
    explicit RollingScaleTracker(uint32_t window = 500, uint32_t update_interval = 20,
                                  double percentile = 0.95, double min_scale = 0.01)
        : _window(window), _update_interval(update_interval)
        , _min_scale(min_scale), _percentile(percentile) {}

    void record(double signal_val) {
        _abs_history.push_back(std::abs(signal_val));
        if (_abs_history.size() > _window) {
            _abs_history.pop_front();
        }
        _cache_dirty = true;
        _tick_count++;
    }

    /// Get current scale factor (updates periodically)
    double getScale() {
        if (_abs_history.size() < 20) return 1.0;  // warmup: no scaling

        if (!_cache_dirty && (_tick_count % _update_interval != 0)) {
            return _cached_scale;
        }

        // Compute percentile of absolute values
        _sorted_cache.assign(_abs_history.begin(), _abs_history.end());
        std::sort(_sorted_cache.begin(), _sorted_cache.end());

        size_t idx = static_cast<size_t>(_percentile * _sorted_cache.size());
        if (idx >= _sorted_cache.size()) idx = _sorted_cache.size() - 1;

        double scale = _sorted_cache[idx];
        _cached_scale = std::max(scale, _min_scale);
        _cache_dirty = false;
        return _cached_scale;
    }

    /// Normalize a signal value to [-1, 1] using current scale
    double normalize(double signal_val) {
        double scale = getScale();
        return std::clamp(signal_val / scale, -1.0, 1.0);
    }

    void reset() {
        _abs_history.clear();
        _sorted_cache.clear();
        _cached_scale = 1.0;
        _tick_count = 0;
        _cache_dirty = true;
    }
};

//==========================================================================
// Adaptive weight framework
//==========================================================================
class AdaptiveWeightFramework {
public:
    struct Config {
        // Layer 1: Base logic weights (by trading logic, not data fitting)
        // Book 权重提高: EC 窄 spread 做市中, 盘口不平衡是最直接的即时供需信号,
        // IC 实测最高(0.047), 理论上应高于 OFI(盘口薄时噪声大)
        double base_ofi;
        double base_trade;
        double base_book;
        double base_mom;
        double base_ll;

        // Weight bounds
        double weight_floor;
        double weight_cap;

        // IC tracking
        uint32_t ic_window;
        uint32_t ic_horizon;
        uint32_t ic_update_interval;

        // Regime factors (layer 2, trading-logic-driven)
        double ofi_thin_factor;
        double ofi_deep_factor;
        double trade_high_vol_factor;
        double trade_low_vol_factor;
        double mom_trending_factor;
        double mom_ranging_factor;
        double ll_cross_term_factor;
        double ll_single_factor;
        double book_normal_factor;

        // Layer 3: confidence mapping
        double ic_confidence_weight;
        double consistency_weight;

        Config()
            : base_ofi(0.25), base_trade(0.20), base_book(0.20), base_mom(0.15), base_ll(0.20)
            , weight_floor(0.05), weight_cap(0.50)
            , ic_window(2000), ic_horizon(5), ic_update_interval(50)
            , ofi_thin_factor(0.5), ofi_deep_factor(1.5)
            , trade_high_vol_factor(1.3), trade_low_vol_factor(0.7)
            , mom_trending_factor(1.5), mom_ranging_factor(0.5)
            , ll_cross_term_factor(1.5), ll_single_factor(0.3)
            , book_normal_factor(1.0)
            , ic_confidence_weight(0.5), consistency_weight(0.5)
        {}
    };

    explicit AdaptiveWeightFramework(const Config& cfg = Config()) : _cfg(cfg) {
        // Initialize IC trackers for each signal
        for (uint8_t i = 0; i < static_cast<uint8_t>(WeightedSignalType::COUNT); i++) {
            _ic_trackers[static_cast<WeightedSignalType>(i)] =
                RollingIC(cfg.ic_window, cfg.ic_horizon);
        }
    }

    /// Record signal value for IC tracking (call every tick)
    void recordSignal(WeightedSignalType type, double signal_val) {
        _ic_trackers[type].recordSignal(signal_val);
    }

    /// Record future return to complete IC pairing (call `horizon` ticks later)
    void recordReturn(double future_return) {
        for (auto& [type, tracker] : _ic_trackers) {
            tracker.recordReturn(future_return);
        }
    }

    /// Periodic IC update (call every ic_update_interval ticks)
    void updateIC() {
        for (auto& [type, tracker] : _ic_trackers) {
            tracker.update();
        }
    }

    /// Compute dynamic weights given current market regime and signal values
    /// \param regime Current market regime
    /// \param signal_values Current signal values [OFI, Trade, Book, Mom, LL]
    /// \param is_cross_term Whether this is a cross-term contract (affects LeadLag)
    /// \return Normalized weights [OFI, Trade, Book, Mom, LL] summing to 1.0
    std::unordered_map<WeightedSignalType, double> computeWeights(
        const MarketRegime& regime,
        const double signal_values[5],
        bool is_cross_term
    ) {
        struct SignalEntry {
            WeightedSignalType type;
            double base_weight;
            double signal_val;
        };

        SignalEntry entries[5] = {
            {WeightedSignalType::OFI,           _cfg.base_ofi,   signal_values[0]},
            {WeightedSignalType::TRADE_FLOW,    _cfg.base_trade, signal_values[1]},
            {WeightedSignalType::BOOK_IMBALANCE,_cfg.base_book,  signal_values[2]},
            {WeightedSignalType::MOMENTUM,      _cfg.base_mom,   signal_values[3]},
            {WeightedSignalType::LEAD_LAG,      _cfg.base_ll,    signal_values[4]},
        };

        // Compute consistency (how many signals agree in direction)
        double weighted_direction = 0;
        double total_w = 0;
        for (int i = 0; i < 5; i++) {
            double s = entries[i].signal_val;
            if (std::abs(s) > 0.01) {
                double sign = (s > 0) ? 1.0 : -1.0;
                weighted_direction += sign * entries[i].base_weight;
                total_w += entries[i].base_weight;
            }
        }
        double consistency = (total_w > 0) ? std::abs(weighted_direction / total_w) : 0.5;

        // Compute final weight for each signal
        std::unordered_map<WeightedSignalType, double> weights;
        double raw_sum = 0;

        for (int i = 0; i < 5; i++) {
            auto type = entries[i].type;
            double w_base = entries[i].base_weight;

            // Layer 2: Market regime adjustment
            double regime_factor = getRegimeFactor(type, regime, is_cross_term);

            // Layer 3: Online confidence adjustment
            double ir = _ic_trackers[type].current_ir;
            // IR → confidence factor: IR>1 → 2.0, IR=0 → 1.0, IR<-1 → 0.3
            // tanh mapping with floor
            double ic_factor = 0.3 + 1.7 * (0.5 + 0.5 * std::tanh(ir * 2.0));

            // Consistency boost: if this signal agrees with majority, boost
            double signal_sign = (entries[i].signal_val > 0) ? 1.0 : -1.0;
            double majority_sign = (weighted_direction > 0) ? 1.0 : -1.0;
            double consistency_boost = (signal_sign == majority_sign && std::abs(entries[i].signal_val) > 0.01)
                                       ? (0.8 + 0.4 * consistency)  // [0.8, 1.2]
                                       : (1.2 - 0.4 * consistency); // [0.8, 1.2]

            // Combine layers
            double w = w_base * regime_factor * ic_factor * consistency_boost;

            // Apply floor and cap
            w = std::max(_cfg.weight_floor, std::min(_cfg.weight_cap, w));

            weights[type] = w;
            raw_sum += w;
        }

        // Normalize to sum=1
        if (raw_sum > 0) {
            for (auto& [type, w] : weights) {
                w /= raw_sum;
            }
        }

        return weights;
    }

    /// Get current IC for a signal (for logging/debugging)
    double getIC(WeightedSignalType type) const {
        auto it = _ic_trackers.find(type);
        return (it != _ic_trackers.end()) ? it->second.current_ic : 0;
    }

    double getIR(WeightedSignalType type) const {
        auto it = _ic_trackers.find(type);
        return (it != _ic_trackers.end()) ? it->second.current_ir : 0;
    }

    const Config& getConfig() const { return _cfg; }
    void reset() {
        for (auto& [type, tracker] : _ic_trackers) {
            tracker = RollingIC(_cfg.ic_window, _cfg.ic_horizon);
        }
    }

private:
    Config _cfg;
    std::unordered_map<WeightedSignalType, RollingIC> _ic_trackers;

    double getRegimeFactor(WeightedSignalType type, const MarketRegime& regime, bool is_cross_term) const {
        switch (type) {
            case WeightedSignalType::OFI:
                if (regime.liquidity == MarketRegime::Liquidity::THIN) return _cfg.ofi_thin_factor;
                if (regime.liquidity == MarketRegime::Liquidity::DEEP) return _cfg.ofi_deep_factor;
                return 1.0;

            case WeightedSignalType::TRADE_FLOW:
                if (regime.vol == MarketRegime::Vol::HIGH || regime.vol == MarketRegime::Vol::EXTREME)
                    return _cfg.trade_high_vol_factor;
                if (regime.vol == MarketRegime::Vol::LOW) return _cfg.trade_low_vol_factor;
                return 1.0;

            case WeightedSignalType::BOOK_IMBALANCE:
                // Book 在深流动性时更可靠(挂单真实), 薄流动性时易被撤单干扰
                if (regime.liquidity == MarketRegime::Liquidity::DEEP) return 1.3;
                if (regime.liquidity == MarketRegime::Liquidity::THIN) return 0.7;
                return _cfg.book_normal_factor;

            case WeightedSignalType::MOMENTUM:
                if (regime.trend == MarketRegime::Trend::TRENDING) return _cfg.mom_trending_factor;
                if (regime.trend == MarketRegime::Trend::RANGING) return _cfg.mom_ranging_factor;
                return 1.0;

            case WeightedSignalType::LEAD_LAG:
                return is_cross_term ? _cfg.ll_cross_term_factor : _cfg.ll_single_factor;

            default:
                return 1.0;
        }
    }
};

} // namespace futu
