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

namespace futu {

//==============================================================================
// Lock-Free SPSC Queue
//==============================================================================

/**
 * @brief Lock-free single-producer single-consumer queue
 * @tparam T Element type (must be trivially copyable or movable)
 * @tparam Capacity Queue capacity (must be power of 2)
 */
template<typename T, size_t Capacity>
class LockFreeQueue
{
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static_assert(Capacity >= 2, "Capacity must be at least 2");
    
public:
    LockFreeQueue()
        : _head(0)
        , _tail(0)
    {
    }
    
    ~LockFreeQueue() = default;
    
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
        if (next_tail == _head.load(std::memory_order_acquire))
        {
            return false;
        }
        
        // Store item
        new (&_buffer[current_tail]) T(item);
        
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
        
        if (next_tail == _head.load(std::memory_order_acquire))
        {
            return false;
        }
        
        new (&_buffer[current_tail]) T(std::move(item));
        
        _tail.store(next_tail, std::memory_order_release);
        
        return true;
    }
    
    /**
     * @brief Push an element, overwrite oldest if full
     * 
     * 修复方案：生产者不再直接修改 _head（消费者索引），
     * 而是通过 _drop_count 原子计数器通知消费者跳过元素。
     * 消费者在 tryPop 时检查 _drop_count 并跳过对应数量的元素。
     * 这保证了 SPSC 语义：_head 只由消费者写，_tail 只由生产者写。
     */
    bool pushOverwrite(const T& item)
    {
        const size_t current_tail = _tail.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) & (Capacity - 1);
        
        bool overwritten = false;
        
        if (next_tail == _head.load(std::memory_order_acquire))
        {
            // 队列满：增加 drop 计数器，消费者会跳过对应数量的元素
            // 不再由生产者修改 _head，避免数据竞争
            _drop_count.fetch_add(1, std::memory_order_release);
            overwritten = true;
        }
        
        new (&_buffer[current_tail]) T(item);
        _tail.store(next_tail, std::memory_order_release);
        
        return overwritten;
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
        // 先处理 drop：消费者跳过被覆盖的元素
        // 这保证了 SPSC 语义：只有消费者修改 _head
        size_t drops = _drop_count.load(std::memory_order_acquire);
        if (drops > 0)
        {
            size_t skipped = 0;
            for (size_t i = 0; i < drops; ++i)
            {
                const size_t current_head = _head.load(std::memory_order_relaxed);
                // 检查队列是否为空（可能 drop 数量超过实际元素）
                if (current_head == _tail.load(std::memory_order_acquire))
                    break;
                // 析构被跳过的元素
                reinterpret_cast<T*>(&_buffer[current_head])->~T();
                _head.store((current_head + 1) & (Capacity - 1), std::memory_order_release);
                ++skipped;
            }
            // 减去已跳过的数量
            _drop_count.fetch_sub(skipped, std::memory_order_release);
        }
        
        const size_t current_head = _head.load(std::memory_order_relaxed);
        
        // Check if queue is empty
        if (current_head == _tail.load(std::memory_order_acquire))
        {
            return false;
        }
        
        // Load item
        item = std::move(*reinterpret_cast<T*>(&_buffer[current_head]));
        
        // Destroy item
        reinterpret_cast<T*>(&_buffer[current_head])->~T();
        
        // Publish head
        _head.store((current_head + 1) & (Capacity - 1), std::memory_order_release);
        
        return true;
    }
    
    /**
     * @brief Pop all available elements
     * @param callback Function to call for each element
     * @return Number of elements processed
     */
    template<typename Callback>
    size_t popAll(Callback&& callback)
    {
        size_t count = 0;
        T item;
        
        while (tryPop(item))
        {
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
    bool empty() const
    {
        return _head.load(std::memory_order_acquire) == 
               _tail.load(std::memory_order_acquire);
    }
    
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
        return Capacity - 1;  // One slot is always empty
    }
    
private:
    // Align to cache line to prevent false sharing
    alignas(64) std::atomic<size_t> _head;
    alignas(64) std::atomic<size_t> _tail;
    alignas(64) std::atomic<size_t> _drop_count{0};  // drop计数器，用于pushOverwrite
    
    // Buffer storage
    alignas(alignof(T)) char _buffer[Capacity * sizeof(T)];
};

//==============================================================================
// Multi-Producer Multi-Consumer Queue (using locks)
//==============================================================================

/**
 * @brief MPMC queue with spinlock protection
 * For cases where multiple producers/consumers are needed
 */
template<typename T, size_t Capacity>
class BlockingQueue
{
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    
public:
    BlockingQueue()
        : _head(0)
        , _tail(0)
    {
    }
    
    bool tryPush(const T& item)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        
        const size_t current_tail = _tail;
        const size_t next_tail = (current_tail + 1) & (Capacity - 1);
        
        if (next_tail == _head)
        {
            return false;
        }
        
        _buffer[current_tail] = item;
        _tail = next_tail;
        
        _cv.notify_one();
        
        return true;
    }
    
    bool tryPop(T& item)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        
        if (_head == _tail)
        {
            return false;
        }
        
        item = std::move(_buffer[_head]);
        _head = (_head + 1) & (Capacity - 1);
        
        return true;
    }
    
    bool pop(T& item, uint32_t timeout_ms = 0)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        
        if (timeout_ms > 0)
        {
            _cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), 
                         [this] { return _head != _tail; });
        }
        
        if (_head == _tail)
        {
            return false;
        }
        
        item = std::move(_buffer[_head]);
        _head = (_head + 1) & (Capacity - 1);
        
        return true;
    }
    
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _head == _tail;
    }
    
    static constexpr size_t capacity() { return Capacity - 1; }
    
private:
    mutable std::mutex _mutex;
    std::condition_variable _cv;
    
    size_t _head;
    size_t _tail;
    T _buffer[Capacity];
};

} // namespace futu
