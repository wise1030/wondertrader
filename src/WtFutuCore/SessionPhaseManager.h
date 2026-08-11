/*!
 * \file SessionPhaseManager.h
 * \brief 5A-1 (v7.5): 会话阶段统一判定 — 时间窗口逻辑单一事实来源
 *
 * 消灭散落于 coordinator / risk monitor 的三份时间窗口判定:
 *   1. processSectionBreak  每节收盘前 N 分钟休息窗口 (StrategyCoordinator)
 *   2. preCheck             isInTradingTime 交易时段门 (StrategyCoordinator)
 *   3. checkCloseout        夜盘/白盘收盘前 closeout 触发窗口 (FutuRiskMonitor)
 *
 * 纯函数设计: 本类只回答"当前时间处于什么窗口", 不含任何状态机
 * (closeout 执行状态机 IDLE→TRIGGERED→DRAINING→... 仍归 FutuRiskMonitor)。
 *
 * 时间格式约定 (与原逻辑一致):
 *   - HHMM (4位, 如 930=09:30) 或 HHMMSS (6位, 如 150000=15:00) 均兼容
 *   - night_close_time: HHMM; 跨日品种收盘在凌晨 (如 230=02:30, 100=01:00)
 *   - close_time: HHMMSS (如 150000)
 */
#pragma once

#include <string>
#include <cstdint>
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSSessionInfo.hpp"
#include "../WTSTools/WTSLogger.h"

namespace futu
{

enum class SessionPhase
{
    CLOSED,         ///< 不在交易时段
    CONTINUOUS,     ///< 连续交易 (正常做市)
    SECTION_BREAK,  ///< 中间节收盘前 N 分钟休息窗口
    CLOSEOUT_WINDOW ///< 日终/夜盘收盘前 closeout 触发窗口
};

struct SessionPhaseConfig
{
    uint32_t section_break_seconds_before = 10; ///< 每节收盘前 N 秒休息 (0=禁用, 默认10s)
    uint32_t close_time = 150000;              ///< 全天收盘 HHMMSS
    uint32_t closeout_minutes_before = 5;      ///< 白盘收盘前 N 分钟触发 closeout
    uint32_t night_close_time = 0;             ///< 夜盘收盘 HHMM (0=无夜盘)
    uint32_t night_minutes_before = 5;         ///< 夜盘收盘前 N 分钟触发 closeout
};

class SessionPhaseManager
{
public:
    void configure(const SessionPhaseConfig& cfg) { _cfg = cfg; }
    const SessionPhaseConfig& config() const { return _cfg; }

    void setSessionInfo(const std::string& code, wtp::WTSSessionInfo* sess) { _sessions[code] = sess; }

    wtp::WTSSessionInfo* getSession(const std::string& code) const
    {
        auto it = _sessions.find(code);
        return (it != _sessions.end()) ? it->second : nullptr;
    }

    /// HHMM 或 HHMMSS -> (hour, min); 非法返回 false
    static bool parseHhmm(uint32_t hhmm_or_hhmmss, uint32_t& hour, uint32_t& min)
    {
        if (hhmm_or_hhmmss >= 10000) {
            hour = hhmm_or_hhmmss / 10000;
            min = (hhmm_or_hhmmss / 100) % 100;
        } else {
            hour = hhmm_or_hhmmss / 100;
            min = hhmm_or_hhmmss % 100;
        }
        return hour <= 23 && min <= 59;
    }

    /// 是否在交易时段 (无 session 信息时默认 true, 兼容旧行为)
    bool isTrading(const std::string& code, uint32_t hhmmss) const
    {
        wtp::WTSSessionInfo* sess = getSession(code);
        if (!sess)
            return true;
        return sess->isInTradingTime(hhmmss);
    }

    /// 中间节休息窗口: 每节收盘前 N 秒 (最后一节归 closeout, 跳过)
    /// @param hhmmss 当前时间 HHMM/HHMMSS (stra_get_time)
    /// @param secs_in_min 分钟内秒数 0-59 (stra_get_secs/1000)
    bool inSectionBreak(const std::string& code, uint32_t hhmmss, uint32_t secs_in_min) const
    {
        if (_cfg.section_break_seconds_before == 0)
            return false;

        wtp::WTSSessionInfo* sess = getSession(code);
        if (!sess)
            return false;

        const auto& sections = sess->getTradingSections();
        if (sections.size() < 2)
            return false; // 单节品种无中间休息段

        uint32_t hour, min;
        if (!parseHhmm(hhmmss, hour, min))
            return false;
        // v7.9: 秒级窗口 (默认小节结束前10s撤单停报, 原1分钟过早, 牺牲成交机会)
        uint32_t cur_sec = hour * 3600 + min * 60 + secs_in_min;

        for (size_t i = 0; i + 1 < sections.size(); i++) {
            // v7.8 fix: 必须用 second_raw (墙钟原始节尾), 不能用 .second
            // .second 是 offsetTime(eTime) 按 session offset 偏移后的时间
            // (FN0100/FD0900 等 offset=300min), 与 stra_get_time 原始墙钟比较
            // 会把休息窗口整体后移 5 小时, 全部落到非交易时段, SECTION_BREAK 永不触发.
            // 框架 isInTradingTime 是输入时间先 offsetTime 再比 (两侧同基准);
            // 本处直接用 raw 对 raw, 与 FutuModuleAssembler 推导 close_time 口径一致.
            uint32_t end_hhmm = sections[i].second_raw;
            uint32_t end_sec = (end_hhmm / 100) * 3600 + (end_hhmm % 100) * 60;
            if (end_sec >= _cfg.section_break_seconds_before &&
                cur_sec >= end_sec - _cfg.section_break_seconds_before && cur_sec < end_sec)
                return true;
        }
        return false;
    }

    /// 夜盘 closeout 触发时间窗口 (仅时间判定, 不含状态机门)
    /// 窗口语义与原 checkCloseout 一致: 只有下界 (进入窗口后恒 true,
    /// 防重复靠调用方 IDLE/night_closeout_done 状态门)
    static bool inNightCloseoutWindow(uint32_t currentTime, uint32_t night_close_time, uint32_t night_minutes_before)
    {
        if (night_close_time == 0 || night_minutes_before == 0)
            return false;

        uint32_t currentHour, currentMin;
        if (!parseHhmm(currentTime, currentHour, currentMin)) {
            WTSLogger::warn("[RISK] Invalid current time format: {}", currentTime);
            return false;
        }

        // 只在夜盘时段 (21:00-05:59) 检查, 避免白盘误触发
        if (!(currentHour >= 21 || currentHour < 6))
            return false;

        uint32_t nightCloseHour = night_close_time / 100;
        uint32_t nightCloseMin = night_close_time % 100;
        if (nightCloseHour > 23 || nightCloseMin > 59) {
            WTSLogger::warn("[RISK] Invalid night_close_time format: {} (hour={}, min={}), "
                            "possible octal misconfiguration. Expected HHMM format.",
                            night_close_time,
                            nightCloseHour,
                            nightCloseMin);
            return false;
        }

        uint32_t currentTotalMin = currentHour * 60 + currentMin;
        uint32_t nightCloseTotalMin = nightCloseHour * 60 + nightCloseMin;

        bool is_overnight = (nightCloseHour < 6); // 收盘在凌晨 → 跨日品种
        if (is_overnight) {
            // 跨日品种统一时间轴: 21:00-23:59 原值 (1260-1439);
            // 00:00-05:59 +1440; close=02:30 → 150+1440=1590
            int32_t closeAbs = static_cast<int32_t>(nightCloseTotalMin) + 1440;
            int32_t triggerAbs = closeAbs - static_cast<int32_t>(night_minutes_before);
            int32_t currentAbs = (currentTotalMin >= 1260) ? static_cast<int32_t>(currentTotalMin)
                                                           : static_cast<int32_t>(currentTotalMin) + 1440;
            return currentAbs >= triggerAbs;
        } else {
            int32_t triggerTotalMin =
                static_cast<int32_t>(nightCloseTotalMin) - static_cast<int32_t>(night_minutes_before);
            if (triggerTotalMin < 0)
                triggerTotalMin = 0;
            return static_cast<int32_t>(currentTotalMin) >= triggerTotalMin;
        }
    }

    /// 白盘 (全天收盘) closeout 触发时间窗口 (仅时间判定, 不含状态机门)
    static bool inDayCloseoutWindow(uint32_t currentTime, uint32_t closeTime, uint32_t minutes_before)
    {
        if (minutes_before == 0)
            return false;

        uint32_t currentHour, currentMin;
        if (!parseHhmm(currentTime, currentHour, currentMin)) {
            WTSLogger::warn("[RISK] Invalid current time format: {}", currentTime);
            return false;
        }

        // 只在白盘时段 (06:00-15:59) 检查, 避免夜盘 21:00+ 误触发
        if (currentHour < 6 || currentHour > 15)
            return false;

        uint32_t closeHour, closeMin;
        if (closeTime < 10000) {
            closeHour = closeTime / 100;
            closeMin = closeTime % 100;
        } else {
            closeHour = closeTime / 10000;
            closeMin = (closeTime / 100) % 100;
        }
        if (closeHour > 23 || closeMin > 59) {
            WTSLogger::warn("[RISK] Invalid close time format: {}, using default 15:15", closeTime);
            closeHour = 15;
            closeMin = 15;
        }

        uint32_t currentTotalMin = currentHour * 60 + currentMin;
        uint32_t closeTotalMin = closeHour * 60 + closeMin;
        int32_t triggerTotalMin = static_cast<int32_t>(closeTotalMin) - static_cast<int32_t>(minutes_before);
        if (triggerTotalMin < 0)
            triggerTotalMin = 0;
        return static_cast<int32_t>(currentTotalMin) >= triggerTotalMin;
    }

    /// 综合阶段判定: CLOSEOUT_WINDOW > SECTION_BREAK > CONTINUOUS > CLOSED
    SessionPhase phase(const std::string& code, uint32_t hhmmss, uint32_t secs_in_min) const
    {
        if (!isTrading(code, hhmmss))
            return SessionPhase::CLOSED;

        bool night = inNightCloseoutWindow(hhmmss, _cfg.night_close_time, _cfg.night_minutes_before);
        bool day = inDayCloseoutWindow(hhmmss, _cfg.close_time, _cfg.closeout_minutes_before);
        if (night || day)
            return SessionPhase::CLOSEOUT_WINDOW;

        if (inSectionBreak(code, hhmmss, secs_in_min))
            return SessionPhase::SECTION_BREAK;

        return SessionPhase::CONTINUOUS;
    }

private:
    SessionPhaseConfig _cfg;
    wtp::wt_hashmap<std::string, wtp::WTSSessionInfo*> _sessions;
};

} // namespace futu
