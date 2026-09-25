#ifndef RATE_LIMITER_HPP
#define RATE_LIMITER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

/**
 * RateLimiter: Token bucket rate limiter per endpoint.
 * O(1) try_acquire(), thread-safe via atomics.
 *
 * Thread-safe: multiple threads can call try_acquire() concurrently.
 * The token refill is computed lazily on each call using atomic compare-exchange.
 *
 * Cost in hot path: 1 atomic load + 1 branch + 1 atomic store (when refilling).
 * Branch-predicted: the "tokens >= 1" branch is almost always taken (fast path).
 */
class RateLimiter {
public:
    RateLimiter(double rate_per_sec, double burst)
        : rate_per_sec_(rate_per_sec), burst_(burst),
          tokens_(burst), last_refill_ns_(now_ns()) {}

    /**
     * Try to acquire a token. Returns true if rate limit is NOT exceeded.
     * O(1), branch-predicted (fast path: tokens >= 1).
     */
    bool try_acquire() {
        refill();
        double current = tokens_.load(std::memory_order_relaxed);
        while (current >= 1.0) {
            if (tokens_.compare_exchange_weak(current, current - 1.0,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
                return true;
            }
            // CAS failed — another thread took a token, retry
            // (branch predictor will predict the while loop as taken only under contention)
        }
        return false;  // Rate limited — BLOCKED before hitting the network
    }

    /**
     * How long until the next token is available.
     * O(1), no atomics (approximate, for scheduling only).
     */
    std::chrono::microseconds next_available() const {
        refill();
        double current = tokens_.load(std::memory_order_relaxed);
        if (current >= 1.0) return std::chrono::microseconds(0);
        double deficit = 1.0 - current;
        double wait_seconds = deficit / rate_per_sec_;
        return std::chrono::microseconds(static_cast<uint64_t>(wait_seconds * 1'000'000));
    }

private:
    const double rate_per_sec_;
    const double burst_;
    mutable std::atomic<double> tokens_;
    mutable std::atomic<uint64_t> last_refill_ns_;

    static uint64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    /**
     * Refill tokens based on elapsed time since last refill.
     * Uses CAS to ensure only one thread refills per window.
     * O(1).
     */
    void refill() const {
        uint64_t now = now_ns();
        uint64_t last = last_refill_ns_.load(std::memory_order_relaxed);
        uint64_t elapsed_ns = now - last;
        if (elapsed_ns == 0) return;  // No time passed — no refill

        double elapsed_sec = static_cast<double>(elapsed_ns) / 1e9;
        double new_tokens = elapsed_sec * rate_per_sec_;
        if (new_tokens < 0.001) return;  // Too small to matter — skip (branch predicted)

        // Only one thread should refill at a time
        if (last_refill_ns_.compare_exchange_strong(last, now,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
            double current = tokens_.load(std::memory_order_relaxed);
            double refilled = current + new_tokens;
            if (refilled > burst_) refilled = burst_;  // Cap at burst
            tokens_.store(refilled, std::memory_order_release);
        }
    }
};

#endif // RATE_LIMITER_HPP
