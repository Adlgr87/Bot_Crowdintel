#ifndef NONCE_MANAGER_HPP
#define NONCE_MANAGER_HPP

#include <atomic>
#include <cstdint>

/**
 * NonceManager: Provides instant, unique nonces for high-frequency bursts.
 * CLOB V2 uses timestamps, but we add a monotonic counter to ensure uniqueness
 * within the same millisecond.
 */
class NonceManager {
public:
    NonceManager() : counter_(0) {}

    // Returns a unique nonce based on current time + atomic counter
    inline uint64_t get_next_nonce() {
        return (get_timestamp_ms() * 1000) + (counter_.fetch_add(1, std::memory_order_relaxed) % 1000);
    }

private:
    // Simplified timestamp for demo; in production use RDTSC or clock_gettime(CLOCK_REALTIME)
    uint64_t get_timestamp_ms() {
        return 1600000000000; // Mock timestamp
    }

    alignas(64) std::atomic<uint64_t> counter_;
};

#endif // NONCE_MANAGER_HPP
