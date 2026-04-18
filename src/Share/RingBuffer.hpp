/*!
 * \file RingBuffer.hpp
 * \brief High-performance ring buffer for latency-sensitive applications
 * 
 * Features:
 *   - Fixed capacity, stack-allocated (no heap allocation after construction)
 *   - O(1) push and access operations
 *   - Cache-friendly contiguous memory layout
 *   - Thread-unsafe (designed for single-threaded hot paths)
 * 
 * Usage:
 *   RingBuffer<double, 100> prices;
 *   prices.push(1.23);
 *   double last = prices.back();
 */
#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <type_traits>
#include <algorithm>

template<typename T, size_t Capacity>
class RingBuffer
{
    static_assert(Capacity > 0, "Capacity must be greater than 0");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2 for optimal performance");

public:
    using value_type = T;
    using size_type = size_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;

    RingBuffer() : _head(0), _size(0) {}

    //==========================================================================
    // Capacity
    //==========================================================================

    constexpr size_type capacity() const { return Capacity; }
    size_type size() const { return _size; }
    bool empty() const { return _size == 0; }
    bool full() const { return _size == Capacity; }

    //==========================================================================
    // Element access
    //==========================================================================

    reference front() { return _buffer[_head]; }
    const_reference front() const { return _buffer[_head]; }

    reference back() {
        return _buffer[(_head + _size - 1) & _mask];
    }
    const_reference back() const {
        return _buffer[(_head + _size - 1) & _mask];
    }

    // Access element by index (0 = oldest, size-1 = newest)
    reference operator[](size_type index) {
        return _buffer[(_head + index) & _mask];
    }
    const_reference operator[](size_type index) const {
        return _buffer[(_head + index) & _mask];
    }

    // Access raw data (for iteration)
    pointer data() { return _buffer.data(); }
    const_pointer data() const { return _buffer.data(); }

    //==========================================================================
    // Modifiers
    //==========================================================================

    // Push element to back (overwrites oldest if full)
    void push(const T& value) {
        if (_size < Capacity) {
            _buffer[(_head + _size) & _mask] = value;
            ++_size;
        } else {
            _buffer[_head] = value;
            _head = (_head + 1) & _mask;
        }
    }

    void push(T&& value) {
        if (_size < Capacity) {
            _buffer[(_head + _size) & _mask] = std::move(value);
            ++_size;
        } else {
            _buffer[_head] = std::move(value);
            _head = (_head + 1) & _mask;
        }
    }

    // Emplace element
    template<typename... Args>
    void emplace(Args&&... args) {
        if (_size < Capacity) {
            new (&_buffer[(_head + _size) & _mask]) T(std::forward<Args>(args)...);
            ++_size;
        } else {
            _buffer[_head].~T();
            new (&_buffer[_head]) T(std::forward<Args>(args)...);
            _head = (_head + 1) & _mask;
        }
    }

    // Pop element from front
    void pop() {
        if (_size > 0) {
            _head = (_head + 1) & _mask;
            --_size;
        }
    }

    // Clear all elements
    void clear() {
        _head = 0;
        _size = 0;
    }

    // Reset and fill with value
    void fill(const T& value) {
        _buffer.fill(value);
        _head = 0;
        _size = Capacity;
    }

    //==========================================================================
    // Statistics (for numeric types)
    //==========================================================================

    // Sum of all elements
    template<typename U = T>
    typename std::enable_if<std::is_arithmetic<U>::value, U>::type
    sum() const {
        U total = U{};
        for (size_type i = 0; i < _size; ++i) {
            total += (*this)[i];
        }
        return total;
    }

    // Average of all elements
    template<typename U = T>
    typename std::enable_if<std::is_arithmetic<U>::value, double>::type
    average() const {
        if (_size == 0) return 0.0;
        return static_cast<double>(sum()) / _size;
    }

    // Standard deviation (sample standard deviation using Bessel's correction)
    // Requires at least 2 samples, returns 0.0 otherwise
    template<typename U = T>
    typename std::enable_if<std::is_arithmetic<U>::value, double>::type
    stddev() const {
        if (_size < 2) return 0.0;  // Safe: cannot divide by zero
        double avg = average();
        double variance = 0.0;
        for (size_type i = 0; i < _size; ++i) {
            double diff = (*this)[i] - avg;
            variance += diff * diff;
        }
        // Safe division: _size >= 2, so (_size - 1) >= 1
        return std::sqrt(variance / static_cast<double>(_size - 1));
    }

    // Min value
    template<typename U = T>
    typename std::enable_if<std::is_arithmetic<U>::value, U>::type
    min() const {
        if (_size == 0) return U{};
        U result = (*this)[0];
        for (size_type i = 1; i < _size; ++i) {
            result = std::min(result, (*this)[i]);
        }
        return result;
    }

    // Max value
    template<typename U = T>
    typename std::enable_if<std::is_arithmetic<U>::value, U>::type
    max() const {
        if (_size == 0) return U{};
        U result = (*this)[0];
        for (size_type i = 1; i < _size; ++i) {
            result = std::max(result, (*this)[i]);
        }
        return result;
    }

    // Percentile (p = 0.0 to 1.0)
    template<typename U = T>
    typename std::enable_if<std::is_arithmetic<U>::value, double>::type
    percentile(double p) const {
        if (_size == 0) return 0.0;
        std::array<U, Capacity> sorted;
        for (size_type i = 0; i < _size; ++i) {
            sorted[i] = (*this)[i];
        }
        std::sort(sorted.begin(), sorted.begin() + _size);
        size_type index = static_cast<size_type>(p * (_size - 1));
        return sorted[index];
    }

    //==========================================================================
    // Iterator support (for range-based for loops)
    //==========================================================================

    class Iterator {
    public:
        Iterator(RingBuffer* buf, size_type index) : _buf(buf), _index(index) {}
        reference operator*() { return (*_buf)[_index]; }
        Iterator& operator++() { ++_index; return *this; }
        bool operator!=(const Iterator& other) const { return _index != other._index; }
    private:
        RingBuffer* _buf;
        size_type _index;
    };

    class ConstIterator {
    public:
        ConstIterator(const RingBuffer* buf, size_type index) : _buf(buf), _index(index) {}
        const_reference operator*() const { return (*_buf)[_index]; }
        ConstIterator& operator++() { ++_index; return *this; }
        bool operator!=(const ConstIterator& other) const { return _index != other._index; }
    private:
        const RingBuffer* _buf;
        size_type _index;
    };

    Iterator begin() { return Iterator(this, 0); }
    Iterator end() { return Iterator(this, _size); }
    ConstIterator begin() const { return ConstIterator(this, 0); }
    ConstIterator end() const { return ConstIterator(this, _size); }
    ConstIterator cbegin() const { return ConstIterator(this, 0); }
    ConstIterator cend() const { return ConstIterator(this, _size); }

private:
    static constexpr size_type _mask = Capacity - 1;  // For fast modulo (bitwise AND)
    
    std::array<T, Capacity> _buffer;
    size_type _head;   // Index of oldest element
    size_type _size;   // Current number of elements
};
