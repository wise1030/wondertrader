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

// 2026-09-19③ 批次B: 前向声明 + 指针持有, 避免与 StrategyCoordinator.h 循环包含
// (coordinator 持有 _phase_mgr 成员); 读配置函数体移入 SessionPhaseManager.cpp
struct CoordinatorConfig; // StrategyCoordinator.h

class SessionPhaseManager
{
public:
    /// 批次B: 改持 CoordinatorConfig 指针 (coordinator 内部助手惯例, 同 CloseoutTrigger::Deps.cfg),
    /// 消除原 SessionPhaseConfig 值拷贝第三副本。指针寿命: _cfg 为 coordinator 成员, 与本对象同寿命。
    void configure(const CoordinatorConfig& cfg);

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
    /// 实现见 .cpp (读 _cfg->section_break_seconds_before)
    bool inSectionBreak(const std::string& code, uint32_t hhmmss, uint32_t secs_in_min) const;

    /// 19.26 F3②: 当前时间所属交易节序号 (sections[i] 内返 i; 非任何节内返 -1)。
    /// 供统计沉默 watchdog 判定"同节断档 vs 跨节跳段"。
    int sectionIndex(const std::string& code, uint32_t hhmmss) const;

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
    /// 实现见 .cpp (读 _cfg-> 窗口字段)
    SessionPhase phase(const std::string& code, uint32_t hhmmss, uint32_t secs_in_min) const;

private:
    const CoordinatorConfig* _cfg = nullptr; // 批次B: 指针持主配置 (configure 注入, 免值拷贝)
    wtp::wt_hashmap<std::string, wtp::WTSSessionInfo*> _sessions;
};

} // namespace futu
