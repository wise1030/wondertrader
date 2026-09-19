/*!
 * \file HaltReason.h
 * \brief 停报/暂停**原因集** (F-core P1-6 阶段 1, 2026-09-19)
 *
 * 背景（19.8 状态清单）: "能不能报价"当前由 4 组状态分别回答 —— FutuRiskMonitor
 *   (`_trading_halted/_quoting_paused/_long_blocked/_short_blocked/_zombie_halts`)、
 *   TradingState(`phase/qphase/block`)、RiskCoordinator(`_rate_breach_since_ms`)、
 *   Coordinator(zombie 闩锁/日志态)。事故排查需要同时读 3–4 处才能判断"谁在停报"。
 *
 * 裁决 A1/B（2026-09-19）:
 *   - P1-6 取代 WS-C（C10 事件通道并入）;
 *   - 门禁层 = **原子位图**（fetch_or/fetch_and, 无锁、多线程安全、读侧一次 load）,
 *     不引入新锁; 低频元数据(since/ttl/source)留待阶段 2 走 SPSC ring → tick 域唯一 writer。
 *
 * **阶段 1 为影子模式**: 既有标志仍是唯一门禁来源; 本组件只镜像 + 一致性对比, 行为零变更。
 *   不一致计数入 5min KPI (`halt_shadow=`), 影子期 ≥3 交易日零不一致才进入阶段 2(读点切换)。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace futu
{

/// 停报/暂停原因 (位序号; 与既有标志一一对应)
enum HaltReason : uint32_t
{
    HR_RISK_HALT = 0,    ///< FutuRiskMonitor::_trading_halted (含不可逆)
    HR_QUOTING_PAUSED,   ///< FutuRiskMonitor::_quoting_paused
    HR_LONG_BLOCKED,     ///< 方向软禁 (long)
    HR_SHORT_BLOCKED,    ///< 方向软禁 (short)
    HR_ZOMBIE_LATCH,     ///< 任一合约 zombie 闩锁
    HR_QPHASE_RISK,      ///< TradingState qphase == RISK_HALTED
    HR_QPHASE_ERROR,     ///< qphase == ERROR (下单错误退避自探)
    HR_QPHASE_TOXICITY,  ///< qphase == TOXICITY
    HR_QPHASE_MARKET,    ///< qphase == MARKET (极端波动)
    HR_RATE_BREACH,      ///< RiskCoordinator 速率违约窗口
    HR_COUNT
};

class HaltReasons
{
public:
    static constexpr uint32_t count() { return static_cast<uint32_t>(HR_COUNT); }
    static uint64_t bitOf(HaltReason r) { return 1ull << static_cast<uint32_t>(r); }
    static uint64_t maskAll() { return (HR_COUNT >= 64) ? ~0ull : ((1ull << HR_COUNT) - 1ull); }

    static const char* name(HaltReason r)
    {
        switch (r)
        {
        case HR_RISK_HALT: return "RISK_HALT";
        case HR_QUOTING_PAUSED: return "QUOTING_PAUSED";
        case HR_LONG_BLOCKED: return "LONG_BLOCKED";
        case HR_SHORT_BLOCKED: return "SHORT_BLOCKED";
        case HR_ZOMBIE_LATCH: return "ZOMBIE_LATCH";
        case HR_QPHASE_RISK: return "QPHASE_RISK";
        case HR_QPHASE_ERROR: return "QPHASE_ERROR";
        case HR_QPHASE_TOXICITY: return "QPHASE_TOXICITY";
        case HR_QPHASE_MARKET: return "QPHASE_MARKET";
        case HR_RATE_BREACH: return "RATE_BREACH";
        default: return "UNKNOWN";
        }
    }

    /// 置位/清位 —— 无锁 (单条 fetch_or / fetch_and), 任意线程可调用
    static void raise(HaltReason r) { _bits.fetch_or(bitOf(r), std::memory_order_acq_rel); }
    static void clear(HaltReason r) { _bits.fetch_and(~bitOf(r), std::memory_order_acq_rel); }
    static void clearAll() { _bits.store(0, std::memory_order_release); }

    static uint64_t raw() { return _bits.load(std::memory_order_acquire); }
    static bool blocked() { return raw() != 0; }
    static bool has(HaltReason r) { return (raw() & bitOf(r)) != 0; }

    /// "A|B|C" 形式 (空位图 → "NONE"), 供日志/巡检
    static std::string describe(uint64_t bits)
    {
        if (bits == 0)
            return "NONE";
        std::string out;
        for (uint32_t i = 0; i < count(); ++i)
        {
            if (!(bits & (1ull << i)))
                continue;
            if (!out.empty())
                out += "|";
            out += name(static_cast<HaltReason>(i));
        }
        return out;
    }

private:
    static inline std::atomic<uint64_t> _bits{0};
};

} // namespace futu
