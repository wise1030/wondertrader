/*!
 * \file BilateralQuoteStats.h
 * \brief 双边报价统计模块（独立模块，不影响报单延迟）
 * 
 * 统计内容：
 *   - 双边报价累计时间
 *   - 占全天交易时间比例
 *   - 平均 bid/ask 宽度（tick 数）
 *   - 有效挂单统计（符合做市最小挂单要求）
 * 
 * 设计要点：
 *   - 独立模块，不耦合核心交易逻辑
 *   - O(1) 更新操作，对报单延迟无影响
 *   - 每次报价后调用 update()，传入双边状态
 */
#pragma once

#include <cstdint>
#include <cmath>
#include <string>

namespace futu {

/// 有效报价信息（单次报价快照）
struct ValidQuoteSnapshot
{
    bool    has_valid_bid;      ///< 是否有有效买单
    bool    has_valid_ask;      ///< 是否有有效卖单
    double  best_bid_price;     ///< 最优买价（满足最小挂单要求的 level 0）
    double  best_ask_price;     ///< 最优卖价（满足最小挂单要求的 level 0）
    double  tick_size;          ///< 合约最小变动价位
    
    ValidQuoteSnapshot()
        : has_valid_bid(false), has_valid_ask(false)
        , best_bid_price(0), best_ask_price(0), tick_size(1.0) {}
    
    /// 计算 bid/ask 宽度（tick 数）
    inline double getSpreadTicks() const
    {
        if (!has_valid_bid || !has_valid_ask || tick_size <= 0)
            return 0.0;
        return (best_ask_price - best_bid_price) / tick_size;
    }
};

/// 双边报价统计配置
struct BilateralStatsConfig
{
    double  min_valid_qty;          ///< 有效挂单最小数量（默认 1.0）
    double  max_obligation_spread;   ///< 做市义务最大宽度（tick，做市宽度越小越好，默认 100.0）
    bool    require_level0;         ///< 是否要求 level 0 有效（默认 true）
    
    BilateralStatsConfig()
        : min_valid_qty(1.0), max_obligation_spread(100.0), require_level0(true) {}
};

/// 双边报价统计结果
struct BilateralStatsResult
{
    uint64_t    total_bilateral_time_ms;    ///< 双边挂单累计时间（毫秒）
    uint64_t    total_session_time_ms;      ///< 全天交易时间（毫秒）
    double      bilateral_ratio;            ///< 双边挂单时间占比
    double      avg_spread_ticks;           ///< 平均 bid/ask 宽度（tick）
    uint64_t    bilateral_sample_count;     ///< 双边报价样本数
    uint64_t    total_sample_count;         ///< 总报价样本数
    uint32_t    bilateral_switch_count;     ///< 双边状态切换次数
    
    BilateralStatsResult()
        : total_bilateral_time_ms(0), total_session_time_ms(0)
        , bilateral_ratio(0), avg_spread_ticks(0)
        , bilateral_sample_count(0), total_sample_count(0)
        , bilateral_switch_count(0) {}
};

/// 双边报价统计器（独立模块）
/// 
/// 使用方式：
/// 1. 交易日开始时调用 onSessionStart()
/// 2. 每次报价后调用 update()，传入 ValidQuoteSnapshot
/// 3. 交易日结束时调用 onSessionEnd()
/// 4. 通过 getResult() 或 formatString() 获取统计结果
class BilateralQuoteStats
{
public:
    BilateralQuoteStats() 
        : _bilateral_start_time(0)
        , _total_bilateral_time(0)
        , _session_start_time(0)
        , _session_time(0)
        , _is_bilateral(false)
        , _bilateral_switch_count(0)
        , _total_spread_ticks(0)
        , _spread_sample_count(0) {}
    
    /// 设置配置
    void setConfig(const BilateralStatsConfig& cfg) { _cfg = cfg; }
    const BilateralStatsConfig& getConfig() const { return _cfg; }
    
    /// 会话开始（交易日开始时调用）
    void onSessionStart(uint64_t now)
    {
        _session_start_time = now;
        _bilateral_start_time = 0;
        _total_bilateral_time = 0;
        _is_bilateral = false;
        _bilateral_switch_count = 0;
        _total_spread_ticks = 0;
        _spread_sample_count = 0;
    }
    
    /// 会话结束（交易日结束时调用）
    void onSessionEnd(uint64_t now)
    {
        // 如果当前是双边状态，需要计入
        if (_is_bilateral && _bilateral_start_time > 0 && now >= _bilateral_start_time)
        {
            _total_bilateral_time += (now - _bilateral_start_time);
            _bilateral_start_time = now;
        }
        if (now >= _session_start_time)
        {
            _session_time = now - _session_start_time;
        }
    }
    
    /// 检查是否为有效双边挂单（满足数量和做市义务宽度要求）
    /// 有效双边挂单定义：
    ///   1. bid 和 ask 都有有效挂单
    ///   2. 挂单数量 ≥ min_valid_qty
    ///   3. 宽度 ≤ max_obligation_spread（符合做市义务要求）
    bool checkBilateral(const ValidQuoteSnapshot& snapshot) const
    {
        // 必须双边都有有效挂单
        if (!snapshot.has_valid_bid || !snapshot.has_valid_ask)
            return false;
            
        // 宽度校验：符合做市义务最大宽度要求
        double spread = snapshot.getSpreadTicks();
        if (spread <= 0 || spread > _cfg.max_obligation_spread)
            return false;
            
        return true;
    }

    //==========================================================================
    // 核心接口：更新统计（报价后调用，O(1) 操作）
    //==========================================================================
    
    /// 更新统计状态
    /// @param snapshot 有效报价快照
    /// @param now      当前时间戳（毫秒）
    void update(const ValidQuoteSnapshot& snapshot, uint64_t now)
    {
        // 1. 更新双边状态
        bool new_bilateral = checkBilateral(snapshot);
        
        if (new_bilateral && !_is_bilateral)
        {
            // 开始双边挂单
            _bilateral_start_time = now;
            _bilateral_switch_count++;
        }
        else if (!new_bilateral && _is_bilateral && _bilateral_start_time > 0 && now >= _bilateral_start_time)
        {
            // 结束双边挂单
            _total_bilateral_time += (now - _bilateral_start_time);
        }
        
        _is_bilateral = new_bilateral;
        
        // 2. 更新 session 时间
        if (_session_start_time > 0 && now >= _session_start_time)
        {
            _session_time = now - _session_start_time;
        }
        
        // 3. 更新 spread 统计（仅在双边时记录）
        if (new_bilateral)
        {
            double spread_ticks = snapshot.getSpreadTicks();
            if (spread_ticks > 0)
            {
                _total_spread_ticks += spread_ticks;
                _spread_sample_count++;
            }
        }
    }
    
    //==========================================================================
    // 结果获取
    //==========================================================================
    
    /// 获取统计结果
    BilateralStatsResult getResult() const
    {
        BilateralStatsResult result;
        
        // 双边时间统计
        result.total_bilateral_time_ms = _total_bilateral_time;
        result.total_session_time_ms = _session_time;
        result.bilateral_ratio = (_session_time > 0) 
            ? (double)_total_bilateral_time / _session_time : 0.0;
        
        // Spread 统计
        result.avg_spread_ticks = (_spread_sample_count > 0)
            ? _total_spread_ticks / _spread_sample_count : 0.0;
        
        // 样本计数
        result.bilateral_sample_count = _spread_sample_count;
        result.total_sample_count = _spread_sample_count;  // 仅双边时记录
        result.bilateral_switch_count = _bilateral_switch_count;
        
        return result;
    }
    
    /// 获取当前是否双边挂单
    bool isBilateral() const { return _is_bilateral; }
    
    /// 获取双边挂单时间占比（快捷接口）
    double getBilateralRatio() const
    {
        return (_session_time > 0) ? (double)_total_bilateral_time / _session_time : 0.0;
    }
    
    /// 获取平均 spread（tick）
    double getAvgSpreadTicks() const
    {
        return (_spread_sample_count > 0) ? _total_spread_ticks / _spread_sample_count : 0.0;
    }
    
    /// 获取双边切换次数
    uint32_t getSwitchCount() const { return _bilateral_switch_count; }
    
    /// 格式化输出（用于日志）
    std::string formatString() const
    {
        auto result = getResult();
        char buf[256];
        snprintf(buf, sizeof(buf),
            "bilateral_time=%.1fs session_time=%.1fs ratio=%.2f%% avg_spread=%.2fticks switches=%u",
            result.total_bilateral_time_ms / 1000.0,
            result.total_session_time_ms / 1000.0,
            result.bilateral_ratio * 100.0,
            result.avg_spread_ticks,
            result.bilateral_switch_count);
        return std::string(buf);
    }
    
private:
    BilateralStatsConfig _cfg;
    
    // 双边时间统计
    uint64_t _bilateral_start_time = 0;     ///< 双边开始时间
    uint64_t _total_bilateral_time = 0;     ///< 双边累计时间
    uint64_t _session_start_time = 0;       ///< 会话开始时间
    uint64_t _session_time = 0;             ///< 会话累计时间
    bool     _is_bilateral = false;         ///< 当前是否双边
    uint32_t _bilateral_switch_count = 0;   ///< 双边切换次数
    
    // Spread 统计
    double   _total_spread_ticks = 0;       ///< Spread 累计（tick）
    uint64_t _spread_sample_count = 0;      ///< Spread 样本数
};

} // namespace futu
