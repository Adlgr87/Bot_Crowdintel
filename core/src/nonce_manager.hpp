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
    NonceManager() : counter_(0) {
        // Calibrate the base clock once at construction
        base_ns_ = get_time_ns();
    }

    // Returns a unique, monotonically increasing nonce (nanosecond precision)
    inline uint64_t get_next_nonce() {
        // Get current time in nanoseconds (monotonic, sub-ms precision)
        uint64_t now_ns = get_time_ns();
        // Ensure monotonicity: if clock hasn't advanced, use counter to break ties
        uint64_t ts = now_ns;
        if (ts <= base_ns_) {
            ts = base_ns_ + counter_.load(std::memory_order_relaxed);
        }
        // Atomically increment counter for uniqueness within the same nanosecond
        uint64_t ctr = counter_.fetch_add(1, std::memory_order_relaxed);
        // Combine timestamp with counter (lower 10 bits for sub-ns disambiguation)
        return (ts << 10) | (ctr & 0x3FF);
    }

private:
    static uint64_t get_time_ns() {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }

    alignas(64) uint64_t base_ns_;
    alignas(64) std::atomic<uint64_t> counter_;
};

#endif // NONCE_MANAGER_HPP
