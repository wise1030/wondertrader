/*!
 * \file TscClock.h
 * \brief rdtsc 低延迟时钟 (P0 测量埋点, 批次3)
 *
 * rdtsc ~6-8ns/次, 比 chrono::now (vDSO ~20-25ns) 低 3 倍,
 * 适合每 tick 多点的 tick-to-trade 直方图埋点。
 * init 期一次性 calibrate() 得到 ns/tick 系数; V8-R4 起支持
 * invariant-TSC 探测、lfence 序列化与低频重校准。
 */
#pragma once

#include <cstdint>
#include <chrono>
#include <thread>
#include <x86intrin.h>
#include <cpuid.h>
#include "../WTSTools/WTSLogger.h"

namespace futu
{

class TscClock
{
public:
    /// 原始 TSC 计数 (~6-8ns)
    /// V8-R4: lfence 序列化 — 裸 __rdtsc() 可被 CPU 乱序提前执行,
    /// 测量区间两端漂移数十 ns, 埋点直方图不可信。
    static inline uint64_t now()
    {
        _mm_lfence();
        return __rdtsc();
    }

    /// init 期一次性校准 (10ms sleep 采样), 幂等。
    /// V8-R4: invariant-TSC 探测 (cpuid 0x80000007:EDX[8]) — 非 invariant
    /// (变频/跨核频率漂移) 时显式失败, 不再静默使用 0.4 兜底。
    /// @return false = 未校准, 此时 toNs 精度不可信
    static bool calibrate()
    {
        if (!detectInvariantTsc()) {
            WTSLogger::error("[TscClock] non-invariant TSC detected, tick-to-trade latency histogram UNRELIABLE "
                             "(ns/tick 保持兜底 {:.3f})",
                             _ns_per_tick);
            _calibrated = false;
            return false;
        }
        doCalibrate();
        _calibrated = true;
        _last_calib_ms = steadyNowMs();
        return true;
    }

    /// 低频重校准 (V8-R4: 长时间运行的频率漂移兜底)。
    /// 仅在非热路径调用 (内部 10ms sleep); 距上次校准 < interval_ms 时跳过。
    static void maybeRecalibrate(uint64_t interval_ms = 60000)
    {
        if (!_calibrated)
            return;
        uint64_t now_ms = steadyNowMs();
        if (now_ms - _last_calib_ms < interval_ms)
            return;
        doCalibrate();
        _last_calib_ms = now_ms;
    }

    /// TSC 差值 → ns
    static inline uint64_t toNs(uint64_t ticks)
    {
        return static_cast<uint64_t>(static_cast<double>(ticks) * _ns_per_tick);
    }

    static inline bool calibrated() { return _calibrated; }

private:
    static bool detectInvariantTsc()
    {
        uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
        // 需先确认 extended leaf 0x80000007 可用
        if (!__get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx) || eax < 0x80000007)
            return false;
        if (!__get_cpuid(0x80000007, &eax, &ebx, &ecx, &edx))
            return false;
        return (edx & (1u << 8)) != 0;
    }

    static void doCalibrate()
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

    static uint64_t steadyNowMs()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    static inline double _ns_per_tick = 0.4; // 未校准时的保守默认 (~2.5GHz)
    static inline bool _calibrated = false;
    static inline uint64_t _last_calib_ms = 0;
};

} // namespace futu
