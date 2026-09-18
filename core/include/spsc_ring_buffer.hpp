#ifndef SPSC_RING_BUFFER_HPP
#define SPSC_RING_BUFFER_HPP

#include <atomic>
#include <memory>
#include <optional>
#include <cstddef>
#include <cstdint>

/**
 * SPSC_RingBuffer: Single-Producer Single-Consumer Lock-Free Queue.
 * Zero-allocation after construction (buffer allocated once at init).
 * Capacity must be a power of 2 for fast modulo via bitmask.
 */
template<typename T, size_t Capacity = 4096>
class SPSC_RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static_assert(Capacity > 1, "Capacity must be > 1");

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

        new (&buffer_[current_head]) T(item);  // Placement new, no allocation
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    // Consumer: Pop an item from the queue
    std::optional<T> try_pop() {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        if (current_tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt; // Queue Empty
        }

        size_t next_tail = (current_tail + 1) & mask_;
        T item = buffer_[current_tail];  // Copy out (POD type)
        buffer_[current_tail].~T();      // Call destructor
        tail_.store(next_tail, std::memory_order_release);
        return item;
    }

    // Returns current capacity
    static constexpr size_t capacity() { return Capacity; }

private:
    alignas(64) std::unique_ptr<T[]> buffer_;
    alignas(64) std::atomic<size_t> head_;  // Producer writes
    alignas(64) std::atomic<size_t> tail_;  // Consumer writes
    static constexpr size_t mask_ = Capacity - 1;
};

#endif // SPSC_RING_BUFFER_HPP