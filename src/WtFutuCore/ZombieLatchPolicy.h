/*!
 * \file ZombieLatchPolicy.h
 * \brief §20.8-B1/B3(2026-11-02): zombie 闩锁计时策略 (header-only, 可单测)
 *
 * 设计要点:
 *  - 计时原点用 steady(墙钟单调)毫秒, 不用 tick/exchange 时间 —— 开盘洪峰下
 *    MdSpi 积压使 tc.timestamp 滞后墙钟可达分钟级: 10-22 09:02 实证闩锁置位
 *    3 秒后 EXCEEDED 即报 held=136s (起点取了滞后时刻, held 被积压灌水)。
 *    steady 时钟下 held = 真实"本次连续持有"。
 *  - arm(was_halted): 解除后再次置位 = 新闩锁段 → 原点重置 + capped/escalated
 *    重新武装; 未解除期间同合约再升级 = 同段续期 → 保持原点 (B1 单调总时长
 *    语义, 2026-09-16, 防重探被新 zombie 反复续期后停报时长无上界)。
 *  - lift: 解除即清全部状态。
 *  - markCapped / markEscalated: 每闩锁段各一次。
 *
 * 线程约束: 仅 Md 线程触碰 (processAutoCancel 唯一调用点), 与既有
 * _zombie_latch_ms 一致, 无需加锁。
 */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace futu
{

class ZombieLatchPolicy
{
public:
    /// 闩锁置位。was_halted=本帧之前该合约已处闩锁态(同段续期); 否则新段重置原点。
    void arm(const std::string& code, bool was_halted, uint64_t steady_now_ms)
    {
        if (was_halted) {
            _since.emplace(code, steady_now_ms); // 同段续期: 保持原点 (B1 单调)
        } else {
            _since[code] = steady_now_ms;        // 新段: 重置原点 (§20.8-B1)
            _capped.erase(code);
            _escalated.erase(code);
        }
    }

    /// 闩锁解除: 清计时原点与一次性集合, 下次 arm 全新起算。
    void lift(const std::string& code)
    {
        _since.erase(code);
        _capped.erase(code);
        _escalated.erase(code);
    }

    /// 本次连续持有时长(ms, steady 域); 无记录或时钟异常回退 0。
    uint64_t heldMs(const std::string& code, uint64_t steady_now_ms) const
    {
        auto it = _since.find(code);
        if (it == _since.end() || steady_now_ms <= it->second)
            return 0;
        return steady_now_ms - it->second;
    }

    /// EXCEEDED 封顶告警: 每闩锁段一次, 首次返回 true。
    bool markCapped(const std::string& code) { return _capped.insert(code).second; }

    /// 长闩锁升级告警 (§20.8-B3): 每闩锁段一次, 首次返回 true。
    bool markEscalated(const std::string& code) { return _escalated.insert(code).second; }

    std::size_t armedCount() const { return _since.size(); }

private:
    std::unordered_map<std::string, uint64_t> _since;   ///< steady_clock 原点 (ms)
    std::unordered_set<std::string> _capped;            ///< 本段已发 EXCEEDED
    std::unordered_set<std::string> _escalated;         ///< 本段已发长闩锁升级
};

} // namespace futu
