/*!
 * \file SideFillBreaker.h
 * \brief 同侧连续成交熔断器（风控层，按合约独立计数）
 *
 * 背景: 2026-08-12 夜盘 21:51:54-21:52:02，ao2610 上 20 笔同向买单在 ~6 秒内
 * 全部成交（"Closing old short" 买入循环），策略簿记 delta 11→49、净仓 2→42，
 * 触发 HALT_QUOTING。skew/retreat 属策略层软控制，拦不住主动买入循环；
 * 本熔断器是风控层硬闸门：某合约同侧连续成交达到阈值 → 暂停该合约报价
 * （调用方撤单 + 拒挂），到期自动恢复。
 *
 * 语义（与方案确认一致）:
 *   - 按合约独立维护状态: ao2609 与 ao2610 的计数/窗口/暂停互不影响;
 *   - 反侧成交打断当前同侧连续序列（连续 = 严格同向）;
 *   - 窗口（windowMs）内未达阈值 → 计数随窗口过期清零;
 *   - 触发后计数清零，暂停 pauseMs，到期自动恢复（“过后再恢复”）;
 *   - 暂停期内不累计计数，避免恢复后立即再触发。
 *
 * 线程安全: onFill（TD 成交回调线程）与 isPaused（MD 报价线程）跨线程调用，
 * 内部用互斥锁保护状态表。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include "SpinLockGuard.h"

namespace futu
{

/// 熔断器配置（默认值: 5 笔 / 3s 窗口 / 5s 暂停）
struct SideFillBreakerConfig
{
    uint32_t max_consecutive_same_side = 5; ///< 同侧连续成交触发阈值 (0=禁用)
    uint32_t window_ms = 3000;              ///< 连续计数窗口 (ms)
    uint32_t pause_ms = 5000;               ///< 触发后暂停该合约报价时长 (ms)
};

/// 单合约熔断状态（每侧独立计数）
struct SideBreakerState
{
    uint32_t bid_count = 0;       ///< 连续买单成交计数
    uint32_t ask_count = 0;       ///< 连续卖单成交计数
    uint64_t bid_window_start_ms = 0;
    uint64_t ask_window_start_ms = 0;
    uint64_t pause_until_ms = 0;  ///< 暂停到期时间 (0 = 未暂停)
};

/// 同侧连续成交熔断器：按合约独立计数，触发后暂停该合约报价，到期自动恢复。
class SideFillBreaker
{
public:
    explicit SideFillBreaker(const SideFillBreakerConfig& cfg = SideFillBreakerConfig{}) : _cfg(cfg) {}

    void setConfig(const SideFillBreakerConfig& cfg) { _cfg = cfg; }
    const SideFillBreakerConfig& config() const { return _cfg; }

    /// 记录一笔成交；返回 true 表示本次触发了熔断（调用方应立即撤单并暂停报价）。
    /// adds_inventory=false (减仓方向的成交: 持多时卖出/持空时买入) 只打断反侧
    /// 连续序列, 不累计熔断 — 降低库存的成交是被鼓励的逆向交易, 熔断它会把
    /// 策略在回补段拽出场, 形成"卖出→回补被熔断→恢复后再被卖出"棘轮
    /// (2026-08-17 ao 实盘: 10 次买方熔断中多数是回补空单的正确交易)。
    bool onFill(const std::string& code, bool is_buy, uint64_t now_ms, bool adds_inventory = true)
    {
        SpinLockGuard _g(_flag);
        if (_cfg.max_consecutive_same_side == 0 || _cfg.pause_ms == 0)
            return false;

        auto& st = _states[code];

        // 暂停到期自动恢复（清零计数与窗口）
        if (st.pause_until_ms > 0) {
            if (now_ms < st.pause_until_ms)
                return false; // 暂停期内不累计
            st = SideBreakerState{};
        }

        // 反侧成交打断当前同侧连续序列（严格同向才算连续）
        if (is_buy) {
            st.ask_count = 0;
            st.ask_window_start_ms = 0;
        } else {
            st.bid_count = 0;
            st.bid_window_start_ms = 0;
        }

        // 减仓方向的成交不累计熔断 (反侧打断逻辑仍执行)
        if (!adds_inventory)
            return false;

        uint32_t& count = is_buy ? st.bid_count : st.ask_count;
        uint64_t& window_start = is_buy ? st.bid_window_start_ms : st.ask_window_start_ms;
        if (window_start == 0 || now_ms < window_start || now_ms - window_start > _cfg.window_ms) {
            count = 0;
            window_start = now_ms;
        }
        ++count;

        if (count >= _cfg.max_consecutive_same_side) {
            st.pause_until_ms = now_ms + _cfg.pause_ms;
            st.bid_count = 0;
            st.ask_count = 0;
            st.bid_window_start_ms = 0;
            st.ask_window_start_ms = 0;
            return true;
        }
        return false;
    }

    /// 该合约当前是否处于熔断暂停（合约级双边暂停，到期自动恢复）
    bool isPaused(const std::string& code, uint64_t now_ms) const
    {
        SpinLockGuard _g(_flag);
        auto it = _states.find(code);
        return it != _states.end() && it->second.pause_until_ms > 0 && now_ms < it->second.pause_until_ms;
    }

    /// 清空全部状态（策略重启/交易日切换时调用）
    void clear()
    {
        SpinLockGuard _g(_flag);
        _states.clear();
    }

private:
    SideFillBreakerConfig _cfg;
    mutable std::atomic_flag _flag = ATOMIC_FLAG_INIT;
    std::unordered_map<std::string, SideBreakerState> _states;
};

} // namespace futu
