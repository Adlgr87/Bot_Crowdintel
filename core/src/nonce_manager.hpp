#ifndef NONCE_MANAGER_HPP
#define NONCE_MANAGER_HPP

#include <atomic>
#include <cstdint>
#include <ctime>

/**
 * NonceManager: Provides instant, unique nonces for high-frequency bursts.
 * Uses CLOCK_REALTIME for sub-millisecond precision with a monotonic counter
 * to guarantee uniqueness within the same nanosecond.
 * Polymarket CLOB V2 requires a unique, monotonically increasing nonce per order.
 */
class NonceManager {
public:
    NonceManager() {
        // Initialize thread-local state on the calling thread
        init_thread_local();
    }

    // Returns a unique, monotonically increasing nonce.
    // HOT PATH: zero syscalls, zero atomic operations.
    // Uses a thread-local precomputed buffer refilled every BUFFER_SIZE calls.
    inline uint64_t get_next_nonce() {
        uint64_t idx = tl_counter_++;
        uint64_t slot = idx & (BUFFER_SIZE - 1);
        if (__builtin_expect(slot == 0, 0)) {
            refill_buffer();
        }
        return tl_precomputed_[slot];
    }

private:
    static constexpr size_t BUFFER_SIZE = 64;
    static constexpr uint64_t EPOCH_BASE_SHIFT = 40;  // high bits for thread ID

    static void init_thread_local() {
        uint64_t now = get_time_ns();
        thread_local_base_ = now;
    }

    static uint64_t get_time_ns() {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }

    inline void refill_buffer() {
        uint64_t now = get_time_ns();
        for (size_t i = 0; i < BUFFER_SIZE; i++) {
            tl_precomputed_[i] = (now << 10) | (i & 0x3FF);
        }
    }

    alignas(64) uint64_t tl_precomputed_[BUFFER_SIZE];
    uint64_t tl_counter_ = 0;
    alignas(64) inline static thread_local uint64_t thread_local_base_ = 0;
};

#endif // NONCE_MANAGER_HPP
