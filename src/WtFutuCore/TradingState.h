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
 * THREADING CONTRACT (P1-7, 2026-06-18; v7.4 修正, 2026-08-03; v7.6 原子化):
 *==========================================================================
 *   实盘线程真相 (框架源码核实): WT UFT 引擎【不】串行调度回调 —
 *     - on_tick/on_transaction/on_order_*: CTP MdSpi 线程
 *     - on_trade/on_order/on_entrust/on_channel_*: CTP TdSpi 线程
 *     - on_session_end(盘中分钟触发): RtTicker 定时线程
 *
 *   v7.6 (并发精细化阶段1): 全字段 std::atomic, 状态转移用 CAS:
 *     - tryResumeFrom → compare_exchange (天然"仅从 expected 退出"语义)
 *     - setQuotingPhase → read-check-CAS 循环 (canTransition 校验不丢失)
 *     - 读侧 (canQuote/isActive/...) 隐式 load, 零锁
 *   多字段复合操作 (reset/exitToQuoting) 是逐字段 store, 存在 ns 级
 *   混合视图窗口 — 仅在 session begin/end 安静期调用, 可接受。
 *
 *   过渡期策略层 _cb_mtx 大锁仍在 (FUTU_CALLBACK_LOCK=1 默认),
 *   本类的原子化保证大锁移除后单字段读写/转移依然安全。
 *
 *   DEBUG 构建 _writer_tid 断言: 原子化后单写者契约已废弃,
 *   setExternalLocking(true) 恒停用 (策略 on_init 调用)。
 *==========================================================================
 *
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include <cstdint>
#include <atomic>

#ifndef NDEBUG
#include <cassert>
#include <thread>
#endif

namespace futu
{

//==========================================================================
// 顶层：业务阶段
//==========================================================================
enum class MmPhase : uint8_t
{
    QUOTING,  ///< 做市报价阶段
    CLOSEOUT, ///< 收盘平仓对冲阶段
};

//==========================================================================
// QUOTING 子状态
//==========================================================================
enum class QuotingPhase : uint8_t
{
    NORMAL,      ///< 正常报价
    TOXICITY,    ///< 毒性流暂停（VPIN/OFI 等信号触发，定时恢复）
    MARKET,      ///< 极端波动暂停（vol tier EXTREME）
    ERROR,       ///< 下单错误暂停（指数退避恢复）
    RISK_HALTED, ///< 风控硬触发（持仓超限/Delta爆炸），需显式 resumeFromRisk
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
struct TradingState
{
    // v7.6: 全字段原子 (MdSpi/TdSpi 双线程读写, 见文件头 THREADING CONTRACT)
    std::atomic<MmPhase> phase{MmPhase::QUOTING};
    std::atomic<QuotingPhase> qphase{QuotingPhase::NORMAL};

    // 方向级软禁（正交，两阶段都适用）
    std::atomic<bool> long_blocked{false};
    std::atomic<bool> short_blocked{false};

    //==========================================================================
    // 查询接口
    //==========================================================================

    /// 能否报价（做市阶段 + NORMAL 子状态）
    bool canQuote() const
    {
        return phase.load(std::memory_order_acquire) == MmPhase::QUOTING &&
               qphase.load(std::memory_order_acquire) == QuotingPhase::NORMAL;
    }

    /// 能否买入
    bool canBuy() const { return canQuote() && !long_blocked.load(std::memory_order_acquire); }

    /// 能否卖出
    bool canSell() const { return canQuote() && !short_blocked.load(std::memory_order_acquire); }

    /// 是否活跃（做市阶段 且 非风控暂停）
    /// 语义映射旧 isActive(): NORMAL 或 TOXICITY 时为 true
    bool isActive() const
    {
        QuotingPhase q = qphase.load(std::memory_order_acquire);
        return phase.load(std::memory_order_acquire) == MmPhase::QUOTING && q != QuotingPhase::RISK_HALTED &&
               q != QuotingPhase::ERROR && q != QuotingPhase::MARKET;
    }

    /// 收盘平仓阶段是否激活
    bool isCloseoutActive() const { return phase.load(std::memory_order_acquire) == MmPhase::CLOSEOUT; }

    //==========================================================================
    // 顶层转移
    //==========================================================================

    /// 进入收盘平仓阶段
    void enterCloseout()
    {
        _check_writer_thread();
        phase.store(MmPhase::CLOSEOUT, std::memory_order_release);
    }

    /// 退出到做市报价阶段（夜盘平仓完成 / session reset）
    void exitToQuoting()
    {
        _check_writer_thread();
        phase.store(MmPhase::QUOTING, std::memory_order_release);
        qphase.store(QuotingPhase::NORMAL, std::memory_order_release);
    }

    //==========================================================================
    // QUOTING 子状态转移
    //==========================================================================

    /// QuotingPhase 转移校验
    /// RISK_HALTED → NORMAL 仅允许通过 resumeFromRisk()
    /// 其他状态间自由转移（互相抢占）
    bool canTransitionQuoting(QuotingPhase next) const
    {
        if (qphase.load(std::memory_order_acquire) == QuotingPhase::RISK_HALTED)
            return next == QuotingPhase::NORMAL;
        return true;
    }

    /// 设置 QUOTING 子状态（抢占式，自动校验）
    /// v7.6: read-check-CAS 循环 — canTransition 校验与写入原子化,
    ///       并发 setQuotingPhase 不会绕过 RISK_HALTED 守卫。
    /// @return true=转移成功(含同态幂等) false=被校验拒绝（RISK_HALTED 不可直接抢占）
    bool setQuotingPhase(QuotingPhase q)
    {
        _check_writer_thread();
        QuotingPhase cur = qphase.load(std::memory_order_acquire);
        for (;;) {
            if (cur == q)
                return true; // idempotent
            // v7.7 C2: 复用 canTransitionQuoting (单一校验逻辑, 防两处漂移)
            if (cur == QuotingPhase::RISK_HALTED && !canTransitionQuoting(q))
                return false; // RISK_HALTED 不可直接抢占
            if (qphase.compare_exchange_weak(cur, q, std::memory_order_acq_rel, std::memory_order_acquire))
                return true;
        }
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
    bool tryResumeFrom(QuotingPhase expected)
    {
        _check_writer_thread();
        // v7.6: CAS 天然等价 "qphase == expected 才翻 NORMAL" 的原子判定
        return qphase.compare_exchange_strong(
            expected, QuotingPhase::NORMAL, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    /// 风控恢复（RISK_HALTED → NORMAL 的唯一合法路径）
    void resumeFromRisk()
    {
        _check_writer_thread();
        qphase.store(QuotingPhase::NORMAL, std::memory_order_release);
    }

    //==========================================================================
    // 全量重置（session begin / 日切）
    //==========================================================================

    void reset()
    {
        _check_writer_thread();
        phase.store(MmPhase::QUOTING, std::memory_order_release);
        qphase.store(QuotingPhase::NORMAL, std::memory_order_release);
        long_blocked.store(false, std::memory_order_release);
        short_blocked.store(false, std::memory_order_release);
    }

    //==========================================================================
    // 方向级软禁
    //==========================================================================

    void blockLong()
    {
        _check_writer_thread();
        long_blocked.store(true, std::memory_order_release);
    }
    void unblockLong()
    {
        _check_writer_thread();
        long_blocked.store(false, std::memory_order_release);
    }
    void blockShort()
    {
        _check_writer_thread();
        short_blocked.store(true, std::memory_order_release);
    }
    void unblockShort()
    {
        _check_writer_thread();
        short_blocked.store(false, std::memory_order_release);
    }

    //==========================================================================
    // 日志/调试
    //==========================================================================

    /// 当前状态字符串
    const char* getPhaseStr() const
    {
        if (phase.load(std::memory_order_acquire) == MmPhase::CLOSEOUT)
            return "CLOSEOUT";
        switch (qphase.load(std::memory_order_acquire)) {
        case QuotingPhase::NORMAL:
            return "NORMAL";
        case QuotingPhase::TOXICITY:
            return "TOXICITY";
        case QuotingPhase::MARKET:
            return "MARKET";
        case QuotingPhase::ERROR:
            return "ERROR";
        case QuotingPhase::RISK_HALTED:
            return "RISK_HALTED";
        }
        return "UNKNOWN";
    }

private:
    //==========================================================================
    // P1-7: DEBUG-only 线程契约校验
    //   v7.6 原子化后单写者契约已废弃; 保留仅为过渡期观察,
    //   setExternalLocking(true) 恒停用 (策略 on_init 调用)。
    //==========================================================================
#ifndef NDEBUG
    mutable std::thread::id _writer_tid{};
    static inline std::atomic<bool> s_external_locking{false};

public:
    static void setExternalLocking(bool on)
    {
        s_external_locking.store(on);
    }

private:
    void _check_writer_thread() const
    {
        if (s_external_locking.load(std::memory_order_relaxed))
            return;
        auto cur = std::this_thread::get_id();
        if (_writer_tid == std::thread::id{}) {
            _writer_tid = cur;
        } else {
            assert(_writer_tid == cur && "TradingState write from different thread!");
        }
    }
#else
public:
    static void setExternalLocking(bool) {}

private:
    void _check_writer_thread() const {}
#endif
};

} // namespace futu
