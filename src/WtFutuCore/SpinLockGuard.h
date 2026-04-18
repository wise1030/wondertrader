#pragma once

#include <atomic>
#include <thread>

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace futu {

struct SpinLockGuard {
    std::atomic_flag& flag;
    SpinLockGuard(std::atomic_flag& f) : flag(f) {
        while (flag.test_and_set(std::memory_order_acquire)) {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }
    }
    ~SpinLockGuard() {
        flag.clear(std::memory_order_release);
    }
};

} // namespace futu
