#pragma once

#include <atomic>
#include <thread>

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace futu
{

struct SpinLockGuard
{
    std::atomic_flag& flag;
    SpinLockGuard(std::atomic_flag& f) : flag(f)
    {
        while (flag.test_and_set(std::memory_order_acquire)) {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }
    }
    ~SpinLockGuard()
    {
        flag.clear(std::memory_order_release);
    }
};

//==========================================================================
// RecursiveSpinLock (v7.6 阶段2): 可重入自旋锁
//   用于公开方法间存在嵌套调用的结构 (如 UnifiedOrderTracker::
//   checkAutoCancel → untrackOrder)。owner tid + 计数, 同线程重入
//   不再抢 flag (无竞争路径 ~2 次原子操作)。
//==========================================================================
struct RecursiveSpinLock
{
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    std::atomic<std::thread::id> owner{std::thread::id{}};
    // count 非原子 — 正确性依赖不变式: 仅 owner 线程可读写
    // (owner==tid 判定本身意味着已持 flag; 非 owner 线程不会触达)。
    // 不得将此锁用于跨线程移交所有权 (如 lock in A / unlock in B)。
    uint32_t count = 0;

    void lock()
    {
        const std::thread::id tid = std::this_thread::get_id();
        if (owner.load(std::memory_order_relaxed) == tid) {
            ++count;
            return;
        }
        while (flag.test_and_set(std::memory_order_acquire)) {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }
        owner.store(tid, std::memory_order_relaxed);
        count = 1;
    }

    void unlock()
    {
        if (--count == 0) {
            owner.store(std::thread::id{}, std::memory_order_relaxed);
            flag.clear(std::memory_order_release);
        }
    }
};

struct RecursiveSpinGuard
{
    RecursiveSpinLock& lk;
    RecursiveSpinGuard(RecursiveSpinLock& l) : lk(l) { lk.lock(); }
    ~RecursiveSpinGuard() { lk.unlock(); }
};

} // namespace futu
