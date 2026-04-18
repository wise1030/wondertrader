#pragma once

#include <atomic>
#include <array>
#include <optional>

namespace wtp {

/// Lock-free SPSC RingBuffer for single producer, single consumer
template<typename T, size_t Capacity>
class LockFreeRingBuffer {
public:
    LockFreeRingBuffer() : _head(0), _tail(0) {}
    
    /// Push element (producer only)
    bool try_push(const T& item) {
        const size_t head = _head.load(std::memory_order_relaxed);
        const size_t next_head = (head + 1) % Capacity;
        
        if (next_head == _tail.load(std::memory_order_acquire)) {
            return false;  // Buffer full
        }
        
        _buffer[head] = item;
        _head.store(next_head, std::memory_order_release);
        return true;
    }
    
    /// Pop element (consumer only)
    std::optional<T> try_pop() {
        const size_t tail = _tail.load(std::memory_order_relaxed);
        
        if (tail == _head.load(std::memory_order_acquire)) {
            return std::nullopt;  // Buffer empty
        }
        
        T item = _buffer[tail];
        _tail.store((tail + 1) % Capacity, std::memory_order_release);
        return item;
    }
    
    /// Check if empty (consumer side)
    bool empty() const {
        return _tail.load(std::memory_order_acquire) == 
               _head.load(std::memory_order_acquire);
    }
    
    /// Peek front element without popping (consumer only)
    std::optional<T> try_peek() const {
        const size_t tail = _tail.load(std::memory_order_relaxed);
        
        if (tail == _head.load(std::memory_order_acquire)) {
            return std::nullopt;  // Buffer empty
        }
        
        return _buffer[tail];
    }
    
    /// Get approximate size
    size_t size() const {
        const size_t head = _head.load(std::memory_order_acquire);
        const size_t tail = _tail.load(std::memory_order_acquire);
        return (head >= tail) ? (head - tail) : (Capacity - tail + head);
    }
    
    /// Clear the buffer
    void clear() {
        _head.store(0, std::memory_order_relaxed);
        _tail.store(0, std::memory_order_relaxed);
    }

private:
    alignas(64) std::atomic<size_t> _head;  // Cache line aligned
    alignas(64) std::atomic<size_t> _tail;  // Cache line aligned
    std::array<T, Capacity> _buffer;
};

} // namespace wtp
