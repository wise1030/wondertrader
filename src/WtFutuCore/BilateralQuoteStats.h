/*!
 * \file BilateralQuoteStats.h
 * \brief 双边报价统计模块（Per-Quoter 独立，符合做市商义务考核语义）
 *
 * R3 v2 设计要点（2026-06）:
 *   - Per-Quoter 独立：每个 FutuQuoter 持有一个 BilateralQuoteStats 值成员
 *   - 累计加权 spread：以 min_valid_qty 为深度阈值，深度内按量加权计算 ask/bid 价
 *   - SessionInfo 注入：用 WTSSessionInfo::timeToMinutes 把 HHMM 映射到 session 累计分钟
 *     非交易时段返回 INVALID_UINT32 → 该次更新不计入统计
 *   - 内部时间单位：minute*1000 + sec_in_min（单调）
 *   - 单一总占比（Q-X4=A）：bilateral_time_secs / session_total_secs
 *
 * 使用方式:
 *   1. 配置:    setConfig(BilateralStatsConfig{...});
 *   2. 注入:    setSessionInfo(commInfo->getSessionInfo());  // 必须，nullptr 会硬失败
 *   3. 日初:    onSessionStart(uTime_HHMM);
 *   4. 报单时:  update(snapshot, uTime_HHMM, sec_in_min);
 *   5. 日末:    onSessionEnd(uTime_HHMM);
 *   6. 输出:    formatString();
 *
 * 与 824 旧版差异:
 *   - 移除：share-pointer 的单实例管理（旧版 N quoter 共享同一状态机导致 N 倍累加）
 *   - 移除：基于 wallclock_ms 的 _session_time（语义错乱）
 *   - 移除：单档 has_valid_bid 的 ValidQuoteSnapshot（已移到 FutuQuoter 累计算法）
 *   - 新增：setSessionInfo + timeToMinutes 计算 session 累计时间
 *   - 新增：累计加权 spread（FutuQuoter::getValidQuoteSnapshot 端实现，本模块只接收结果）
 */
#pragma once

#include <cstdint>
#include <cmath>
#include <string>
#include "../Includes/WTSSessionInfo.hpp"
#include "../WTSTools/WTSLogger.h"

namespace futu {

/// 有效报价快照（Per-Quoter，由 FutuQuoter::getValidQuoteSnapshot 累计加权计算后填充）
///
/// 累计加权语义（与交易所做市义务对齐）:
///   - has_valid_bid: bid 侧累计深度 ≥ min_valid_qty
///   - weighted_bid_price: 累计前 min_valid_qty 手的按量加权平均价
///     (最后一档可能只取一部分以恰好凑够 min_valid_qty)
///   - ask 侧对称
struct ValidQuoteSnapshot
{
    bool    has_valid_bid;          ///< bid 侧累计深度是否够 min_valid_qty
    bool    has_valid_ask;          ///< ask 侧累计深度是否够 min_valid_qty
    double  weighted_bid_price;     ///< bid 侧累计加权价
    double  weighted_ask_price;     ///< ask 侧累计加权价
    double  tick_size;              ///< 合约最小变动价位

    ValidQuoteSnapshot()
        : has_valid_bid(false), has_valid_ask(false)
        , weighted_bid_price(0), weighted_ask_price(0), tick_size(1.0) {}

    /// 计算累计加权 spread（tick 数）
    inline double getSpreadTicks() const
    {
        if (!has_valid_bid || !has_valid_ask || tick_size <= 0)
            return 0.0;
        return (weighted_ask_price - weighted_bid_price) / tick_size;
    }
};

/// 双边报价统计配置
struct BilateralStatsConfig
{
    double  min_valid_qty;              ///< 累计深度阈值（手），如 ao=10
    double  max_obligation_spread;      ///< 做市义务最大宽度（tick），如 50

    BilateralStatsConfig()
        : min_valid_qty(10.0), max_obligation_spread(50.0) {}
};

/// 双边报价统计结果
struct BilateralStatsResult
{
    uint64_t    total_bilateral_time_sec;   ///< 双边挂单累计时间（秒）
    uint64_t    total_session_time_sec;     ///< session 全天交易时间（秒）
    double      bilateral_ratio;            ///< 双边挂单时间占比 [0, 1]
    double      avg_spread_ticks;           ///< 累计加权 spread 平均（tick）
    uint64_t    bilateral_sample_count;     ///< 双边样本数
    uint32_t    bilateral_switch_count;     ///< 双边状态切换次数

    BilateralStatsResult()
        : total_bilateral_time_sec(0), total_session_time_sec(0)
        , bilateral_ratio(0), avg_spread_ticks(0)
        , bilateral_sample_count(0), bilateral_switch_count(0) {}
};

/// 双边报价统计器（Per-Quoter 独立）
class BilateralQuoteStats
{
public:
    BilateralQuoteStats()
        : _session_info(nullptr)
        , _last_minute_units(0)         // minute*1000 + sec_in_min
        , _bilateral_start_units(0)
        , _total_bilateral_units(0)
        , _session_total_secs(0)
        , _is_bilateral(false)
        , _bilateral_switch_count(0)
        , _total_spread_ticks(0)
        , _spread_sample_count(0) {}

    /// 设置配置
    void setConfig(const BilateralStatsConfig& cfg) { _cfg = cfg; }
    const BilateralStatsConfig& getConfig() const { return _cfg; }

    /// 注入 SessionInfo（必须！nullptr 会让 onSessionStart 硬失败禁用统计）
    /// @return true=注入成功 false=nullptr,统计模块已禁用
    bool setSessionInfo(WTSSessionInfo* sessInfo, const char* code_for_log = "")
    {
        _session_info = sessInfo;
        if (!sessInfo)
        {
            WTSLogger::error("[BILATERAL_STATS] {} setSessionInfo: nullptr, statistics DISABLED for this code",
                             code_for_log ? code_for_log : "");
            return false;
        }

        // 预计算 session 总秒数（所有 trading section 长度求和）
        const auto& sections = sessInfo->getTradingSections();
        uint64_t total_min = 0;
        for (const auto& sec : sections)
        {
            // section.first / section.second 是 HHMM 格式
            uint32_t start_min = (sec.first / 100) * 60 + (sec.first % 100);
            uint32_t end_min   = (sec.second / 100) * 60 + (sec.second % 100);
            if (end_min >= start_min)
                total_min += (end_min - start_min);
            else
                total_min += (1440 - start_min + end_min);  // 跨午夜（夜盘）
        }
        _session_total_secs = total_min * 60;

        WTSLogger::info("[BILATERAL_STATS] {} sessinfo='{}' total_session={}min ({} sections)",
                        code_for_log ? code_for_log : "",
                        sessInfo->id() ? sessInfo->id() : "",
                        total_min, sections.size());
        return true;
    }

    bool hasSessionInfo() const { return _session_info != nullptr; }

    /// 会话开始
    /// @param uTime_HHMM 当前时间 HHMM 格式（来自 stra_get_time）
    void onSessionStart(uint32_t uTime_HHMM)
    {
        _last_minute_units = 0;
        _bilateral_start_units = 0;
        _total_bilateral_units = 0;
        _is_bilateral = false;
        _bilateral_switch_count = 0;
        _total_spread_ticks = 0;
        _spread_sample_count = 0;
        // session_total_secs 在 setSessionInfo 时已算好,不重置
    }

    /// 会话结束 — 如果当前是双边状态，把残余时长计入累计
    /// @param uTime_HHMM 当前时间 HHMM 格式
    /// @param sec_in_min 分钟内秒数 [0, 59]
    void onSessionEnd(uint32_t uTime_HHMM, uint32_t sec_in_min)
    {
        if (!_session_info) return;

        uint64_t now_units = computeMinuteUnits(uTime_HHMM, sec_in_min);
        if (now_units == INVALID_UNITS) return;  // 非交易时段，不更新

        if (_is_bilateral && _bilateral_start_units > 0 && now_units >= _bilateral_start_units)
        {
            _total_bilateral_units += (now_units - _bilateral_start_units);
            _bilateral_start_units = 0;
        }
        _is_bilateral = false;
    }

    /// 检查 snapshot 是否符合做市义务（累计深度 + 加权 spread 双判）
    bool checkBilateral(const ValidQuoteSnapshot& snapshot) const
    {
        if (!snapshot.has_valid_bid || !snapshot.has_valid_ask)
            return false;
        double spread = snapshot.getSpreadTicks();
        if (spread <= 0 || spread > _cfg.max_obligation_spread)
            return false;
        return true;
    }

    /// 更新统计状态（每次 onOrder/onTrade 调用，O(1)）
    /// @param snapshot 累计加权快照
    /// @param uTime_HHMM 当前时间 HHMM
    /// @param sec_in_min 分钟内秒数
    void update(const ValidQuoteSnapshot& snapshot, uint32_t uTime_HHMM, uint32_t sec_in_min)
    {
        if (!_session_info) return;  // 硬失败:无 sessinfo 不统计

        uint64_t now_units = computeMinuteUnits(uTime_HHMM, sec_in_min);
        if (now_units == INVALID_UNITS) return;  // 非交易时段，丢弃

        bool new_bilateral = checkBilateral(snapshot);

        // 状态切换处理：bilateral 进/出
        if (new_bilateral && !_is_bilateral)
        {
            // 进入双边
            _bilateral_start_units = now_units;
            _bilateral_switch_count++;
        }
        else if (!new_bilateral && _is_bilateral && _bilateral_start_units > 0
                 && now_units >= _bilateral_start_units)
        {
            // 退出双边：把这段时长计入累计
            _total_bilateral_units += (now_units - _bilateral_start_units);
            _bilateral_start_units = 0;
        }
        // else: 持续双边或持续非双边，不做时间累加（onSessionEnd 时 flush 残余双边段）

        _is_bilateral = new_bilateral;
        _last_minute_units = now_units;

        // Spread 样本（仅在双边时记录）
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

    /// 获取统计结果
    BilateralStatsResult getResult() const
    {
        BilateralStatsResult result;

        // 内部单位 = session 累计秒数（computeMinuteUnits 返回 minutes*60+sec_in_min）
        // 双边累计差值 = 双边时长（秒）
        result.total_bilateral_time_sec = _total_bilateral_units;
        result.total_session_time_sec = _session_total_secs;
        result.bilateral_ratio = (_session_total_secs > 0)
            ? (double)_total_bilateral_units / _session_total_secs : 0.0;

        result.avg_spread_ticks = (_spread_sample_count > 0)
            ? _total_spread_ticks / _spread_sample_count : 0.0;
        result.bilateral_sample_count = _spread_sample_count;
        result.bilateral_switch_count = _bilateral_switch_count;
        return result;
    }

    bool isBilateral() const { return _is_bilateral; }
    double getBilateralRatio() const { return getResult().bilateral_ratio; }
    double getAvgSpreadTicks() const
    {
        return (_spread_sample_count > 0) ? _total_spread_ticks / _spread_sample_count : 0.0;
    }
    uint32_t getSwitchCount() const { return _bilateral_switch_count; }

    /// 格式化输出
    std::string formatString() const
    {
        auto r = getResult();
        char buf[256];
        snprintf(buf, sizeof(buf),
            "bilateral=%llus session=%llus ratio=%.2f%% avg_spread=%.2fticks switches=%u samples=%llu",
            (unsigned long long)r.total_bilateral_time_sec,
            (unsigned long long)r.total_session_time_sec,
            r.bilateral_ratio * 100.0,
            r.avg_spread_ticks,
            r.bilateral_switch_count,
            (unsigned long long)r.bilateral_sample_count);
        return std::string(buf);
    }

private:
    static constexpr uint64_t INVALID_UNITS = (uint64_t)-1;

    /// 把 (HHMM, sec) 映射到 session 累计秒数（单调）
    /// 非交易时段返回 INVALID_UNITS
    uint64_t computeMinuteUnits(uint32_t uTime_HHMM, uint32_t sec_in_min) const
    {
        if (!_session_info) return INVALID_UNITS;
        uint32_t mins = _session_info->timeToMinutes(uTime_HHMM, false);
        if (mins == INVALID_UINT32) return INVALID_UNITS;
        return (uint64_t)mins * 60 + (sec_in_min < 60 ? sec_in_min : 59);
    }

private:
    BilateralStatsConfig _cfg;
    WTSSessionInfo*      _session_info;          ///< 注入的 session（不持有所有权）

    // 时间累计（单位：秒）
    uint64_t _last_minute_units;                 ///< 最近一次 update 的 session-累计秒
    uint64_t _bilateral_start_units;             ///< 当前双边段的起始 session-累计秒
    uint64_t _total_bilateral_units;             ///< 双边累计秒
    uint64_t _session_total_secs;                ///< session 总秒数（trading section 之和）

    bool     _is_bilateral;
    uint32_t _bilateral_switch_count;

    // Spread 样本
    double   _total_spread_ticks;
    uint64_t _spread_sample_count;
};

} // namespace futu
