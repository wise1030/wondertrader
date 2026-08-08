/*!
 * \file OrderApiGuard.h
 * \brief v7.6 阶段3: 下单 API 互斥 (L2)
 *
 * 框架 UftStraContext 内部容器 (_orders 等) 非线程安全, 实盘
 * MdSpi (quoter/takerReduce/closeout 发单) 与 TdSpi (requoteAfterFill/
 * AUTO REDUCE/残腿对冲 发单) 并发 stra_buy/sell/cancel 是框架级竞态
 * (不可越界修框架)。所有 stra_* 下单调用统一经 orderApiCall 包裹,
 * 临界区 = 单次框架调用 (亚 μs), 两线程同时发单才竞争。
 *
 * 锁序 (单向, 不可逆):
 *   quoter._lock → tracker._lock          (refreshQuotes 内 getOrderInfoCopy)
 *   router._lock → tracker._lock          (submit* 内 checkSelfTrade)
 *   portfolio._lock → arb._pair_states_spin (checkOvershootSignFlip 回调)
 *   结构锁 (quoter/router/orch/coordinator) → orderApiMutex
 *   orderApiMutex 内不得再取任何结构锁 (stra_* 同步回执不进策略回调)。
 *   已验证无环: refreshPositionsFromPortfolio 顺序获取 (portfolio 锁即取
 *   即放, 再取 arb spin, 不同时持有); arb 锁内不调 portfolio。
 *
 * recursive: 防御回调内嵌套发单路径。
 */
#pragma once

#include <mutex>

namespace futu
{

inline std::recursive_mutex& orderApiMutex()
{
    static std::recursive_mutex m;
    return m;
}

/// 锁内执行一次 stra_* 调用
template <typename F>
inline auto orderApiCall(F&& fn) -> decltype(fn())
{
    std::lock_guard<std::recursive_mutex> g(orderApiMutex());
    return fn();
}

} // namespace futu
