/**
 * test_presigned_pool.cpp — Phase 3: PresignedOrderPool Test Suite
 *
 * Verifies (Phase 3: Order Manager, Reconciliación y Positions):
 *   T3-5: check_and_invalidate with price deviation
 *
 * Also covers:
 *   - add_order (presigned order registration)
 *   - get_valid_order_ids (pool status query)
 *   - invalidate_all (emergency cancel)
 *   - remove_order (cleanup after completion)
 *   - valid_count and size queries
 *
 * Dependencies: presigned_pool.hpp, order_book.hpp (Level2Entry), eip712_signer.hpp
 * These headers are designed for isolated compilation (no execution_engine.cpp dependency).
 *
 * CRITICAL: Does NOT modify eip712_signer.hpp.
 */

#include "presigned_pool.hpp"
#include "order_book.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

/* ───── Test framework (minimal, no external deps) ───── */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

struct TestCase {
    const char* name;
    bool passed;
};
static TestCase g_cases[256];
static int g_case_idx = 0;

#define CASE(name_, cond_) do { \
    g_cases[g_case_idx].name = name_; \
    g_cases[g_case_idx].passed = (cond_); \
    g_case_idx++; \
} while(0)

static void run_test(const char* name, bool (*fn)()) {
    g_tests_run++;
    std::cout << "▶ " << name << " ..." << std::flush;
    bool ok = fn();
    if (ok) {
        g_tests_passed++;
        std::cout << " ✅ PASS" << std::endl;
    } else {
        g_tests_failed++;
        std::cout << " ❌ FAIL" << std::endl;
    }
}

/* ───── Test helpers ───── */

static PresignedOrder make_order(uint64_t nonce, uint64_t salt,
                                  uint64_t price, uint64_t size,
                                  uint8_t side, bool valid = true) {
    PresignedOrder order;
    order.nonce = nonce;
    order.salt = salt;
    memset(order.signature.data(), 0xAB, 65);
    order.payload = "{\"price\":\"" + std::to_string(price) + "\"}";
    order.price = price;
    order.size = size;
    order.side = side;
    order.created_at = std::chrono::steady_clock::now();
    order.valid = valid;
    order.client_order_id = "client_" + std::to_string(nonce);
    return order;
}

static Level2Entry make_l2_entry(uint64_t price, uint64_t size) {
    Level2Entry entry;
    entry.price = price;
    entry.size = size;
    entry.timestamp = 0;
    return entry;
}

/* ───── T3-5: add_order and basic pool operations ───── */

static bool test_add_and_query() {
    PresignedOrderPool pool(500);  // 500 bps max deviation

    CASE("pool starts empty (size=0)", pool.size() == 0);
    CASE("pool starts with 0 valid orders", pool.valid_count() == 0);

    // Add a buy order (side=0)
    PresignedOrder buy_order = make_order(100, 1, 500000000, 100000000, 0);
    pool.add_order(buy_order.client_order_id, buy_order);

    CASE("pool size = 1 after add", pool.size() == 1);
    CASE("valid_count = 1 after add", pool.valid_count() == 1);

    auto valid_ids = pool.get_valid_order_ids();
    CASE("get_valid_order_ids returns 1 order", valid_ids.size() == 1);
    CASE("get_valid_order_ids contains our order", valid_ids[0] == buy_order.client_order_id);

    return true;
}

/* ───── T3-5: check_and_invalidate — price deviation within tolerance ───── */

static bool test_check_and_invalidate_within_tolerance() {
    PresignedOrderPool pool(500);  // 500 bps (5%) max deviation

    // Buy order at $0.50 (500000000 in * 1e6)
    PresignedOrder order = make_order(100, 1, 500000000, 100000000, 0);
    pool.add_order(order.client_order_id, order);

    // Market moves slightly: ask = $0.51, bid = $0.49
    // Deviation = |0.51 - 0.50| / 0.50 * 10000 = 200 bps < 500 bps → still valid
    Level2Entry best_bid = make_l2_entry(490000000, 100000000);  // $0.49
    Level2Entry best_ask = make_l2_entry(510000000, 100000000);  // $0.51

    pool.check_and_invalidate(best_bid, best_ask);

    CASE("price within tolerance → order still valid",
         pool.valid_count() == 1);

    return true;
}

/* ───── T3-5: check_and_invalidate — price deviation exceeds tolerance ───── */

static bool test_check_and_invalidate_exceeds_tolerance() {
    PresignedOrderPool pool(500);  // 500 bps (5%) max deviation

    // Buy order at $0.50
    PresignedOrder order = make_order(100, 1, 500000000, 100000000, 0);
    pool.add_order(order.client_order_id, order);

    // Market moves significantly: ask = $0.60, bid = $0.40
    // For a buy order (side=0): current_ref = ask = $0.60
    // Deviation = |0.60 - 0.50| / 0.50 * 10000 = 2000 bps > 500 bps → invalidate
    Level2Entry best_bid = make_l2_entry(400000000, 100000000);  // $0.40
    Level2Entry best_ask = make_l2_entry(600000000, 100000000);  // $0.60

    pool.check_and_invalidate(best_bid, best_ask);

    CASE("price exceeds tolerance → order invalidated",
         pool.valid_count() == 0);

    auto valid_ids = pool.get_valid_order_ids();
    CASE("no valid orders after invalidation", valid_ids.empty());

    return true;
}

/* ───── T3-5: check_and_invalidate — sell order uses bid price ───── */

static bool test_check_and_invalidate_sell_side() {
    PresignedOrderPool pool(500);  // 500 bps max

    // Sell order at $0.50 (side=1)
    PresignedOrder order = make_order(200, 2, 500000000, 100000000, 1);
    pool.add_order(order.client_order_id, order);

    // For sell orders: current_ref = bid
    // Small change: bid = $0.49 → deviation = |0.49 - 0.50| / 0.50 * 10000 = 200 bps < 500 → valid
    Level2Entry best_bid = make_l2_entry(490000000, 100000000);  // $0.49
    Level2Entry best_ask = make_l2_entry(510000000, 100000000);  // $0.51

    pool.check_and_invalidate(best_bid, best_ask);
    CASE("sell order, price within tolerance → still valid", pool.valid_count() == 1);

    // Large change: bid = $0.40 → deviation = |0.40 - 0.50| / 0.50 * 10000 = 2000 bps > 500 → invalid
    best_bid = make_l2_entry(400000000, 100000000);  // $0.40

    pool.check_and_invalidate(best_bid, best_ask);
    CASE("sell order, price exceeds tolerance → invalidated", pool.valid_count() == 0);

    return true;
}

/* ───── T3-5: invalidate_all (emergency cancel) ───── */

static bool test_invalidate_all() {
    PresignedOrderPool pool(500);

    // Add 5 orders
    for (int i = 0; i < 5; i++) {
        PresignedOrder order = make_order(100 + i, 1 + i, 500000000, 100000000, i % 2);
        pool.add_order(order.client_order_id, order);
    }

    CASE("5 orders added, valid_count = 5", pool.valid_count() == 5);

    pool.invalidate_all();

    CASE("invalidate_all → valid_count = 0", pool.valid_count() == 0);
    CASE("invalidate_all → size still = 5 (orders not removed)", pool.size() == 5);

    auto valid_ids = pool.get_valid_order_ids();
    CASE("invalidate_all → get_valid_order_ids empty", valid_ids.empty());

    return true;
}

/* ───── T3-5: remove_order ───── */

static bool test_remove_order() {
    PresignedOrderPool pool(500);

    PresignedOrder order1 = make_order(100, 1, 500000000, 100000000, 0);
    PresignedOrder order2 = make_order(200, 2, 500000000, 100000000, 1);
    PresignedOrder order3 = make_order(300, 3, 500000000, 100000000, 0);

    pool.add_order(order1.client_order_id, order1);
    pool.add_order(order2.client_order_id, order2);
    pool.add_order(order3.client_order_id, order3);

    CASE("3 orders added", pool.size() == 3);

    // Remove one
    pool.remove_order(order2.client_order_id);
    CASE("after remove: size = 2", pool.size() == 2);
    CASE("after remove: valid_count = 2", pool.valid_count() == 2);

    // Remove a non-existent order (should be safe, no-op)
    pool.remove_order("nonexistent");
    CASE("remove nonexistent order is safe (size still 2)", pool.size() == 2);

    return true;
}

/* ───── T3-5: check_and_invalidate — only invalidates stale orders ───── */

static bool test_check_and_invalidate_selective() {
    PresignedOrderPool pool(500);

    // Add a buy order at $0.50
    PresignedOrder order_at_050 = make_order(100, 1, 500000000, 100000000, 0);
    pool.add_order(order_at_050.client_order_id, order_at_050);

    // Add a buy order at $0.40
    PresignedOrder order_at_040 = make_order(200, 2, 400000000, 100000000, 0);
    pool.add_order(order_at_040.client_order_id, order_at_040);

    // Market: ask = $0.51
    // order_at_050: |0.51 - 0.50| / 0.50 * 10000 = 200 bps < 500 → valid
    // order_at_040: |0.51 - 0.40| / 0.40 * 10000 = 2750 bps > 500 → invalid
    Level2Entry best_bid = make_l2_entry(490000000, 100000000);  // $0.49
    Level2Entry best_ask = make_l2_entry(510000000, 100000000);  // $0.51

    pool.check_and_invalidate(best_bid, best_ask);

    CASE("selective invalidation: order at $0.50 still valid", pool.valid_count() == 1);

    auto valid_ids = pool.get_valid_order_ids();
    CASE("selective invalidation: only $0.50 order remains valid",
         valid_ids.size() == 1 && valid_ids[0] == order_at_050.client_order_id);

    return true;
}

/* ───── T3-5: check_and_invalidate — already invalid orders are skipped ───── */

static bool test_check_and_invalidate_skip_invalid() {
    PresignedOrderPool pool(500);

    // Add a valid order and an already-invalid order
    PresignedOrder valid_order = make_order(100, 1, 500000000, 100000000, 0, true);
    PresignedOrder invalid_order = make_order(200, 2, 500000000, 100000000, 0, false);

    pool.add_order(valid_order.client_order_id, valid_order);
    pool.add_order(invalid_order.client_order_id, invalid_order);

    CASE("2 orders added (1 valid, 1 invalid)", pool.size() == 2);
    CASE("valid_count = 1", pool.valid_count() == 1);

    // Price hasn't moved — the already-invalid order should remain invalid
    Level2Entry best_bid = make_l2_entry(499000000, 100000000);
    Level2Entry best_ask = make_l2_entry(500000000, 100000000);

    pool.check_and_invalidate(best_bid, best_ask);

    CASE("already-invalid order stays invalid", pool.valid_count() == 1);
    CASE("valid order stays valid (no price move)", pool.valid_count() == 1);

    return true;
}

/* ───── T3-5: configurable max_price_deviation_bps ───── */

static bool test_configurable_deviation() {
    // Strict pool: 100 bps (1%) max deviation
    PresignedOrderPool strict_pool(100);

    PresignedOrder order = make_order(100, 1, 500000000, 100000000, 0);
    strict_pool.add_order(order.client_order_id, order);

    // Ask = $0.505 → deviation = |0.505 - 0.50| / 0.50 * 10000 = 100 bps → exactly at limit
    Level2Entry best_bid = make_l2_entry(495000000, 100000000);
    Level2Entry best_ask = make_l2_entry(505000000, 100000000);

    strict_pool.check_and_invalidate(best_bid, best_ask);
    // 100 bps is NOT > 100 bps → should still be valid
    CASE("strict pool: 100bps deviation at 100bps limit → still valid",
         strict_pool.valid_count() == 1);

    // Ask = $0.52 → deviation = |0.52 - 0.50| / 0.50 * 10000 = 400 bps > 100 → invalid
    best_ask = make_l2_entry(520000000, 100000000);
    strict_pool.check_and_invalidate(best_bid, best_ask);
    CASE("strict pool: 400bps deviation at 100bps limit → invalidated",
         strict_pool.valid_count() == 0);

    return true;
}

/* ───── T3-5: zero price edge case ───── */

static bool test_zero_price_edge_case() {
    PresignedOrderPool pool(500);

    // Order with zero price — should not be invalidated (division by zero guard)
    PresignedOrder order = make_order(100, 1, 0, 100000000, 0);
    pool.add_order(order.client_order_id, order);

    Level2Entry best_bid = make_l2_entry(490000000, 100000000);
    Level2Entry best_ask = make_l2_entry(510000000, 100000000);

    pool.check_and_invalidate(best_bid, best_ask);

    CASE("zero-price order not invalidated (division guard)", pool.valid_count() == 1);

    return true;
}

/* ───── Main ───── */

int main() {
    std::cout << "📜 PresignedOrderPool — Phase 3 Test Suite (T3-5)" << std::endl;
    std::cout << "══════════════════════════════════════════════════════════" << std::endl;
    std::cout << std::endl;

    // T3-5: Cancelación de órdenes obsoletas
    run_test("T3-5: add_and_query",                        test_add_and_query);
    run_test("T3-5: check_and_invalidate_within_tolerance", test_check_and_invalidate_within_tolerance);
    run_test("T3-5: check_and_invalidate_exceeds_tolerance", test_check_and_invalidate_exceeds_tolerance);
    run_test("T3-5: check_and_invalidate_sell_side",        test_check_and_invalidate_sell_side);
    run_test("T3-5: invalidate_all",                        test_invalidate_all);
    run_test("T3-5: remove_order",                          test_remove_order);
    run_test("T3-5: check_and_invalidate_selective",        test_check_and_invalidate_selective);
    run_test("T3-5: check_and_invalidate_skip_invalid",     test_check_and_invalidate_skip_invalid);
    run_test("T3-5: configurable_deviation",                test_configurable_deviation);
    run_test("T3-5: zero_price_edge_case",                  test_zero_price_edge_case);

    std::cout << std::endl;
    std::cout << "══════════════════════════════════════════════════════════" << std::endl;

    // Print individual case results
    std::cout << std::endl;
    for (int i = 0; i < g_case_idx; i++) {
        std::cout << "  " << (g_cases[i].passed ? "✅" : "❌") << " "
                  << g_cases[i].name << std::endl;
    }

    std::cout << std::endl;
    int failed_tests = g_tests_run - g_tests_passed;
    int failed_cases = 0;
    for (int i = 0; i < g_case_idx; i++) {
        if (!g_cases[i].passed) failed_cases++;
    }

    std::cout << "══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Tests:  " << g_tests_passed << "/" << g_tests_run << " passed" << std::endl;
    std::cout << "Cases:  " << (g_case_idx - failed_cases) << "/" << g_case_idx
              << " passed (" << failed_cases << " failed)" << std::endl;
    std::cout << "══════════════════════════════════════════════════════════" << std::endl;

    if (failed_tests > 0 || failed_cases > 0) {
        std::cout << "❌ " << failed_tests << " test(s) failed, "
                  << failed_cases << " case(s) failed" << std::endl;
        return 1;
    }

    std::cout << "✅ All " << g_tests_passed << "/" << g_tests_run << " tests passed ("
              << g_case_idx << " assertion cases)" << std::endl;
    std::cout << "✅ PresignedOrderPool: price deviation invalidation, "
              << "invalidate_all, remove_order all verified." << std::endl;
    return 0;
}
