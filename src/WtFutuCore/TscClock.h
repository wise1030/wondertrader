/*!
 * \file TscClock.h
 * \brief rdtsc 低延迟时钟 (P0 测量埋点, 批次3)
 *
 * rdtsc ~6-8ns/次, 比 chrono::now (vDSO ~20-25ns) 低 3 倍,
 * 适合每 tick 多点的 tick-to-trade 直方图埋点。
 * init 期一次性 calibrate() 得到 ns/tick 系数, 运行期零校准开销。
 */
#pragma once

#include <cstdint>
#include <chrono>
#include <thread>
#include <x86intrin.h>

namespace futu
{

class TscClock
{
public:
    /// 原始 TSC 计数 (~6-8ns)
    static inline uint64_t now() { return __rdtsc(); }

    /// init 期一次性校准 (10ms sleep 采样), 幂等
    static void calibrate()
    {
        auto c0 = std::chrono::steady_clock::now();
        uint64_t t0 = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        uint64_t t1 = __rdtsc();
        auto c1 = std::chrono::steady_clock::now();
        double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count());
        if (t1 > t0)
            _ns_per_tick = ns / static_cast<double>(t1 - t0);
    }

    /// TSC 差值 → ns
    static inline uint64_t toNs(uint64_t ticks)
    {
        return static_cast<uint64_t>(static_cast<double>(ticks) * _ns_per_tick);
    }

private:
    static inline double _ns_per_tick = 0.4; // 未校准时的保守默认 (~2.5GHz)
};

} // namespace futu
