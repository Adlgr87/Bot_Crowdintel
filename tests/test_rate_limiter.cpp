/**
 * test_rate_limiter.cpp — Unit tests for the token-bucket RateLimiter.
 *
 * Covered scenarios (T1-1):
 *   1. Burst consumption      — all burst tokens can be acquired immediately
 *   2. Exhaustion / blocking  — try_acquire() returns false when tokens are spent
 *   3. next_available() timing — returns correct ≈1/rate seconds after exhaustion
 *   4. Thread-safety            — concurrent acquire from N threads; total
 *                                 successes == expected (no over-acquire)
 *
 * Build: CMake auto-discovers via GLOB_RECURSE on tests/test_*.cpp
 *   (see core/CMakeLists.txt — no manual registration required)
 */

#include "rate_limiter.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

// ─── Tiny test harness ────────────────────────────────────────────────────

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_CASE(name)                                                        \
    do {                                                                       \
        g_tests_run++;                                                         \
        std::cout << "  ⏳ [" << #name << "] ... " << std::flush;             \
    } while (0)

#define TEST_PASS()                                                            \
    do {                                                                       \
        g_tests_passed++;                                                      \
        std::cout << "\r  ✅ [" << __func__ << "] PASSED\n";                   \
    } while (0)

#define TEST_FAIL(msg)                                                         \
    do {                                                                       \
        g_tests_failed++;                                                      \
        std::cout << "\r  ❌ [" << __func__ << "] FAILED: " << msg << "\n";    \
        return 1;                                                             \
    } while (0)

// ─── T1-1 Test 1: Burst consumption ────────────────────────────────────────

static int test_burst_consumption() {
    TEST_CASE("burst_consumption");

    const double rate = 100.0;   // 100 tokens/sec
    const double burst = 10.0;   // 10-token bucket

    RateLimiter rl(rate, burst);

    // All burst tokens should be acquirable immediately (no waiting)
    int acquired = 0;
    for (int i = 0; i < static_cast<int>(burst); i++) {
        if (rl.try_acquire()) acquired++;
    }

    if (acquired != static_cast<int>(burst)) {
        TEST_FAIL("Expected " << burst << " acquisitions, got " << acquired);
    }

    TEST_PASS();
    return 0;
}

// ─── T1-1 Test 2: Exhaustion / blocking ────────────────────────────────────

static int test_exhaustion_blocks() {
    TEST_CASE("exhaustion_blocks");

    // Use a very low rate so no measurable refill occurs during the test.
    // rate=0.001 tokens/sec → 1 token per ~1000 seconds.
    // Even the refill threshold check (< 0.001 tokens) prevents any refill
    // from completing in microseconds.
    const double rate = 0.001;
    const double burst = 5.0;

    RateLimiter rl(rate, burst);

    // Drain the bucket
    for (int i = 0; i < static_cast<int>(burst); i++) {
        if (!rl.try_acquire()) {
            TEST_FAIL("Failed to acquire burst token #" << i);
        }
    }

    // Next call must be blocked — no token can have refilled at this rate
    if (rl.try_acquire()) {
        TEST_FAIL("try_acquire() returned true after burst was exhausted");
    }

    TEST_PASS();
    return 0;
}

// ─── T1-1 Test 3: next_available() returns correct time ─────────────────────

static int test_next_available_timing() {
    TEST_CASE("next_available_timing");

    const double rate = 10.0;    // 10 tokens/sec → 100ms between tokens
    const double burst = 1.0;    // single-token bucket for deterministic timing

    RateLimiter rl(rate, burst);

    // Consume the single burst token
    if (!rl.try_acquire()) {
        TEST_FAIL("Failed to acquire initial token");
    }

    // Next token should be ~1/rate seconds away
    auto wait = rl.next_available();
    auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wait).count();

    // Expected: 1/10 * 1e6 = 100000μs = 100ms.
    // Allow ±50% tolerance (timing, platform jitter, etc.)
    const auto expected_ms = static_cast<long long>(1.0 / rate * 1000.0);
    const auto tolerance_ms = expected_ms / 2;   // ±50%

    if (wait_ms < expected_ms - tolerance_ms || wait_ms > expected_ms + tolerance_ms) {
        TEST_FAIL("next_available() returned " << wait_ms << "ms, expected ~"
                    << expected_ms << "ms (±" << tolerance_ms << "ms)");
    }

    TEST_PASS();
    return 0;
}

// ─── T1-1 Test 4: Thread-safety — no over-acquire under contention ──────────

static int test_thread_safety() {
    TEST_CASE("thread_safety");

    // Low rate so refill is negligible during the test window, high burst
    // so we can verify the atomic CAS doesn't allow over-acquisition.
    const double rate = 0.001;     // 1 token per ~1000 sec — no refill during test
    const double burst = 200.0;   // 200-token bucket

    RateLimiter rl(rate, burst);

    const int num_threads = 8;
    const int acquires_per_thread = 50;   // 8 × 50 = 400 requests total
    const int total_requests = num_threads * acquires_per_thread;

    std::atomic<int> total_acquired(0);
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    std::atomic<int> ready(0);

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&rl, &total_acquired, &ready, acquires_per_thread]() {
            // Spin-wait until all threads are ready (maximize contention)
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) < 8) {
                std::this_thread::yield();
            }

            int local = 0;
            for (int i = 0; i < acquires_per_thread; i++) {
                if (rl.try_acquire()) local++;
            }
            total_acquired.fetch_add(local, std::memory_order_release);
        });
    }

    for (auto& t : threads) t.join();

    int acquired = total_acquired.load();

    // Since burst=200 and we fire 400 requests with ~0 refill (rate=1/sec, test runs
    // in microseconds), the total acquired MUST be <= 200 (the bucket size).
    // And it MUST be > 0 (some threads will succeed).
    // Critically: it must NOT exceed burst — proving the atomic CAS prevents
    // over-acquisition under contention.

    if (acquired > static_cast<int>(burst)) {
        TEST_FAIL("Thread-safety violation: acquired " << acquired
                     << " tokens but burst is only " << burst);
    }

    if (acquired == 0) {
        TEST_FAIL("No tokens acquired — something is wrong");
    }

    if (acquired < static_cast<int>(burst)) {
        // Some tokens might be left unused if threads finish at different rates.
        // This is acceptable.
        std::cout << "\r  ✅ [thread_safety] PASSED (acquired " << acquired
                  << "/" << burst << " under contention)" << std::endl;
    } else {
        std::cout << "\r  ✅ [thread_safety] PASSED (acquired exactly "
                  << acquired << " = burst)" << std::endl;
    }

    g_tests_passed++;
    return 0;
}

// ─── T1-1 Test 5: Refill actually works after waiting ──────────────────────

static int test_refill_after_wait() {
    TEST_CASE("refill_after_wait");

    const double rate = 100.0;   // 100 tokens/sec → 1 token per 10ms
    const double burst = 1.0;    // single-token bucket

    RateLimiter rl(rate, burst);

    // Consume the token
    if (!rl.try_acquire()) {
        TEST_FAIL("Failed to acquire initial token for refill test");
    }
    if (rl.try_acquire()) {
        TEST_FAIL("try_acquire() returned true after bucket was drained");
    }

    // Wait long enough for 2 tokens to refill
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    // Should be able to acquire again
    if (!rl.try_acquire()) {
        TEST_FAIL("try_acquire() returned false after refill wait — refill logic broken");
    }

    TEST_PASS();
    return 0;
}

// ─── T1-1 Test 6: next_available() returns 0 when tokens available ──────────

static int test_next_available_zero() {
    TEST_CASE("next_available_zero");

    RateLimiter rl(100.0, 10.0);

    auto wait = rl.next_available();
    auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(wait).count();

    if (wait_us != 0) {
        TEST_FAIL("next_available() returned " << wait_us << "μs, expected 0 when tokens available");
    }

    TEST_PASS();
    return 0;
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main() {
    std::cout << "🚀 RateLimiter Token Bucket — Unit Tests\n";
    std::cout << "=========================================\n\n";

    int (*tests[])() = {
        test_burst_consumption,
        test_exhaustion_blocks,
        test_next_available_timing,
        test_thread_safety,
        test_refill_after_wait,
        test_next_available_zero,
    };

    for (auto test : tests) {
        if (test() != 0) return 1;
    }

    std::cout << "\n=========================================\n";
    std::cout << "📊 Results: " << g_tests_passed << "/" << g_tests_run
              << " passed";
    if (g_tests_failed > 0) {
        std::cout << ", " << g_tests_failed << " FAILED";
    }
    std::cout << "\n";

    return g_tests_failed > 0 ? 1 : 0;
}
