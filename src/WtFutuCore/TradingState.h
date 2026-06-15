/*!
 * \file TradingState.h
 * \brief Unified Trading State — Hierarchical State Machine (HSM)
 *
 * P1-1 重构 (2026-06): 两层分层状态机，替代旧扁平 bool 标志位
 *
 * 业务语义分层:
 *   顶层 MmPhase — 做市报价 vs 收盘平仓对冲（两个业务阶段）
 *   QUOTING 子状态 — 毒性/市场/错误/风控暂停
 *   CLOSEOUT 子状态 — 由 RiskMonitor 的 CloseoutSub 管理（TradingState 不跟踪细节）
 *   方向级软禁 long_blocked/short_blocked — 正交于两个阶段
 *
 * 线程安全约束 (P1-7):
 *   WT UFT 回调在 reader 线程串行调用，无并发。不加 std::atomic。
 *   若未来引入多线程，需将 MmPhase/QuotingPhase 改为 atomic 或加锁。
 *
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include <cstdint>

namespace futu {

//==========================================================================
// 顶层：业务阶段
//==========================================================================
enum class MmPhase : uint8_t {
    QUOTING,    ///< 做市报价阶段
    CLOSEOUT,   ///< 收盘平仓对冲阶段
};

//==========================================================================
// QUOTING 子状态
//==========================================================================
enum class QuotingPhase : uint8_t {
    NORMAL,         ///< 正常报价
    TOXICITY,       ///< 毒性流暂停（VPIN/OFI 等信号触发，定时恢复）
    MARKET,         ///< 极端波动暂停（vol tier EXTREME）
    ERROR,          ///< 下单错误暂停（指数退避恢复）
    RISK_HALTED,    ///< 风控硬触发（持仓超限/Delta爆炸），需显式 resumeFromRisk
};

//==========================================================================
// 统一交易状态 — Single Source of Truth
//==========================================================================
//
// 设计要点:
//   - MmPhase 只有两个值：做市 vs 收盘平仓。转移只发生在 closeout 触发/完成。
//   - QuotingPhase 是 QUOTING 内的子状态。互相可抢占（高优先级覆盖）。
//   - RISK_HALTED → NORMAL 只能通过 resumeFromRisk()（带转移校验）。
//   - CLOSEOUT 子状态由 RiskMonitor 的 CloseoutSub 管理，TradingState 不跟踪细节。
//   - long_blocked/short_blocked 是方向级软禁，与 phase/qphase 正交。
//   - 不再需要 syncFromRiskMonitor：各模块管自己的域，strategy 是编排者。
//
struct TradingState {
    MmPhase      phase  = MmPhase::QUOTING;
    QuotingPhase qphase = QuotingPhase::NORMAL;

    // 方向级软禁（正交，两阶段都适用）
    bool long_blocked  = false;
    bool short_blocked = false;

    //==========================================================================
    // 查询接口
    //==========================================================================

    /// 能否报价（做市阶段 + NORMAL 子状态）
    bool canQuote() const {
        return phase == MmPhase::QUOTING && qphase == QuotingPhase::NORMAL;
    }

    /// 能否买入
    bool canBuy() const {
        return canQuote() && !long_blocked;
    }

    /// 能否卖出
    bool canSell() const {
        return canQuote() && !short_blocked;
    }

    /// 是否活跃（做市阶段 且 非风控暂停）
    /// 语义映射旧 isActive(): NORMAL 或 TOXICITY 时为 true
    bool isActive() const {
        return phase == MmPhase::QUOTING
            && qphase != QuotingPhase::RISK_HALTED
            && qphase != QuotingPhase::ERROR
            && qphase != QuotingPhase::MARKET;
    }

    /// 收盘平仓阶段是否激活
    bool isCloseoutActive() const {
        return phase == MmPhase::CLOSEOUT;
    }

    //==========================================================================
    // 顶层转移
    //==========================================================================

    /// 进入收盘平仓阶段
    void enterCloseout() {
        phase = MmPhase::CLOSEOUT;
    }

    /// 退出到做市报价阶段（夜盘平仓完成 / session reset）
    void exitToQuoting() {
        phase  = MmPhase::QUOTING;
        qphase = QuotingPhase::NORMAL;
    }

    //==========================================================================
    // QUOTING 子状态转移
    //==========================================================================

    /// QuotingPhase 转移校验
    /// RISK_HALTED → NORMAL 仅允许通过 resumeFromRisk()
    /// 其他状态间自由转移（互相抢占）
    bool canTransitionQuoting(QuotingPhase next) const {
        if (qphase == QuotingPhase::RISK_HALTED)
            return next == QuotingPhase::NORMAL;
        return true;
    }

    /// 设置 QUOTING 子状态（抢占式，自动校验）
    /// @return true=转移成功(含同态幂等) false=被校验拒绝（RISK_HALTED 不可直接抢占）
    bool setQuotingPhase(QuotingPhase q) {
        if (qphase == q) return true;  // idempotent
        if (!canTransitionQuoting(q)) return false;
        qphase = q;
        return true;
    }

    /// 风控恢复（RISK_HALTED → NORMAL 的唯一合法路径）
    void resumeFromRisk() {
        qphase = QuotingPhase::NORMAL;
    }

    //==========================================================================
    // 全量重置（session begin / 日切）
    //==========================================================================

    void reset() {
        phase  = MmPhase::QUOTING;
        qphase = QuotingPhase::NORMAL;
        long_blocked  = false;
        short_blocked = false;
    }

    //==========================================================================
    // 方向级软禁
    //==========================================================================

    void blockLong()   { long_blocked  = true;  }
    void unblockLong() { long_blocked  = false; }
    void blockShort()  { short_blocked = true;  }
    void unblockShort(){ short_blocked = false; }

    //==========================================================================
    // 日志/调试
    //==========================================================================

    /// 当前状态字符串
    const char* getPhaseStr() const {
        if (phase == MmPhase::CLOSEOUT) return "CLOSEOUT";
        switch (qphase) {
            case QuotingPhase::NORMAL:       return "NORMAL";
            case QuotingPhase::TOXICITY:     return "TOXICITY";
            case QuotingPhase::MARKET:       return "MARKET";
            case QuotingPhase::ERROR:        return "ERROR";
            case QuotingPhase::RISK_HALTED:  return "RISK_HALTED";
        }
        return "UNKNOWN";
    }
};

} // namespace futu
