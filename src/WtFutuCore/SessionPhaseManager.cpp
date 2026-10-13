/*!
 * \file SessionPhaseManager.cpp
 * \brief SessionPhaseManager 中读取 CoordinatorConfig 的函数体
 *
 * 2026-09-19③ 批次B: 本类由"持 SessionPhaseConfig 值拷贝"改为"持 CoordinatorConfig 指针"。
 * 头文件仅存指针 + 前向声明 (避免与 StrategyCoordinator.h 循环包含 — coordinator 持有
 * _phase_mgr 成员), 故读 _cfg-> 字段的函数体集中在本文件。
 * 纯静态时间窗口判定 (inNightCloseoutWindow/inDayCloseoutWindow 等) 不含配置读取,
 * 保留在头文件内联, FutuRiskMonitor 等外部调用方不受影响。
 */
#include "SessionPhaseManager.h"
#include "StrategyCoordinator.h"

namespace futu
{

void SessionPhaseManager::configure(const CoordinatorConfig& cfg)
{
    _cfg = &cfg;
}

bool SessionPhaseManager::inSectionBreak(const std::string& code, uint32_t hhmmss, uint32_t secs_in_min) const
{
    if (!_cfg || _cfg->section_break_seconds_before == 0)
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
        if (end_sec >= _cfg->section_break_seconds_before &&
            cur_sec >= end_sec - _cfg->section_break_seconds_before && cur_sec < end_sec)
            return true;

        // 19.26 F2: 节间空隙整体视为 break (原窗口只覆盖 [end-N, end))。
        // 2026-09-24 实证: 11:30:05 本节尾后首 tick 触发 resume, 往已闭市上午场报单,
        // CTP 批量拒"当前状态禁止交易" — resume 必须等下一节开盘 (13:30), 空隙内
        // MM pipeline 全程跳过, 与午休前撤单语义一致。隔夜空隙(01:00→09:00)无 tick
        // 天然不触发, 行为不变。
        uint32_t next_start_hhmm = sections[i + 1].first_raw;
        uint32_t next_start_sec = (next_start_hhmm / 100) * 3600 + (next_start_hhmm % 100) * 60;
        if (cur_sec >= end_sec && cur_sec < next_start_sec)
            return true;
    }
    return false;
}

int SessionPhaseManager::sectionIndex(const std::string& code, uint32_t hhmmss) const
{
    wtp::WTSSessionInfo* sess = getSession(code);
    if (!sess)
        return -1;

    uint32_t hour, min;
    if (!parseHhmm(hhmmss, hour, min))
        return -1;
    uint32_t cur_sec = hour * 3600 + min * 60;

    const auto& sections = sess->getTradingSections();
    for (size_t i = 0; i < sections.size(); i++) {
        uint32_t s_hhmm = sections[i].first_raw;
        uint32_t e_hhmm = sections[i].second_raw;
        uint32_t s_sec = (s_hhmm / 100) * 3600 + (s_hhmm % 100) * 60;
        uint32_t e_sec = (e_hhmm / 100) * 3600 + (e_hhmm % 100) * 60;
        if (cur_sec >= s_sec && cur_sec <= e_sec)
            return static_cast<int>(i);
    }
    return -1;
}

SessionPhase SessionPhaseManager::phase(const std::string& code, uint32_t hhmmss, uint32_t secs_in_min) const
{
    if (!isTrading(code, hhmmss))
        return SessionPhase::CLOSED;

    bool night = _cfg && inNightCloseoutWindow(hhmmss, _cfg->night_close_time, _cfg->night_minutes_before);
    bool day = _cfg && inDayCloseoutWindow(hhmmss, _cfg->close_time, _cfg->closeout_minutes_before);
    if (night || day)
        return SessionPhase::CLOSEOUT_WINDOW;

    if (inSectionBreak(code, hhmmss, secs_in_min))
        return SessionPhase::SECTION_BREAK;

    return SessionPhase::CONTINUOUS;
}

} // namespace futu
