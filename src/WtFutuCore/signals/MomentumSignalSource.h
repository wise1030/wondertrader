/*!
 * \file MomentumSignalSource.h
 * \brief Momentum Signal Source
 *
 * Calculates price momentum from recent price history:
 *   - Simple momentum: (recent_avg - earlier_avg) / earlier_avg
 *   - EMA momentum: EMA-based trend detection
 *
 * Momentum is useful for:
 *   - Trend detection
 *   - Market state classification
 *   - Alpha signal enhancement
 */
#pragma once

#include "ISignalSource.h"
#include "MarketDataContext.h"
#include "../../Share/RingBuffer.hpp"
#include <cmath>
#include <algorithm>

namespace futu
{

//==============================================================================
// Momentum Signal Source
//==============================================================================

class MomentumSignalSource : public ISignalSource
{
public:
    struct Config
    {
        uint32_t window;  ///< Price history window
        double ema_alpha; ///< EMA smoothing factor

        Config() : window(50), ema_alpha(0.1) {}
    };

    explicit MomentumSignalSource(const Config& cfg = Config())
        : _cfg(cfg), _enabled(true), _last_mid(0), _ema_momentum(0)
    {}

    //==========================================================================
    // ISignalSource Interface
    //==========================================================================

    const std::string& name() const override
    {
        static std::string n = "Momentum";
        return n;
    }

    SignalType type() const override { return SignalType::MOMENTUM; }

    void update(const MarketDataContext& book) override
    {
        double mid = book.getMidPrice();
        uint64_t ts = book.getTimestamp();

        if (mid <= 0) {
            _result.valid = false;
            return;
        }

        // 增量对数收益 + 滚动和: O(1)/tick
        // 旧代码每 tick 对全部历史(≤127)重算 std::log, 相同相邻对反复计算.
        // 对数收益率 log(P_t/P_{t-1}) 消除品种价格差异, 数学性质:
        //   - 可加性: log(P3/P1) = log(P3/P2) + log(P2/P1)
        //   - 对称性: 涨跌相同幅度, 对数收益率绝对值相同
        if (_last_mid > 0) {
            double lr = std::log(mid / _last_mid);
            if (_log_returns.full())
                _log_return_sum -= _log_returns.front();
            _log_returns.push(lr);
            _log_return_sum += lr;
        }
        _last_mid = mid;

        size_t n = _log_returns.size();
        if (n >= 9) // 收益数 = 价格数-1, 等价于原 _price_history.size() >= 10
        {
            calculateMomentum();
            _result.timestamp = ts;
            _result.valid = true; // valid=true移到if内部，样本不足时不标记valid
        } else {
            _result.valid = false; // 样本不足，不纳入加权计算
        }
    }

    const SignalResult& result() const override { return _result; }

    double getAlphaValue() const override { return _result.alpha; }

    bool enabled() const override { return _enabled; }
    void setEnabled(bool e) override { _enabled = e; }

    void reset() override
    {
        _log_returns.clear();
        _log_return_sum = 0;
        _last_mid = 0;
        _ema_momentum = 0;
        _result = AlphaSignalResult();
    }

    /// Set configuration
    void setConfig(const Config& cfg) { _cfg = cfg; }

    //==========================================================================
    // Additional Accessors
    //==========================================================================

    /// Get momentum value [-1, 1]
    double getMomentum() const { return _result.alpha; }

    /// Get EMA momentum
    double getEMAMomentum() const { return _ema_momentum; }

private:
    Config _cfg;
    bool _enabled;
    AlphaSignalResult _result;

    RingBuffer<double, 128> _log_returns; ///< 对数收益环形缓冲
    double _log_return_sum = 0;           ///< 滚动和(满环时扣减最旧)
    double _last_mid;
    double _ema_momentum;

    void calculateMomentum()
    {
        size_t n = _log_returns.size();
        if (n == 0)
            return;

        // V8-S6: window 配置此前被静默忽略 (恒用全缓冲 128)。现按最近
        // min(window, 128) 个收益取均值; O(w<=128) 直算, 开销可忽略。
        const size_t w = std::min<size_t>(_cfg.window > 0 ? _cfg.window : 128, n);
        double window_sum = 0;
        for (size_t i = n - w; i < n; ++i)
            window_sum += _log_returns[i];

        // 均值乘以1000作为缩放因子(对数收益率通常很小, 如0.0001级别)
        double raw_momentum = window_sum / static_cast<double>(w) * 1000.0;

        // Scale and clamp to [-1, 1]
        double momentum = std::tanh(raw_momentum);

        _result.alpha = momentum;

        // Update EMA momentum
        _ema_momentum = _cfg.ema_alpha * momentum + (1 - _cfg.ema_alpha) * _ema_momentum;

        _result.confidence = (n >= 19) ? 1.0 : static_cast<double>(n + 1) / 20.0;
    }
};

} // namespace futu
