/*!
 * \file HaltGate.h
 * \brief 报价门禁统一求值 (F-core P1-6 阶段 2, 2026-09-19)
 *
 * 阶段 1 已把 10 类停报/暂停原因镜像为原子位图 (`HaltReasons`, 影子模式)。
 * 阶段 2 把**报价门禁**从"多处各自判定"收敛为一处求值, 并支持三种模式:
 *
 *   | 模式 | 语义 | 用途 |
 *   |---|---|---|
 *   | `legacy` (默认) | 仅看既有标志（行为与切换前逐比特一致） | 影子期/一键回退 |
 *   | `union` | 既有标志 **或** 原因位图 | 影子期后首选的保守激活态（只可能"多停"，不会"少停"） |
 *   | `bits` | 仅看原因位图 | 长期目标态（阶段 4 删旧标志后） |
 *
 * **只用于"是否允许报价/报单"的门禁点**（当前 3 处：processQuoting / requoteAfterFill /
 * arb 复活门）。恢复、相位同步、订单结果标注等需要**精确 legacy 语义**的位置不得使用本类
 * （例如 `checkAndRecover` 后的 `!isTradingHalted()` 确认、`isTradingHalted()→setQuotingPhase(RISK_HALTED)`
 * 同步、CloseoutOrchestrator 的 `self_canceled` 归类）。
 *
 * 已知事实（本次审计）：`isQuotingPaused` 此前**没有任何报价门禁**（唯一读点在 RiskCoordinator
 * 的恢复判定），即 pauseQuoting 置位后报价路径并不读取它；`union`/`bits` 模式下该原因才会真正
 * 参与门禁 —— 这是从 legacy 切到 union 时**唯一新增的行为变化**，激活前需在影子期确认。
 *
 * §20.9 批 1（2026-11-12）: `blocked/statusLine` 改由调用方传入本策略实例位图
 * （`TradingState::haltReasons().raw()`）—— 位图已从进程级 static 实例化，HaltGate 不再持有位图。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "HaltReason.h"

namespace futu
{

enum class HaltGateMode : uint32_t
{
    LEGACY = 0, ///< 仅既有标志 (默认; 与切换前行为一致)
    UNION = 1,  ///< 既有标志 ∪ 原因位图 (保守方向)
    BITS = 2    ///< 仅原因位图 (长期目标态)
};

class HaltGate
{
public:
    static HaltGateMode mode() { return _mode.load(std::memory_order_relaxed); }
    static void setMode(HaltGateMode m) { _mode.store(m, std::memory_order_relaxed); }

    static const char* modeName(HaltGateMode m)
    {
        switch (m)
        {
        case HaltGateMode::LEGACY: return "legacy";
        case HaltGateMode::UNION: return "union";
        case HaltGateMode::BITS: return "bits";
        default: return "unknown";
        }
    }

    /// 解析配置串; 非法值返回 false 并保持当前模式 (调用方打 warn)
    static bool parseMode(const char* s, HaltGateMode& out)
    {
        if (s == nullptr)
            return false;
        const std::string v(s);
        if (v == "legacy")
        {
            out = HaltGateMode::LEGACY;
            return true;
        }
        if (v == "union")
        {
            out = HaltGateMode::UNION;
            return true;
        }
        if (v == "bits")
        {
            out = HaltGateMode::BITS;
            return true;
        }
        return false;
    }

    /// 门禁统一求值。
    /// @param legacy_blocked 该门禁点原本的判定（既有标志/相位算出的"禁止"）
    /// @param bits           本策略实例的原因位图 (`TradingState::haltReasons().raw()`;
    ///                       §20.9 批 1 起位图 per-strategy, 不再读进程级 static)
    /// @return 是否禁止（true = 禁止报价/报单）
    static bool blocked(bool legacy_blocked, uint64_t bits)
    {
        switch (mode())
        {
        case HaltGateMode::LEGACY: return legacy_blocked;
        case HaltGateMode::UNION: return legacy_blocked || bits != 0;
        case HaltGateMode::BITS: return bits != 0;
        default: return legacy_blocked;
        }
    }

    /// 供巡检/KPI: 当前位图与模式的人读串
    static std::string statusLine(uint64_t bits)
    {
        std::string s = "mode=";
        s += modeName(mode());
        s += " reasons=";
        s += HaltReasons::describe(bits);
        return s;
    }

private:
    static inline std::atomic<HaltGateMode> _mode{HaltGateMode::LEGACY};
};

} // namespace futu
