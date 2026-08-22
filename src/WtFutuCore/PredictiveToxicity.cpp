/*!
 * \file PredictiveToxicity.cpp
 * \brief Predictive Toxicity Detection Implementation
 */

#include "PredictiveToxicity.h"
#include "../Includes/WTSDataDef.hpp"
#include <algorithm>
#include <cmath>

namespace futu
{

PredictiveToxicity::PredictiveToxicity() : _has_alpha_data(false), _cache_dirty(true) {}

//------------------------------------------------------------------------------
// Configuration
//------------------------------------------------------------------------------

void PredictiveToxicity::setBucketSize(double bucket_size)
{
    _bucket_size = bucket_size > 0 ? bucket_size : _cfg.vpin_bucket_size;
}

//------------------------------------------------------------------------------
// Data Input
//------------------------------------------------------------------------------

void PredictiveToxicity::updateAlpha(const AlphaResult& alpha, const TradeImbalanceResult& tradeImb)
{
    _latest_alpha = alpha;
    _latest_trade_imb = tradeImb;
    _has_alpha_data = true;
    _cache_dirty = true;
}

//------------------------------------------------------------------------------
// VPIN Analysis
//------------------------------------------------------------------------------

void PredictiveToxicity::onTrade(double price, double qty, bool isBuy, uint64_t timestamp)
{
    if (_bucket_size <= 0)
        return;

    if (isBuy) {
        _current_bucket.buy_volume += qty;
    } else {
        _current_bucket.sell_volume += qty;
    }
    _current_bucket.total_volume += qty;
    _current_bucket.end_time = timestamp;

    if (_current_bucket.start_time == 0) {
        _current_bucket.start_time = timestamp;
    }

    // Check if bucket is full
    if (_current_bucket.total_volume >= _bucket_size) {
        // V8-T3: 桶内 imbalance 按实际桶量归一到 [0,1] (经典 VPIN 口径)。
        // 原实现 push |buy-sell| 原始量、分母用 bucket_size -- 单边流时
        // imbalance 可超桶量 (如 1200/1000), VPIN 无界 (>1.0) 且系统性高估。
        // 关桶后余量保留在 _current_bucket (已天然带入下桶, 不丢弃)。
        double imbalance = std::abs(_current_bucket.buy_volume - _current_bucket.sell_volume);
        double normalized_imbalance =
            (_current_bucket.total_volume > 0) ? imbalance / _current_bucket.total_volume : 0.0;
        _order_imbalances.push_back(normalized_imbalance);

        // Maintain fixed window
        if (_order_imbalances.size() > _cfg.vpin_window) {
            _order_imbalances.pop_front();
        }

        // Calculate VPIN: window 内归一 imbalance 均值, 严格有界 [0,1]
        double sum_imbalance = 0;
        for (double imb : _order_imbalances) {
            sum_imbalance += imb;
        }

        if (!_order_imbalances.empty()) {
            _vpin = sum_imbalance / _order_imbalances.size();
        }

        // Save and reset bucket
        _buckets.push_back(_current_bucket);
        if (_buckets.size() > _cfg.vpin_window) {
            _buckets.pop_front();
        }

        _current_bucket = VolumeBucket();
        _current_bucket.start_time = timestamp;

        _cache_dirty = true;
    }
}

void PredictiveToxicity::onTickVolume(const char* stdCode, const wtp::WTSTickData* tick)
{
    if (!tick)
        return;

    if (_bucket_size <= 0) {
        setBucketSize(_cfg.vpin_bucket_size);
        if (_bucket_size <= 0)
            return;
    }

    double qty = tick->volume();
    if (qty <= 0)
        return;

    double price = tick->price();
    uint64_t timestamp = tick->actiontime();

    // Infer trade direction
    bool isBuy = true;
    auto it = _last_ticks.find(stdCode);
    if (it != _last_ticks.end()) {
        const LastTickInfo& last = it->second;
        double last_mid = (last.bid_px + last.ask_px) / 2.0;
        double current_mid = (tick->bidprice(0) + tick->askprice(0)) / 2.0;

        if (price >= tick->askprice(0)) {
            isBuy = true;
        } else if (price <= tick->bidprice(0)) {
            isBuy = false;
        } else if (current_mid > last_mid) {
            isBuy = true;
        } else if (current_mid < last_mid) {
            isBuy = false;
        } else {
            onTrade(price, qty / 2.0, true, timestamp);
            onTrade(price, qty / 2.0, false, timestamp);
            _last_ticks[stdCode] = {tick->bidprice(0), tick->askprice(0), tick->totalvolume()};
            return;
        }
    }

    onTrade(price, qty, isBuy, timestamp);
    _last_ticks[stdCode] = {tick->bidprice(0), tick->askprice(0), tick->totalvolume()};
}

//------------------------------------------------------------------------------
// Cache Update
//------------------------------------------------------------------------------

void PredictiveToxicity::updateCache() const
{
    if (!_cache_dirty)
        return;

    _cached_result = PredictiveToxicityResult();

    // Warmup gate
    // 冷启动 _buckets 累积不足时 VPIN 噪声极大(单 bucket 就能算出非零值),
    // 导致 is_toxic 抖动锁死策略。要求至少 min_warmup_buckets 个完整桶后才启用 VPIN。
    // 期间 vpin=0, 但 alpha 通道(OFI/Trade/extreme)仍然生效 — 旧代码在此提前 return,
    // 与下方注释矛盾, 预热期形成保护真空.
    const bool vpin_ready = (_buckets.size() >= _cfg.min_warmup_buckets);
    _cached_result.vpin_ready = vpin_ready;
    _cached_result.vpin = vpin_ready ? _vpin : 0.0;

    if (_has_alpha_data) {
        // OFI toxicity
        _cached_result.ofi_toxicity = std::abs(_latest_alpha.ofi_component);

        // Trade imbalance toxicity
        _cached_result.trade_toxicity =
            std::abs(_latest_trade_imb.imbalance_ratio) * (0.5 + 0.5 * _latest_trade_imb.large_trade_ratio);

        // Combined alpha toxicity
        // V8-T6: ofi/trade 权重由 ToxicFlowDetector::setParams 归一化 (和=1),
        // alpha_toxicity 严格有界 [0,1] (此前 0.3+0.3 上限 0.6, 尺度失真)
        _cached_result.alpha_toxicity =
            _cfg.ofi_weight * _cached_result.ofi_toxicity + _cfg.trade_weight * _cached_result.trade_toxicity;

        // Check for extreme signals (V8-R2: 阈值可配置, 默认 0.9 --
        // 旧硬编码 0.6 低于 OFI 归一器设计的常态映射区 (tanh 0.5-0.8),
        // 使 extreme 兜底成为常态触发路径)
        if (_cached_result.ofi_toxicity > _cfg.extreme_threshold
            || _cached_result.trade_toxicity > _cfg.extreme_threshold) {
            _cached_result.extreme_signal = std::max(_cached_result.ofi_toxicity, _cached_result.trade_toxicity);
        }
    }

    // Combined score: weighted combination of VPIN and alpha (not max)
    // V8-T6: vpin/alpha 通道权重可配置 (vpin_weight), 归一化 (和=1)
    double vpin_w = _cfg.vpin_weight;
    if (!(vpin_w >= 0.0 && vpin_w <= 1.0))
        vpin_w = 0.5;
    _cached_result.combined_score = vpin_w * _cached_result.vpin + (1.0 - vpin_w) * _cached_result.alpha_toxicity;

    // 注意: extreme_signal 不在此叠加 — 由 ToxicFlowDetector 门面在 realized 加权后
    // 统一叠加, 避免双重放大(本类内部叠加一次 + 门面再叠加一次).

    // Is toxic?
    _cached_result.is_toxic = _cached_result.combined_score > _cfg.alpha_threshold ||
                              (vpin_ready && _cached_result.vpin > _cfg.vpin_threshold);

    // Toxic side
    // V8-R2: side 归属与 is_toxic 解耦 -- 门面 extreme 兜底触发时 pred 可能
    // 未过阈 (combined < alpha_threshold), 旧判定导致这类事件 side 恒 0 ->
    // 双边抑制; ofi/imb 同号即给方向 (门面仅在自身 is_toxic 时消费)
    if (_has_alpha_data) {
        if (_latest_alpha.ofi_component > 0 && _latest_trade_imb.imbalance_ratio > 0)
            _cached_result.toxic_side = 1;
        else if (_latest_alpha.ofi_component < 0 && _latest_trade_imb.imbalance_ratio < 0)
            _cached_result.toxic_side = -1;
    }

    _cache_dirty = false;
}

//------------------------------------------------------------------------------
// Analysis
//------------------------------------------------------------------------------

PredictiveToxicityResult PredictiveToxicity::analyze() const
{
    updateCache();
    return _cached_result;
}

double PredictiveToxicity::getToxicityScore() const
{
    updateCache();
    return _cached_result.combined_score;
}

//------------------------------------------------------------------------------
// Reset
//------------------------------------------------------------------------------

void PredictiveToxicity::reset()
{
    _has_alpha_data = false;
    _cache_dirty = true;
    _cached_result = PredictiveToxicityResult();

    _buckets.clear();
    _order_imbalances.clear();
    _current_bucket = VolumeBucket();
    _vpin = 0.0;
    _last_ticks.clear();
}

} // namespace futu
