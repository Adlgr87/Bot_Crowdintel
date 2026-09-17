#ifndef SPSC_RING_BUFFER_HPP
#define SPSC_RING_BUFFER_HPP

#include <atomic>
#include <vector>
#include <memory>
#include <optional>
#include <cstddef>

/**
 * SPSC_RingBuffer: Single-Producer Single-Consumer Lock-Free Queue.
 * Designed for zero-allocation communication between the Alpha Engine (Cold Path)
 * and the Execution Engine (Hot Path).
 */
template<typename T, size_t Capacity = 1024>
class SPSC_RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

public:
    SPSC_RingBuffer() : head_(0), tail_(0) {
        buffer_ = std::make_unique<T[]>(Capacity);
    }

    // Producer: Push an item into the queue
    bool try_push(const T& item) {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (current_head + 1) & mask_;
        
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false; // Queue Full
        }
        
        buffer_[current_head] = item;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    // Consumer: Pop an item from the queue
    std::optional<T> try_pop() {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        
        if (current_tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt; // Queue Empty
        }
        
        T item = buffer_[current_tail];
        tail_.store((current_tail + 1) & mask_, std::memory_order_release);
        return item;
    }

private:
    std::unique_ptr<T[]> buffer_;
    alignas(64) std::atomic<size_t> head_; // Avoid False Sharing
    alignas(64) std::atomic<size_t> tail_;
    static constexpr size_t mask_ = Capacity - 1;
};

#endif // SPSC_RING_BUFFER_HPP
