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
 *==========================================================================
 * THREADING CONTRACT (P1-7, 2026-06-18):
 *==========================================================================
 *   TradingState 设计为单线程访问:
 *     - 所有写入(setQuotingPhase / tryResumeFrom / enterCloseout /
 *       exitToQuoting / resumeFromRisk / reset / blockLong/blockShort/
 *       unblockLong/unblockShort) 必须在 UFT reader 线程上执行
 *       (WT 引擎对 on_tick, on_order, on_trade, on_session_*, on_entrust 等
 *        回调串行调度)。
 *     - 跨线程读取(如 AsyncArbitrageExecutor 异步任务读 isActive())
 *       仅限于查询单字节 enum 成员。x86/arm64 下对齐 uint8_t 读是原子的。
 *     - 若未来引入并发写场景,必须改为 std::atomic<uint8_t> 或加 mutex。
 *
 *   DEBUG 构建启用 _writer_tid 断言: 首次写记录线程 id, 后续写若来自不同
 *   线程立即 assert 失败, 提早暴露并发误用. Release 构建零开销.
 *==========================================================================
 *
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include <cstdint>

#ifndef NDEBUG
#include <cassert>
#include <thread>
#endif

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
//   - 非 H 子态的恢复统一走 tryResumeFrom(expected) (P1-6/U1, 2026-06-18):
//       * 必须显式声明"我在从哪个状态退出",避免高优先级期间被低优先级
//         else 分支误翻 NORMAL.
//       * 例如 HALT 期间 MARKET 退出分支不应该把 H 翻 N.
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
        _check_writer_thread();
        phase = MmPhase::CLOSEOUT;
    }

    /// 退出到做市报价阶段（夜盘平仓完成 / session reset）
    void exitToQuoting() {
        _check_writer_thread();
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
        _check_writer_thread();
        if (qphase == q) return true;  // idempotent
        if (!canTransitionQuoting(q)) return false;
        qphase = q;
        return true;
    }

    /// P1-6/U1: 从指定子态恢复到 NORMAL — 仅当 expected 与当前 qphase 匹配时生效
    ///
    /// 用途: 非 H 子态 (TOXICITY/MARKET/ERROR) 的退出统一入口.
    /// 防止"高优先级期间被低优先级 else 分支误翻 NORMAL"的跨态闪烁问题.
    /// 例如 HALT 期间 MARKET 的 shouldPause=false 分支不应将 qphase 翻 N.
    ///
    /// @param expected 期望的当前子态. qphase != expected 时 no-op 并返回 false.
    /// @return true=恢复成功(qphase 已变 NORMAL) false=当前态不匹配,跳过
    ///
    /// 注: H 退出仍走 resumeFromRisk(). 不要用 tryResumeFrom(RISK_HALTED).
    bool tryResumeFrom(QuotingPhase expected) {
        _check_writer_thread();
        if (qphase != expected) return false;
        qphase = QuotingPhase::NORMAL;
        return true;
    }

    /// 风控恢复（RISK_HALTED → NORMAL 的唯一合法路径）
    void resumeFromRisk() {
        _check_writer_thread();
        qphase = QuotingPhase::NORMAL;
    }

    //==========================================================================
    // 全量重置（session begin / 日切）
    //==========================================================================

    void reset() {
        _check_writer_thread();
        phase  = MmPhase::QUOTING;
        qphase = QuotingPhase::NORMAL;
        long_blocked  = false;
        short_blocked = false;
    }

    //==========================================================================
    // 方向级软禁
    //==========================================================================

    void blockLong()   { _check_writer_thread(); long_blocked  = true;  }
    void unblockLong() { _check_writer_thread(); long_blocked  = false; }
    void blockShort()  { _check_writer_thread(); short_blocked = true;  }
    void unblockShort(){ _check_writer_thread(); short_blocked = false; }

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

private:
    //==========================================================================
    // P1-7: DEBUG-only 线程契约校验
    //   首次写记录线程 id, 后续写若来自不同线程立即 assert 失败.
    //   Release 构建编译为空, 零开销.
    //==========================================================================
#ifndef NDEBUG
    mutable std::thread::id _writer_tid{};
    void _check_writer_thread() const {
        auto cur = std::this_thread::get_id();
        if (_writer_tid == std::thread::id{}) {
            _writer_tid = cur;
        } else {
            assert(_writer_tid == cur && "TradingState write from different thread!");
        }
    }
#else
    void _check_writer_thread() const {}
#endif
};

} // namespace futu
