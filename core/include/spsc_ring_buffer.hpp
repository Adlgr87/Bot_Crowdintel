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
 *
 * Handles non-trivially-destructible types (e.g. TelemetryEvent with std::string):
 * - Raw memory is allocated (no default-construction of elements)
 * - Elements are constructed via placement new in try_push()
 * - Elements are destructed in try_pop() after copying out
 * - Destructor only destructs elements still live (between tail_ and head_)
 */
template<typename T, size_t Capacity = 4096>
class SPSC_RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static_assert(Capacity > 1, "Capacity must be > 1");

public:
    SPSC_RingBuffer() : head_(0), tail_(0) {
        // Allocate raw memory — no element construction.
        // Elements are constructed on-demand via placement new in try_push().
        // This avoids constructing all Capacity default-objects (which would
        // be wasted memory for non-trivial types like std::string).
        buffer_ = static_cast<T*>(::operator new[](Capacity * sizeof(T)));
    }

    ~SPSC_RingBuffer() {
        // Destruct only the elements that are still live in the buffer
        // (between tail_ and head_). Elements already popped were destructed
        // in try_pop(); elements never pushed were never constructed.
        size_t current_head = head_.load(std::memory_order_relaxed);
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        while (current_tail != current_head) {
            buffer_[current_tail].~T();
            current_tail = (current_tail + 1) & mask_;
        }
        // Free raw memory (no destructors called by ::operator delete[])
        ::operator delete[](buffer_);
    }

    // Non-copyable, non-movable (owns raw memory)
    SPSC_RingBuffer(const SPSC_RingBuffer&) = delete;
    SPSC_RingBuffer& operator=(const SPSC_RingBuffer&) = delete;

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
        T item = buffer_[current_tail];  // Copy out
        buffer_[current_tail].~T();      // Call destructor on slot
        tail_.store(next_tail, std::memory_order_release);
        return item;
    }

    // Returns current capacity
    static constexpr size_t capacity() { return Capacity; }

private:
    alignas(64) T* buffer_;         // Raw memory (no unique_ptr — manual destructor)
    alignas(64) std::atomic<size_t> head_;  // Producer writes
    alignas(64) std::atomic<size_t> tail_;  // Consumer writes
    static constexpr size_t mask_ = Capacity - 1;
};

#endif // SPSC_RING_BUFFER_HPP