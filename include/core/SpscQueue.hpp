#pragma once

#include <atomic>
#include <cstddef>
#include <vector>
#include <new>

namespace core {

#if defined(__cpp_lib_hardware_interference_size)
using std::hardware_destructive_interference_size;
#else
constexpr size_t hardware_destructive_interference_size = 64;
#endif

// Lock-Free Single-Producer Single-Consumer (SPSC) Ring Buffer
// Thread safe for exactly 1 producer and 1 consumer (D16, Section 3)
template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(size_t capacity)
        : capacity_(capacity + 1),
          buffer_(capacity + 1) {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    bool push(const T& item) {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (current_head + 1) % capacity_;

        // Check if queue is full
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[current_head] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    bool push(T&& item) {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (current_head + 1) % capacity_;

        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[current_head] = std::move(item);
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        // Check if queue is empty
        if (current_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        item = std::move(buffer_[current_tail]);
        tail_.store((current_tail + 1) % capacity_, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    size_t capacity() const {
        return capacity_ - 1;
    }

private:
    const size_t capacity_;
    std::vector<T> buffer_;

    // Cacheline padding between head and tail to prevent false sharing
    alignas(hardware_destructive_interference_size) std::atomic<size_t> head_{0};
    alignas(hardware_destructive_interference_size) std::atomic<size_t> tail_{0};
};

} // namespace core
