/*!
 * \file OFISignalSource.h
 * \brief Order Flow Imbalance Signal Source
 * 
 * Calculates OFI (Order Flow Imbalance) from order book data:
 *   - Standard OFI: cumulative order flow pressure
 *   - Normalized OFI: [-1, 1] range
 *   - Bid/Ask pressure: [0, 1] each
 * 
 * Uses data from MarketDataContext as single source.
 */
#pragma once

#include "ISignalSource.h"
#include "MarketDataContext.h"
#include "../Share/RingBuffer.hpp"
#include <cmath>
#include <algorithm>

namespace futu {

//==============================================================================
// OFI Signal Source
//==============================================================================

class OFISignalSource : public ISignalSource
{
public:
    struct Config
    {
        uint32_t window;           ///< OFI calculation window
        double normalization_factor;    ///< Adaptive normalization (computed)
        
        Config() 
            : window(50)
            , normalization_factor(200.0)
        {}
    };
    
    explicit OFISignalSource(const Config& cfg = Config())
        : _cfg(cfg)
        , _enabled(true)
        , _prev_bid_price(0)
        , _prev_ask_price(0)
        , _prev_bid_vol(0)
        , _prev_ask_vol(0)
        , _cumulative_ofi(0)
        , _last_timestamp(0)
    {
    }
    
    //==========================================================================
    // ISignalSource Interface
    //==========================================================================
    
    const std::string& name() const override 
    { 
        static std::string n = "OFI"; 
        return n; 
    }
    
    SignalType type() const override 
    { 
        return SignalType::OFI; 
    }
    
    void update(const MarketDataContext& book) override
    {
        double bid_px = book.getBidPrice();
        double ask_px = book.getAskPrice();
        double bid_vol = book.getBidVol();
        double ask_vol = book.getAskVol();
        uint64_t timestamp = book.getTimestamp();
        
        if (bid_px <= 0 || ask_px <= 0)
        {
            _result.valid = false;
            return;
        }
        
        if (_prev_bid_price > 0 && _prev_ask_price > 0)
        {
            // Calculate OFI using standard formulation
            // OFI = e_bid - e_ask
            double e_bid = 0, e_ask = 0;
            
            // Bid side
            if (bid_px > _prev_bid_price)
            {
                e_bid = bid_vol;  // New level, new volume
            }
            else if (bid_px < _prev_bid_price)
            {
                e_bid = -_prev_bid_vol;  // Level removed
            }
            else
            {
                e_bid = bid_vol - _prev_bid_vol;  // Volume change at same level
            }
            
            // Ask side (negative because ask volume increase is bearish)
            if (ask_px > _prev_ask_price)
            {
                e_ask = -_prev_ask_vol;  // Level removed
            }
            else if (ask_px < _prev_ask_price)
            {
                e_ask = ask_vol;  // New level, new volume
            }
            else
            {
                e_ask = ask_vol - _prev_ask_vol;  // Volume change at same level
            }
            
            // OFI = e_bid - e_ask
            double ofi = e_bid - e_ask;
            
            // Store in history
            _ofi_history.push(ofi);
            
            // Update cumulative OFI
            _cumulative_ofi += ofi;
            
            // 移除0.999乘法衰减，仅用ring buffer弹出做衰减
            // 原代码同时使用 *=0.999(乘法衰减) 和 -=front()(窗口弹出)，双重衰减导致信号偏小。
            // 方案A: 仅用ring buffer弹出做衰减，_cumulative_ofi始终等于sum(_ofi_history)，数学一致。
            // 方案B(已弃用): *=0.999 会在窗口内OFI=0时仍残留累积偏移，但该偏移量级极小
            //   (0.999^50≈0.95, 5%误差)，而ring buffer弹出已足够处理窗口外数据。
            //   双重衰减则使信号被过度压制(0.999^50 * 弹出 ≈ 信号偏小5%+)。
            
            // Remove old samples (5 second window approximation)
            while (_ofi_history.size() > _cfg.window)
            {
                _cumulative_ofi -= _ofi_history.front();
                _ofi_history.pop();
            }
            
            // Update adaptive normalization
            updateNormalization();
            
            // Normalize OFI using tanh
            _result.ofi = std::tanh(_cumulative_ofi / _cfg.normalization_factor);
            _result.cumulative_ofi = _cumulative_ofi;
            
            // Calculate bid/ask pressure
            double total_pressure = 1.0 + std::abs(_result.ofi);
            if (_result.ofi > 0)
            {
                _result.bid_pressure = (1.0 + _result.ofi) / (2.0 * total_pressure) * 2.0;
                _result.ask_pressure = 1.0 - _result.bid_pressure;
            }
            else
            {
                _result.ask_pressure = (1.0 - _result.ofi) / (2.0 * total_pressure) * 2.0;
                _result.bid_pressure = 1.0 - _result.ask_pressure;
            }
            
            _result.bid_pressure = std::clamp(_result.bid_pressure, 0.0, 1.0);
            _result.ask_pressure = std::clamp(_result.ask_pressure, 0.0, 1.0);
            
            _result.confidence = _ofi_history.size() >= 10 ? 1.0 : 0.5;
            _result.valid = true;
        }
        
        // Store current state
        _prev_bid_price = bid_px;
        _prev_ask_price = ask_px;
        _prev_bid_vol = bid_vol;
        _prev_ask_vol = ask_vol;
        _last_timestamp = timestamp;
        _result.timestamp = timestamp;
    }
    
    const SignalResult& result() const override { return _result; }
    
    bool enabled() const override { return _enabled; }
    void setEnabled(bool e) override { _enabled = e; }
    
    void reset() override
    {
        _prev_bid_price = 0;
        _prev_ask_price = 0;
        _prev_bid_vol = 0;
        _prev_ask_vol = 0;
        _cumulative_ofi = 0;
        _last_timestamp = 0;
        _ofi_history.clear();
        _result = OFISignalResult();
        _cfg.normalization_factor = 200.0;
    }
    
    /// Set configuration
    void setConfig(const Config& cfg) { _cfg = cfg; }
    
    //==========================================================================
    // Additional Accessors
    //==========================================================================
    
    double getOFI() const { return _result.ofi; }
    double getCumulativeOFI() const { return _result.cumulative_ofi; }
    double getBidPressure() const { return _result.bid_pressure; }
    double getAskPressure() const { return _result.ask_pressure; }
    
private:
    Config _cfg;
    bool _enabled;
    OFISignalResult _result;
    
    // Previous state for OFI calculation
    double _prev_bid_price;
    double _prev_ask_price;
    double _prev_bid_vol;
    double _prev_ask_vol;
    
    // OFI history
    RingBuffer<double, 128> _ofi_history;
    double _cumulative_ofi;
    uint64_t _last_timestamp;
    
    void updateNormalization()
    {
        size_t n = _ofi_history.size();
        if (n < 10) return;
        
        // 原方法: 用单 tick OFI 的 std_dev 做归一化
        // 问题: EC 盘口薄, 单 tick OFI 量级大手(±5~±20),
        //   但 cumulative_ofi 累积 50 tick 后量级更大(±50~±500),
        //   std_dev(单tick) 与 cumulative_ofi 量级不匹配 → tanh 饱和.
        
        // 新方法: 用 cumulative_ofi 的滚动幅度做归一化
        // 计算 _ofi_history 的 sum 的绝对值作为 cumulative 估计
        double cum_sum = 0;
        double cum_abs_sum = 0;
        for (size_t i = 0; i < n; ++i)
        {
            cum_sum += _ofi_history[i];
            cum_abs_sum += std::abs(_ofi_history[i]);
        }
        
        // avg_abs_ofi = 窗口内平均单 tick |OFI|
        // cumulative 的典型幅度 ≈ avg_abs_ofi × sqrt(n) (随机游走)
        // 或 ≈ avg_abs_ofi × n (同方向累积)
        // 用 avg_abs_ofi × n 作为保守估计 (覆盖同方向累积情况)
        double avg_abs = cum_abs_sum / n;
        double cum_scale = avg_abs * n * 0.5;  // 0.5 因子: 介于随机游走和同方向之间
        
        // 归一化因子: 使典型 cumulative_ofi 映射到 tanh 的线性区 (0.5-0.8)
        if (cum_scale > 0.1)
        {
            _cfg.normalization_factor = std::clamp(cum_scale, 5.0, 500.0);
        }
    }
};

} // namespace futu
