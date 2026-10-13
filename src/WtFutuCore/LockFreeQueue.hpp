/*!
 * \file LockFreeQueue.hpp
 * \brief Lock-Free Single-Producer Single-Consumer Queue
 * 
 * A high-performance lock-free queue for passing data between threads.
 * Uses a ring buffer structure with atomic indices for synchronization.
 * 
 * Features:
 *   - Single-Producer Single-Consumer (SPSC) pattern
 *   - Wait-free operations (no locks, no CAS loops in common case)
 *   - Memory barrier aware
 *   - Cache-friendly design
 * 
 * Part of WtFutuCore - Futures High-Frequency Market Making Engine
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <type_traits>
#include <new>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace futu
{

//==============================================================================
// Lock-Free SPSC Queue
//==============================================================================

/**
 * @brief Lock-free single-producer single-consumer queue
 * @tparam T Element type (must be trivially copyable or movable)
 * @tparam Capacity Queue capacity (must be power of 2)
 */
template <typename T, size_t Capacity>
class LockFreeQueue
{
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

public:
    LockFreeQueue() : _head(0), _tail(0) {}

    // v7.7 C5: 析构时 drain 残留元素 — T 可含非平凡成员 (ArbTickData/
    //   ArbOrderRequest/OrphanLeg 均含 std::string), default 析构泄漏其资源。
    ~LockFreeQueue()
    {
        T item;
        while (tryPop(item)) {
        }
    }

    // Non-copyable, non-movable
    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;
    LockFreeQueue(LockFreeQueue&&) = delete;
    LockFreeQueue& operator=(LockFreeQueue&&) = delete;

    //==========================================================================
    // Producer Interface (called from producer thread only)
    //==========================================================================

    /**
     * @brief Try to push an element to the queue
     * @param item Element to push
     * @return true if successful, false if queue is full
     */
    bool tryPush(const T& item)
    {
        const size_t current_tail = _tail.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) & (Capacity - 1);

        // Check if queue is full
        if (next_tail == _head.load(std::memory_order_acquire)) {
            return false;
        }

        // Store item
        new (&_buffer[current_tail * sizeof(T)]) T(item);

        // Publish tail
        _tail.store(next_tail, std::memory_order_release);

        return true;
    }

    /**
     * @brief Try to push an element (move semantics)
     */
    bool tryPush(T&& item)
    {
        const size_t current_tail = _tail.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) & (Capacity - 1);

        if (next_tail == _head.load(std::memory_order_acquire)) {
            return false;
        }

        new (&_buffer[current_tail * sizeof(T)]) T(std::move(item));

        _tail.store(next_tail, std::memory_order_release);

        return true;
    }

    //==========================================================================
    // Consumer Interface (called from consumer thread only)
    //==========================================================================

    /**
     * @brief Try to pop an element from the queue
     * @param item Output element
     * @return true if successful, false if queue is empty
     */
    bool tryPop(T& item)
    {
        const size_t current_head = _head.load(std::memory_order_relaxed);

        // Check if queue is empty
        if (current_head == _tail.load(std::memory_order_acquire)) {
            return false;
        }

        // Load item
        item = std::move(*reinterpret_cast<T*>(&_buffer[current_head * sizeof(T)]));

        // Destroy item
        reinterpret_cast<T*>(&_buffer[current_head * sizeof(T)])->~T();

        // Publish head
        _head.store((current_head + 1) & (Capacity - 1), std::memory_order_release);

        return true;
    }

    /**
     * @brief Pop all available elements
     * @param callback Function to call for each element
     * @return Number of elements processed
     */
    template <typename Callback>
    size_t popAll(Callback&& callback)
    {
        size_t count = 0;
        T item;

        while (tryPop(item)) {
            callback(item);
            ++count;
        }

        return count;
    }

    //==========================================================================
    // Query Interface (can be called from any thread)
    //==========================================================================

    /**
     * @brief Check if queue is empty
     */
    bool empty() const { return _head.load(std::memory_order_acquire) == _tail.load(std::memory_order_acquire); }

    /**
     * @brief Check if queue is full
     */
    bool full() const
    {
        const size_t next_tail = (_tail.load(std::memory_order_acquire) + 1) & (Capacity - 1);
        return next_tail == _head.load(std::memory_order_acquire);
    }

    /**
     * @brief Get approximate size (may be stale)
     */
    size_t size() const
    {
        const size_t tail = _tail.load(std::memory_order_acquire);
        const size_t head = _head.load(std::memory_order_acquire);
        return (tail - head + Capacity) & (Capacity - 1);
    }

    /**
     * @brief Get capacity
     */
    static constexpr size_t capacity()
    {
        return Capacity - 1; // One slot is always empty
    }

private:
    // Align to cache line to prevent false sharing
    alignas(64) std::atomic<size_t> _head;
    alignas(64) std::atomic<size_t> _tail;

    // Buffer storage — 64B 对齐, 避免首元素与 _tail 共享 cache line
    alignas(64) char _buffer[Capacity * sizeof(T)];
};

//==============================================================================
// Multi-Producer Multi-Consumer Queue (using locks)
//==============================================================================


} // namespace futu
