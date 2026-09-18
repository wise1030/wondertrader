/*!
 * \file HotLogGate.h
 * \brief 热路径日志闸门 (F-core P0-B, 2026-09-18)
 *
 * 背景: 2026-09-17 单日 Runner 日志 342,544 行 / 47MB, 其中 debug 级别占 86.6%;
 *   热路径 `[QUOTE]` 87.5k + `[STICKY]` 74.7k + `[REQUOTE]` 7.4k + `[SIGNAL_DECOMP]` 1.8k
 *   + `[TOXIC]` 1.8k ≈ 166k 行/日。每行都要 fmt::format 10+ 字段并同步落盘, 属每 tick 的
 *   固定开销 (实测进程 79% 时间在内核态, 写日志是主要来源之一)。
 *
 * 设计:
 *   - 全局静态开关 (启动时由配置注入) + thread_local 计数采样, 无新锁、无跨线程竞争。
 *   - 仅决定"是否落日志", 不改变任何交易/风控语义; 默认 enabled=true, sampleEvery=10
 *     (即保留 1/10 样本, 兼顾日常可观测与开销)。
 *   - hit() 内部维护 emitted/skipped 计数, 供 5min 巡检 KPI 行输出。
 *   - 运行时切换: 调用 configure() 即可 (本仓由 config.yaml 的 logging.hotQuote* 启动注入)。
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace futu
{

class HotLogGate
{
public:
    /// 配置注入 (启动时调用一次; 亦可在运行期切换)
    static void configure(bool enabled, uint32_t sample_every)
    {
        _enabled.store(enabled, std::memory_order_relaxed);
        _sample_every.store(sample_every == 0 ? 1u : sample_every, std::memory_order_relaxed);
    }

    static bool enabled() { return _enabled.load(std::memory_order_relaxed); }
    static uint32_t sampleEvery() { return _sample_every.load(std::memory_order_relaxed); }

    /// 本次是否输出。采样按 thread_local 计数, 避免多线程争抢同一原子。
    /// 同时维护 emitted/skipped 统计。
    static bool hit()
    {
        if (!_enabled.load(std::memory_order_relaxed))
        {
            _skipped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const uint32_t n = _sample_every.load(std::memory_order_relaxed);
        if (n <= 1)
        {
            _emitted.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        if (++_local_counter >= n)
        {
            _local_counter = 0;
            _emitted.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        _skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    static uint64_t emitted() { return _emitted.load(std::memory_order_relaxed); }
    static uint64_t skipped() { return _skipped.load(std::memory_order_relaxed); }

private:
    static inline std::atomic<bool> _enabled{true};
    static inline std::atomic<uint32_t> _sample_every{10};
    static inline std::atomic<uint64_t> _emitted{0};
    static inline std::atomic<uint64_t> _skipped{0};
    static inline thread_local uint32_t _local_counter{0};
};

} // namespace futu
