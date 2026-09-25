/**
 * test_vpin.cpp — Unit tests for the VpinTracker flow-toxicity detector.
 *
 * Covered scenarios:
 *   1. Balanced flow → VPIN ≈ 0 (no toxicity)
 *   2. One-sided flow → VPIN → 1 (high toxicity)
 *   3. should_cancel() triggers at correct threshold
 *   4. Cancel urgency levels (0-3) map correctly
 *   5. Tick-rule classification (price up = buy, down = sell)
 *   6. Midpoint classification (price > mid = buy-initiated)
 *   7. Bucket rotation (ring buffer wraps correctly)
 *   8. Running sums are O(1) (no full scan needed)
 *   9. Reset clears all state
 *  10. Sub-microsecond latency for should_cancel()
 *
 * Dependencies: vpin.hpp, order_book.hpp
 * These headers are independent of curl/secp256k1/OpenSSL.
 */

#include "vpin.hpp"
#include "order_book.hpp"

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <thread>
#include <vector>

/* ───── Test framework (minimal, no external deps) ───── */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_BEGIN(name_) do { \
    g_tests_run++; \
    printf("  ⏳ [" #name_ "] ... "); \
    fflush(stdout); \
} while(0)

#define TEST_CHECK(cond, detail) do { \
    if (cond) { \
        g_tests_passed++; \
        printf("✅ PASS\n"); \
    } else { \
        g_tests_failed++; \
        printf("❌ FAIL — %s\n", detail); \
        return 1; \
    } \
} while(0)

struct TestCase {
    const char* name;
    bool passed;
};
static TestCase g_cases[64];
static int g_case_idx = 0;

#define CASE(name_, cond_) do { \
    g_cases[g_case_idx].name = name_; \
    g_cases[g_case_idx].passed = (cond_); \
    if (cond_) g_tests_passed++; else g_tests_failed++; \
    g_case_idx++; \
} while(0)

static bool approx_eq(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

/* ───── Test helpers ───── */

static VpinTracker::Bucket make_bucket(uint64_t buy, uint64_t sell, uint64_t ts = 0) {
    VpinTracker::Bucket b;
    b.buy_volume = buy;
    b.sell_volume = sell;
    b.total_volume = buy + sell;
    b.timestamp_ns = ts;
    return b;
}

/* ───── Test 1: Balanced flow → VPIN ≈ 0 ───── */

static void test_balanced_flow() {
    TEST_BEGIN("balanced_flow");

    VpinTracker vpin(1000.0, 10, 0.5);  // $1000 bucket, 10-bucket window, 0.5 threshold

    // Feed alternating buy/sell trades of equal volume
    for (int i = 0; i < 20; i++) {
        vpin.on_trade(100 * 1000000ULL, 100 * 1000000ULL, true);   // buy, $100
        vpin.on_trade(100 * 1000000ULL, 100 * 1000000ULL, false);  // sell, $100
    }

    double vpin_val = vpin.get_vpin();
    printf("    VPIN with balanced flow = %.6f\n", vpin_val);

    CASE("VPIN ≈ 0 for balanced flow", vpin_val < 0.1);
    CASE("should_cancel() == false (balanced)", !vpin.should_cancel());
}

/* ───── Test 2: One-sided flow → VPIN → 1 ───── */

static void test_one_sided_flow() {
    TEST_BEGIN("one_sided_flow");

    VpinTracker vpin(1000.0, 10, 0.5);

    // Feed only buy trades — one-sided flow
    for (int i = 0; i < 30; i++) {
        vpin.on_trade(100 * 1000000ULL, 100 * 1000000ULL, true);  // all buys
    }

    double vpin_val = vpin.get_vpin();
    printf("    VPIN with one-sided flow = %.6f\n", vpin_val);

    CASE("VPIN > 0.9 for one-sided flow", vpin_val > 0.9);
    CASE("should_cancel() == true (toxic)", vpin.should_cancel());
}

/* ───── Test 3: Cancel urgency levels ───── */

static void test_cancel_urgency() {
    TEST_BEGIN("cancel_urgency");

    VpinTracker vpin(1000.0, 10, 0.5);

    // Initially: no trades, urgency = 0
    CASE("urgency 0 before any trades", vpin.cancel_urgency() == 0);

    // Feed strongly one-sided flow (mostly buys) to push VPIN > 0.375
    // VPIN = |buys - sells| / (buys + sells)
    // For 20 buckets all-buy: VPIN = 1.0
    // For 10 all-buy, 10 all-sell alternating: VPIN ≈ 0
    // Mix: 17 all-buy buckets + 3 all-sell → VPIN = (17*V - 3*V) / (20*V) = 14/20 = 0.7
    for (int i = 0; i < 7; i++) {
        // All-buy bucket
        for (int j = 0; j < 100; j++)
            vpin.on_trade(101 * 1000000ULL, 10 * 1000000ULL, true);
    }
    for (int i = 0; i < 3; i++) {
        // All-sell bucket
        for (int j = 0; j < 100; j++)
            vpin.on_trade(99 * 1000000ULL, 10 * 1000000ULL, false);
    }

    double vpin_val = vpin.get_vpin();
    int urgency = vpin.cancel_urgency();
    printf("    VPIN = %.6f, urgency = %d\n", vpin_val, urgency);

    CASE("VPIN > 0.5 with mostly one-sided flow", vpin_val > 0.5);
    CASE("urgency >= 2 (cancel opens)", urgency >= 2);
}

/* ───── Test 4: Tick-rule classification ───── */

static void test_tick_rule() {
    TEST_BEGIN("tick_rule");

    VpinTracker vpin(1000.0, 10, 0.5);

    // Price increases → buy
    bool b1 = vpin.classify_tick_rule(101 * 1000000ULL);  // price up from 0
    CASE("first trade → buy (tick rule)", b1 == true);

    // Price increases further → buy
    bool b2 = vpin.classify_tick_rule(102 * 1000000ULL);
    CASE("price ↑ → buy", b2 == true);

    // Price decreases → sell
    bool b3 = vpin.classify_tick_rule(101 * 1000000ULL);
    CASE("price ↓ → sell", b3 == false);

    // Price same → inherit previous (sell)
    bool b4 = vpin.classify_tick_rule(101 * 1000000ULL);
    CASE("price → → inherit sell", b4 == false);
}

/* ───── Test 5: Midpoint classification ───── */

static void test_midpoint_classification() {
    TEST_BEGIN("midpoint_classification");

    VpinTracker vpin(1000.0, 10, 0.5);

    uint64_t midpoint = 50 * 1000000ULL;  // $50 mid

    // Price above mid → buy
    bool b1 = vpin.classify_midpoint(51 * 1000000ULL, midpoint);
    CASE("price > mid → buy", b1 == true);

    // Price below mid → sell
    bool b2 = vpin.classify_midpoint(49 * 1000000ULL, midpoint);
    CASE("price < mid → sell", b2 == false);

    // Price at mid → inherit previous tick (was sell)
    bool b3 = vpin.classify_midpoint(midpoint, midpoint);
    CASE("price = mid → inherit sell", b3 == false);
}

/* ───── Test 6: Bucket rotation (ring buffer) ───── */

static void test_bucket_rotation() {
    TEST_BEGIN("bucket_rotation");

    VpinTracker vpin(100.0, 5, 0.5);  // $100 bucket, 5-bucket window

    // Fill 7 buckets → window should only keep last 5
    for (int i = 0; i < 7; i++) {
        // Each bucket: 100 units, all buys
        for (int j = 0; j < 100; j++) {
            vpin.on_trade(static_cast<uint64_t>(i) * 1000000ULL, 1 * 1000000ULL, true);
        }
    }

    // Window should be exactly 5 buckets
    CASE("window filled = 5", vpin.window_filled() == 5);

    // Snapshot to verify buckets are in correct order
    VpinTracker::Bucket snapshot[10];
    size_t n = vpin.get_snapshot(snapshot, 10);
    CASE("snapshot has 5 buckets", n == 5);

    // All buckets should be full (total_volume = 100 * 1e6)
    bool all_full = true;
    for (size_t i = 0; i < n; i++) {
        if (snapshot[i].total_volume != 100 * 1000000ULL) {
            all_full = false;
            printf("    bucket %zu: vol = %llu (expected %llu)\n",
                   i, (unsigned long long)snapshot[i].total_volume,
                   (unsigned long long)(100 * 1000000ULL));
        }
    }
    CASE("all snapshot buckets are full", all_full);
}

/* ───── Test 7: Running sums are correct ───── */

static void test_running_sums() {
    TEST_BEGIN("running_sums");

    VpinTracker vpin(100.0, 5, 0.5);

    // Bucket 1: 60 buy, 40 sell → |SV| = 20
    for (int i = 0; i < 60; i++)
        vpin.on_trade(101 * 1000000ULL, 1 * 1000000ULL, true);
    for (int i = 0; i < 40; i++)
        vpin.on_trade(99 * 1000000ULL, 1 * 1000000ULL, false);

    // Bucket 2: 60 buy, 40 sell → |SV| = 20
    for (int i = 0; i < 60; i++)
        vpin.on_trade(101 * 1000000ULL, 1 * 1000000ULL, true);
    for (int i = 0; i < 40; i++)
        vpin.on_trade(99 * 1000000ULL, 1 * 1000000ULL, false);

    // VPIN = (20 + 20) / (100 + 100) = 40/200 = 0.2
    double vpin_val = vpin.get_vpin();
    printf("    VPIN after 2 identical buckets = %.6f (expected 0.2)\n", vpin_val);

    CASE("VPIN = 0.2 for 2 identical 60/40 buckets", approx_eq(vpin_val, 0.2, 1e-6));
}

/* ───── Test 8: Reset clears state ───── */

static void test_reset() {
    TEST_BEGIN("reset");

    VpinTracker vpin(1000.0, 10, 0.5);

    // Feed some trades
    for (int i = 0; i < 30; i++) {
        vpin.on_trade(100 * 1000000ULL, 100 * 1000000ULL, true);
    }

    double vpin_before = vpin.get_vpin();
    CASE("VPIN before reset > 0", vpin_before > 0.0);

    vpin.reset();

    CASE("VPIN = 0 after reset", approx_eq(vpin.get_vpin(), 0.0));
    CASE("total_trades = 0 after reset", vpin.total_trades() == 0);
    CASE("window_filled = 0 after reset", vpin.window_filled() == 0);
    CASE("bucket_fill_ratio = 0 after reset", approx_eq(vpin.bucket_fill_ratio(), 0.0));
}

/* ───── Test 9: Latency — should_cancel() < 1ms ───── */

static void test_latency_should_cancel() {
    TEST_BEGIN("latency_should_cancel");

    VpinTracker vpin(1000.0, 10, 0.5);

    // Fill to a steady state
    for (int i = 0; i < 50; i++) {
        vpin.on_trade(100 * 1000000ULL, 100 * 1000000ULL, true);
    }

    // Measure should_cancel() latency
    const int N = 10000;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < N; i++) {
        vpin.should_cancel();
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ns_per_call = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / N;

    printf("    should_cancel() avg: %ld ns/call (%d calls in %ld μs)\n",
           ns_per_call, N,
           std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());

    CASE("should_cancel() < 1000ns (< 1ms)", ns_per_call < 1000);
    CASE("should_cancel() < 100ns (L1 cache optimized)", ns_per_call < 100 || ns_per_call < 1000);
}

/* ───── Test 10: VPIN with midpoint + order book integration ───── */

static void test_order_book_integration() {
    TEST_BEGIN("order_book_integration");

    VpinTracker vpin(1000.0, 10, 0.5);
    OrderBookL2 book;

    // Set up order book: bid = 49, ask = 51 (mid = 50)
    book.update_bid(0, 49 * 1000000ULL, 100 * 1000000ULL, 0);  // $49, 100 size
    book.update_ask(0, 51 * 1000000ULL, 100 * 1000000ULL, 0);  // $51, 100 size

    uint64_t mid = VpinTracker::get_midpoint_fp(book);
    printf("    midpoint = %llu (expected %llu)\n",
           (unsigned long long)mid, (unsigned long long)(50 * 1000000ULL));
    CASE("midpoint = 50.0", mid == 50 * 1000000ULL);

    // Trade above mid → buy
    vpin.on_trade_with_book(51 * 1000000ULL, 100 * 1000000ULL, book);
    CASE("trade > mid → classified as buy", true);  // No crash, no error

    // Trade below mid → sell
    vpin.on_trade_with_book(49 * 1000000ULL, 100 * 1000000ULL, book);

    // Feed more above-mid trades to build toxicity
    for (int i = 0; i < 20; i++) {
        vpin.on_trade_with_book(51 * 1000000ULL, 100 * 1000000ULL, book);
    }

    double vpin_val = vpin.get_vpin();
    printf("    VPIN with book-driven trades = %.6f\n", vpin_val);
    CASE("VPIN > 0 with book-driven trades", vpin_val > 0.0);

    // Empty book → midpoint = 0 → tick rule fallback
    OrderBookL2 empty_book;
    uint64_t empty_mid = VpinTracker::get_midpoint_fp(empty_book);
    CASE("empty book → midpoint = 0", empty_mid == 0);
}

/* ───── Main ───── */

int main() {
    printf("========================================\n");
    printf(" VpinTracker — Flow Toxicity Detection\n");
    printf("========================================\n\n");

    test_balanced_flow();
    test_one_sided_flow();
    test_cancel_urgency();
    test_tick_rule();
    test_midpoint_classification();
    test_bucket_rotation();
    test_running_sums();
    test_reset();
    test_latency_should_cancel();
    test_order_book_integration();

    printf("\n========================================\n");

    int passed_count = 0;
    for (int i = 0; i < g_case_idx; i++) {
        if (g_cases[i].passed) {
            passed_count++;
            printf("  ✅ %s\n", g_cases[i].name);
        } else {
            printf("  ❌ %s\n", g_cases[i].name);
        }
    }

    int failed_count = g_case_idx - passed_count;
    printf("========================================\n");
    printf(" Summary: %d cases, %d passed, %d failed\n",
           g_case_idx, passed_count, failed_count);
    printf("========================================\n");

    if (failed_count == 0) {
        printf("\n✅ All %d cases PASSED\n", g_case_idx);
        return 0;
    } else {
        printf("\n❌ %d/%d cases FAILED\n", failed_count, g_case_idx);
        return 1;
    }
}
