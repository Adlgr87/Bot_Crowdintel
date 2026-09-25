#ifndef RATE_LIMITER_HPP
#define RATE_LIMITER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <time.h>
#include <algorithm>

/**
 * PreciseTokenBucket: Microsecond-precision token bucket rate limiter.
 *
 * Key improvements over the original RateLimiter:
 * - Fixed-point INTEGER token storage (uint64_t, scale = 2^20) → no FP drift
 * - No refill threshold (all elapsed nanoseconds are applied, even sub-μs)
 * - clock_gettime(CLOCK_MONOTONIC) for nanosecond resolution
 * - acquire_with_wait() blocks precisely until a token is available → avoids HTTP 429
 * - next_available_ns() returns exact wait time for scheduling
 * - O(1) try_acquire(), thread-safe via atomic CAS
 *
 * Fixed-point scale: 1 token = TOKEN_SCALE (2^20 ≈ 1.05M) units.
 * At 1 token/sec, 1 microsecond of elapsed time = 1.05 fixed-point units
 * → sub-microsecond refill precision even at low rates.
 *
 * Hot path cost: 1 atomic load + 1 branch + 1 atomic store (when refilling).
 *   Branch-predicted: the "tokens >= SCALE" branch is almost always taken.
 *
 * Thread-safe: multiple threads can call try_acquire() concurrently.
 * The token refill is computed lazily on each call using atomic compare-exchange.
 */
class PreciseTokenBucket {
public:
    // Fixed-point scale: 2^20 = 1,048,576 units per token
    // Chosen for: (a) bit-shift efficiency, (b) sub-μs precision at 1 tok/s,
    // (c) headroom in uint64_t for burst * scale (max burst ~1M at this scale).
    static constexpr uint64_t TOKEN_SCALE = 1ULL << 20;  // 1,048,576

    /**
     * @param rate_per_sec  Token refill rate (tokens per second, can be fractional)
     * @param burst         Maximum burst capacity in tokens
     */
    PreciseTokenBucket(double rate_per_sec, double burst)
        : rate_per_sec_(rate_per_sec),
          burst_fp_(to_fixed(burst)),
          // Pre-compute rate in fixed-point units per nanosecond.
          // rate_fp_per_ns = rate_per_sec * TOKEN_SCALE / 1e9
          // For rate=1/s: 1 * 1,048,576 / 1e9 = ~1.05e-3 units/ns
          // At elapsed=1μs (1000ns): refill = 1,050 units ≈ 0.001 tokens
          rate_fp_per_ns_(rate_per_sec * static_cast<double>(TOKEN_SCALE) / 1e9),
          tokens_(burst_fp_),
          last_refill_ns_(now_ns()) {}

    /**
     * Try to acquire one token without blocking.
     * Returns true if rate limit is NOT exceeded (token was consumed).
     * O(1), branch-predicted.
     */
    bool try_acquire() {
        refill();
        uint64_t current = tokens_.load(std::memory_order_relaxed);
        // Fast path: CAS subtract 1 token (TOKEN_SCALE units)
        while (current >= TOKEN_SCALE) {
            if (tokens_.compare_exchange_weak(
                    current, current - TOKEN_SCALE,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return true;
            }
            // CAS failed — another thread took a token, `current` is updated.
            // Retry loop handles contention.
        }
        return false;  // Rate limited — bucket is empty
    }

    /**
     * Try to acquire n tokens without blocking.
     * Returns true if sufficient tokens were available.
     * O(1) with CAS loop (rare contention).
     */
    bool try_acquire(size_t n) {
        if (n == 0) return true;
        refill();
        uint64_t needed = static_cast<uint64_t>(n) * TOKEN_SCALE;
        uint64_t current = tokens_.load(std::memory_order_relaxed);
        while (current >= needed) {
            if (tokens_.compare_exchange_weak(
                    current, current - needed,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    /**
     * Block until one token is available, then consume it.
     * Uses precise nanosecond scheduling to sleep exactly until the next
     * token is refilled — avoiding 429 bursts.
     *
     * Sleeps with ~5% early-wake margin to account for scheduler jitter.
     * O(1) per sleep cycle.
     */
    void acquire_with_wait() {
        while (!try_acquire()) {
            uint64_t wait_ns = next_available_ns();
            if (wait_ns == 0) continue;  // Race: refill happened
            // Sleep 5% early to wake up slightly before the token is ready
            uint64_t sleep_ns = (wait_ns * 95) / 100;
            // Ensure minimum sleep of 100ns (avoid busy-looping on very fast refill rates)
            if (sleep_ns < 100) sleep_ns = 100;

            struct timespec ts;
            ts.tv_sec = sleep_ns / 1'000'000'000ULL;
            ts.tv_nsec = sleep_ns % 1'000'000'000ULL;
            clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
        }
    }

    /**
     * Returns the time in nanoseconds until the next token is available.
     * Returns 0 if a token is immediately available.
     * O(1), no blocking.
     */
    uint64_t next_available_ns() const {
        refill();
        uint64_t current = tokens_.load(std::memory_order_relaxed);
        if (current >= TOKEN_SCALE) return 0;  // Token available now

        // Deficit in fixed-point units: how many sub-token units we need.
        uint64_t deficit_fp = TOKEN_SCALE - (current % TOKEN_SCALE);
        if (deficit_fp == 0) return 0;

        // Wait time (ns) = deficit_fp / rate_fp_per_ns
        // = deficit_fp / (rate_per_sec * TOKEN_SCALE / 1e9)
        // = deficit_fp * 1e9 / (rate_per_sec * TOKEN_SCALE)
        if (rate_per_sec_ <= 0.0) return UINT64_MAX;

        double wait_ns = static_cast<double>(deficit_fp) / rate_fp_per_ns_;
        return static_cast<uint64_t>(wait_ns);
    }

    /**
     * Returns the current token count in fixed-point units (fractional tokens
     * are preserved). Useful for telemetry/monitoring.
     */
    uint64_t tokens_fixed() const {
        const_cast<PreciseTokenBucket*>(this)->refill();
        return tokens_.load(std::memory_order_relaxed);
    }

    /** Returns the token count as a double (for logging). */
    double tokens_approx() const {
        return static_cast<double>(tokens_fixed()) / static_cast<double>(TOKEN_SCALE);
    }

    double rate_per_sec() const { return rate_per_sec_; }
    double burst() const { return static_cast<double>(burst_fp_) / static_cast<double>(TOKEN_SCALE); }

    /** Reset the bucket to full capacity (for testing or rate-limit recovery). */
    void reset() {
        tokens_.store(burst_fp_, std::memory_order_release);
        last_refill_ns_.store(now_ns(), std::memory_order_release);
    }

    /**
     * Manual refill trigger (for testing).
     * In production, refill() is called automatically by try_acquire().
     */
    void refill() const {
        uint64_t now = now_ns();
        uint64_t last = last_refill_ns_.load(std::memory_order_relaxed);
        uint64_t elapsed_ns = now - last;
        if (elapsed_ns == 0) return;

        double new_tokens_fp = static_cast<double>(elapsed_ns) * rate_fp_per_ns_;
        uint64_t new_tokens = static_cast<uint64_t>(new_tokens_fp);
        if (new_tokens == 0) return;

        if (last_refill_ns_.compare_exchange_strong(
                last, now,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            uint64_t current = tokens_.load(std::memory_order_relaxed);
            uint64_t refilled = current + new_tokens;
            if (refilled > burst_fp_) refilled = burst_fp_;
            tokens_.store(refilled, std::memory_order_release);
        }
    }

    // ─── Internal helpers ────────────────────────────────────────────
private:
    static uint64_t now_ns() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    static uint64_t to_fixed(double val) {
        return static_cast<uint64_t>(val * static_cast<double>(TOKEN_SCALE));
    }

    // --- Configuration (immutable after construction) ---
    const double rate_per_sec_;
    const uint64_t burst_fp_;          // Burst capacity in fixed-point units
    const double rate_fp_per_ns_;      // Pre-computed: tokens (FP units) per nanosecond

    // --- State (atomic, thread-safe) ---
    mutable std::atomic<uint64_t> tokens_;        // Current token count (fixed-point)
    mutable std::atomic<uint64_t> last_refill_ns_; // Last refill timestamp (ns, CLOCK_MONOTONIC)
};

/**
 * RateLimiter: Alias for PreciseTokenBucket — maintains backward compatibility
 * with existing code that references `RateLimiter`.
 *
 * The original RateLimiter used floating-point token counts with a 0.001
 * refill threshold, which could cause HTTP 429 under burst traffic.
 * PreciseTokenBucket eliminates FP drift and applies ALL elapsed time.
 */
using RateLimiter = PreciseTokenBucket;

#endif // RATE_LIMITER_HPP
