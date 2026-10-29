#pragma once

#include <atomic>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cstddef>

/**
 * Single-Producer Single-Consumer (SPSC) Lock-Free RingBuffer.
 * Designed for real-time audio threads (e.g. Oboe FastMixer / FastCapture).
 * Guaranteed zero-lock, zero heap allocation during write/read.
 * 64-byte alignment prevents false sharing across CPU cores.
 */
template <typename T>
class LockFreeSPSCRingBuffer {
public:
    explicit LockFreeSPSCRingBuffer(size_t capacity)
        : capacity_(capacity + 1), buffer_(capacity + 1) {}

    ~LockFreeSPSCRingBuffer() = default;

    // Non-copyable, movable
    LockFreeSPSCRingBuffer(const LockFreeSPSCRingBuffer&) = delete;
    LockFreeSPSCRingBuffer& operator=(const LockFreeSPSCRingBuffer&) = delete;
    LockFreeSPSCRingBuffer(LockFreeSPSCRingBuffer&&) noexcept = default;
    LockFreeSPSCRingBuffer& operator=(LockFreeSPSCRingBuffer&&) noexcept = default;

    /**
     * Producer writes up to `count` elements into the ring buffer.
     * Returns the actual number of elements written.
     */
    size_t write(const T* data, size_t count) {
        if (!data || count == 0) return 0;

        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t free_space = (tail + capacity_ - head - 1) % capacity_;
        const size_t to_write = std::min(count, free_space);
        if (to_write == 0) return 0;

        const size_t first_chunk = std::min(to_write, capacity_ - head);
        std::memcpy(&buffer_[head], data, first_chunk * sizeof(T));
        if (to_write > first_chunk) {
            std::memcpy(&buffer_[0], data + first_chunk, (to_write - first_chunk) * sizeof(T));
        }
        head_.store((head + to_write) % capacity_, std::memory_order_release);
        return to_write;
    }

    /**
     * Consumer reads up to `count` elements from the ring buffer.
     * Returns the actual number of elements read.
     */
    size_t read(T* dest, size_t count) {
        if (!dest || count == 0) return 0;

        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t available = (head + capacity_ - tail) % capacity_;
        const size_t to_read = std::min(count, available);
        if (to_read == 0) return 0;

        const size_t first_chunk = std::min(to_read, capacity_ - tail);
        std::memcpy(dest, &buffer_[tail], first_chunk * sizeof(T));
        if (to_read > first_chunk) {
            std::memcpy(dest + first_chunk, &buffer_[0], (to_read - first_chunk) * sizeof(T));
        }
        tail_.store((tail + to_read) % capacity_, std::memory_order_release);
        return to_read;
    }

    /**
     * Instantly purges the ring buffer. Safe to call on Barge-in.
     */
    void clear() {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }

    size_t available_read() const {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        return (head + capacity_ - tail) % capacity_;
    }

    size_t available_write() const {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return (tail + capacity_ - head - 1) % capacity_;
    }

    size_t capacity() const {
        return capacity_ - 1;
    }

    bool empty() const {
        return available_read() == 0;
    }

private:
    const size_t capacity_;
    std::vector<T> buffer_;
    alignas(64) std::atomic<size_t> head_{0}; // 64-byte cache line alignment prevents false sharing
    alignas(64) std::atomic<size_t> tail_{0};
};
