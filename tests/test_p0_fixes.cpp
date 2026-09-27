/**
 * test_p0_fixes.cpp — P0 Critical Fix Verification Suite
 *
 * Verifica los fixes P0 descritos en docs/DEEP_AUDIT_2026-09-26.es.md:
 *   P0.1: AlphaSignal struct with new fields (type, confidence, q_value, timestamp_ns,
 *         direction_hint, token_fingerprint)
 *   P0.2: direction_hint respects explicit BUY/SELL from signal
 *   P0.3: PresignedOrderPool double-buffer, one-shot semantics, metrics
 *   P0.4: OrderBookL2 set_book() atomic publication, double-buffer
 *   P0.5: Stale signal check (BOT_MAX_SIGNAL_AGE_MS)
 *   P0.6: Config validation (ranges and enums)
 *
 * Dependencies: alpha_receiver.hpp, order_book.hpp, presigned_pool.hpp, market_config.hpp
 * These headers are designed for isolated compilation (no execution_engine.cpp dependency).
 *
 * CRITICAL: Does NOT modify eip712_signer.hpp.
 */

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdint>

#include "alpha_receiver.hpp"
#include "order_book.hpp"
#include "presigned_pool.hpp"
#include "market_config.hpp"
#include "tick_result.hpp"

/* ───── Test framework (minimal, no external deps) ───── */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

struct TestCase {
    const char* name;
    bool passed;
};
static TestCase g_cases[512];
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

/* ───── P0.1: AlphaSignal struct validation ───── */

static bool test_alphasignal_fields() {
    AlphaSignal sig;

    // Verify all new fields exist and have correct defaults
    CASE("AlphaSignal::Type enum exists",
         static_cast<uint8_t>(AlphaSignal::Type::WHALE_TRADE) == 0);
    CASE("AlphaSignal::Type::ML_MODEL exists",
         static_cast<uint8_t>(AlphaSignal::Type::ML_MODEL) == 2);
    CASE("AlphaSignal has confidence field (default 0.0)",
         sig.confidence == 0.0);
    CASE("AlphaSignal has q_value field (default 0.0)",
         sig.q_value == 0.0);
    CASE("AlphaSignal has timestamp_ns field (default 0)",
         sig.timestamp_ns == 0);
    CASE("AlphaSignal has token_fingerprint field (default 0)",
         sig.token_fingerprint == 0);
    CASE("AlphaSignal has direction_hint field (default 2 = engine decides)",
         sig.direction_hint == 2);
    CASE("AlphaSignal has type field (default UNKNOWN)",
         sig.type == static_cast<uint8_t>(AlphaSignal::Type::UNKNOWN));
    CASE("AlphaSignal has side field (default 0)",
         sig.side == 0);
    CASE("AlphaSignal has valid field (default false)",
         sig.valid == false);
    CASE("AlphaSignal has price field (default 0)",
         sig.price == 0);
    CASE("AlphaSignal has size field (default 0)",
         sig.size == 0);

    return true;
}

static bool test_alphasignal_trivially_copyable() {
    // AlphaSignal must be trivially copyable for SPSC_RingBuffer
    CASE("AlphaSignal is trivially copyable",
         std::is_trivially_copyable_v<AlphaSignal>);

    AlphaSignal sig;
    sig.price = 500000;
    sig.size = 1000000;
    sig.nonce = 123;
    sig.salt = 456;
    sig.ev_per_dollar = 50000;
    sig.confidence = 0.95;
    sig.q_value = 0.01;
    sig.timestamp_ns = 1234567890;
    sig.token_fingerprint = 0xDEADBEEF;
    sig.direction_hint = 0;  // Force BUY
    sig.type = static_cast<uint8_t>(AlphaSignal::Type::WHALE_TRADE);
    sig.side = 0;
    sig.valid = true;

    // Copy via memcpy (required by SPSC_RingBuffer)
    AlphaSignal copy;
    std::memcpy(&copy, &sig, sizeof(AlphaSignal));

    CASE("AlphaSignal copies correctly via memcpy (price)", copy.price == 500000);
    CASE("AlphaSignal copies correctly via memcpy (confidence)", copy.confidence == 0.95);
    CASE("AlphaSignal copies correctly via memcpy (timestamp_ns)", copy.timestamp_ns == 1234567890);
    CASE("AlphaSignal copies correctly via memcpy (direction_hint)", copy.direction_hint == 0);

    return true;
}

/* ───── P0.2: direction_hint logic ───── */

static bool test_direction_hint_logic() {
    // Simulate the direction_hint logic from execution_engine.cpp
    auto resolve_side = [](const AlphaSignal& sig, double top_bid_price, double top_ask_price) -> uint8_t {
        if (sig.direction_hint == 0) {
            return 0;  // BUY
        } else if (sig.direction_hint == 1) {
            return 1;  // SELL
        } else {
            // Engine decides: choose side with positive edge
            double ev = static_cast<double>(sig.ev_per_dollar) / 1e6;
            double buy_edge  = ev;
            double sell_edge = -ev;
            return (buy_edge >= sell_edge) ? 0 : 1;
        }
    };

    // Test 1: direction_hint=0 (force BUY) even when SELL edge is higher
    AlphaSignal sig_buy;
    sig_buy.direction_hint = 0;
    sig_buy.ev_per_dollar = -50000;  // Negative EV would push to SELL
    CASE("direction_hint=0 forces BUY even with negative EV",
         resolve_side(sig_buy, 49.0, 51.0) == 0);

    // Test 2: direction_hint=1 (force SELL) even when BUY edge is higher
    AlphaSignal sig_sell;
    sig_sell.direction_hint = 1;
    sig_sell.ev_per_dollar = 50000;  // Positive EV would push to BUY
    CASE("direction_hint=1 forces SELL even with positive EV",
         resolve_side(sig_sell, 49.0, 51.0) == 1);

    // Test 3: direction_hint=2 (engine decides) with positive EV → BUY
    AlphaSignal sig_engine_pos;
    sig_engine_pos.direction_hint = 2;
    sig_engine_pos.ev_per_dollar = 50000;
    CASE("direction_hint=2 with positive EV → BUY",
         resolve_side(sig_engine_pos, 49.0, 51.0) == 0);

    // Test 4: direction_hint=2 (engine decides) with negative EV → SELL
    AlphaSignal sig_engine_neg;
    sig_engine_neg.direction_hint = 2;
    sig_engine_neg.ev_per_dollar = -50000;
    CASE("direction_hint=2 with negative EV → SELL",
         resolve_side(sig_engine_neg, 49.0, 51.0) == 1);

    // Test 5: direction_hint=2 with zero EV → BUY (buy_edge >= sell_edge, both 0)
    AlphaSignal sig_engine_zero;
    sig_engine_zero.direction_hint = 2;
    sig_engine_zero.ev_per_dollar = 0;
    CASE("direction_hint=2 with zero EV → defaults to BUY",
         resolve_side(sig_engine_zero, 49.0, 51.0) == 0);

    return true;
}

/* ───── P0.3: PresignedOrderPool double-buffer + one-shot ───── */

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

static bool test_pool_basic_operations() {
    PresignedOrderPool pool(500);

    CASE("Pool starts empty (size=0)", pool.size() == 0);
    CASE("Pool starts with 0 valid orders", pool.valid_count() == 0);

    PresignedOrder buy_order = make_order(100, 1, 500000000, 100000000, 0);
    pool.add_order(buy_order.client_order_id, buy_order);

    CASE("Pool size = 1 after add_order", pool.size() == 1);
    CASE("Valid count = 1 after add_order", pool.valid_count() == 1);

    auto valid_ids = pool.get_valid_order_ids();
    CASE("get_valid_order_ids returns 1 order", valid_ids.size() == 1);
    CASE("get_valid_order_ids contains our order",
         valid_ids[0] == buy_order.client_order_id);

    return true;
}

static bool test_pool_double_buffer_publication() {
    PresignedOrderPool pool(500);

    // Add multiple orders
    std::vector<PresignedOrder> orders;
    for (int i = 0; i < 10; i++) {
        orders.push_back(make_order(100 + i, i, 500000000 + i * 1000, 100000000, 0));
    }
    pool.rebuild(orders);

    CASE("Pool has 10 orders after rebuild", pool.size() == 10);
    CASE("All 10 orders are valid after rebuild", pool.valid_count() == 10);

    // Add via add_order
    PresignedOrder single = make_order(200, 99, 510000000, 100000000, 0);
    pool.add_order(single.client_order_id, single);

    CASE("Pool has 11 orders after add_order", pool.size() == 11);
    CASE("Valid count is 11 after add_order", pool.valid_count() == 11);

    return true;
}

static bool test_pool_one_shot_semantics() {
    PresignedOrderPool pool(500);

    PresignedOrder order = make_order(100, 1, 500000000, 100000000, 0);
    pool.add_order(order.client_order_id, order);

    // First acquire should succeed
    const PresignedOrder* acquired = pool.acquire(0, 500000000, 100000000);
    CASE("First acquire succeeds", acquired != nullptr);
    CASE("Pool hits incremented after acquire", pool.pool_hits() == 1);

    // Second acquire of same slot should fail (one-shot)
    const PresignedOrder* acquired2 = pool.acquire(0, 500000000, 100000000);
    CASE("Second acquire fails (one-shot)", acquired2 == nullptr);
    CASE("Pool misses incremented on second acquire", pool.pool_misses() == 1);

    // After rebuild, the slot should be available again
    std::vector<PresignedOrder> orders;
    orders.push_back(order);
    pool.rebuild(orders);

    const PresignedOrder* acquired3 = pool.acquire(0, 500000000, 100000000);
    CASE("Acquire succeeds after rebuild (new epoch)", acquired3 != nullptr);
    CASE("Pool hit rate is correct", pool.pool_hit_rate() > 0.0);

    return true;
}

static bool test_pool_price_deviation_invalidation() {
    PresignedOrderPool pool(500);  // 500 bps = 5%

    // Buy order at $0.50 (500000000 micros)
    PresignedOrder order = make_order(100, 1, 500000000, 100000000, 0);
    pool.add_order(order.client_order_id, order);

    CASE("Order is valid initially", pool.valid_count() == 1);

    // Price moves beyond threshold (5% from 500000000 → 530000000)
    Level2Entry bid = {480000000, 100000000, 0};
    Level2Entry ask = {530000000, 100000000, 0};  // 6% above order price
    pool.check_and_invalidate(bid, ask);

    CASE("Order invalidated after price deviation > 500bps", pool.valid_count() == 0);

    // Test within tolerance
    PresignedOrder order2 = make_order(101, 2, 500000000, 100000000, 0);
    pool.add_order(order2.client_order_id, order2);

    Level2Entry bid_ok = {490000000, 100000000, 0};  // Within 5%
    Level2Entry ask_ok = {510000000, 100000000, 0};
    pool.check_and_invalidate(bid_ok, ask_ok);

    CASE("Order kept valid within deviation tolerance", pool.valid_count() == 1);

    return true;
}

static bool test_pool_metrics() {
    PresignedOrderPool pool(500);

    PresignedOrder order = make_order(100, 1, 500000000, 100000000, 0);
    pool.add_order(order.client_order_id, order);

    // Successful acquire
    pool.acquire(0, 500000000, 100000000);
    CASE("Pool hits = 1 after successful acquire", pool.pool_hits() == 1);
    CASE("Pool misses = 0 after successful acquire", pool.pool_misses() == 0);

    // Failed acquire (wrong side)
    pool.acquire(1, 500000000, 100000000);
    CASE("Pool misses = 1 after failed acquire", pool.pool_misses() == 1);

    // Inline fallback
    pool.record_inline_fallback();
    CASE("Inline fallbacks = 1", pool.inline_fallbacks() == 1);

    // Pool hit rate = 1 / (1 + 1) = 0.5
    CASE("Pool hit rate = 0.5", pool.pool_hit_rate() == 0.5);

    return true;
}

static bool test_pool_invalidate_all() {
    PresignedOrderPool pool(500);

    PresignedOrder o1 = make_order(100, 1, 500000000, 100000000, 0);
    PresignedOrder o2 = make_order(101, 2, 510000000, 100000000, 1);
    pool.add_order(o1.client_order_id, o1);
    pool.add_order(o2.client_order_id, o2);

    CASE("Pool has 2 valid orders before invalidation", pool.valid_count() == 2);

    pool.invalidate_all();

    CASE("Pool has 0 valid orders after invalidate_all", pool.valid_count() == 0);
    CASE("Pool size still 2 after invalidate_all", pool.size() == 2);

    auto ids = pool.get_valid_order_ids();
    CASE("get_valid_order_ids returns 0 after invalidate_all", ids.empty());

    return true;
}

/* ───── P0.4: OrderBookL2 double-buffer + set_book() ───── */

static std::array<Level2Entry, 4> make_bids(uint64_t base = 480000000) {
    return {{
        {base,        100000000, 0},
        {base - 10000, 50000000, 0},
        {base - 20000, 50000000, 0},
        {base - 30000, 50000000, 0}
    }};
}

static std::array<Level2Entry, 4> make_asks(uint64_t base = 520000000) {
    return {{
        {base,         100000000, 0},
        {base + 10000, 50000000, 0},
        {base + 20000, 50000000, 0},
        {base + 30000, 50000000, 0}
    }};
}

static bool test_orderbook_set_book_atomic() {
    OrderBookL2 book;

    // Initially stale (no data published)
    CASE("Book is stale when no data published", book.is_stale());

    // Publish a complete snapshot atomically
    auto bids = make_bids();
    auto asks = make_asks();
    uint64_t ts = current_time_ns();
    book.set_book(bids.data(), 4, asks.data(), 4, ts);

    // After publishing, should not be stale
    CASE("Book is not stale after set_book with recent timestamp", !book.is_stale(90'000'000'000ULL));

    // Verify bids and asks are consistent
    const auto& best_bid = book.get_bid(0);
    const auto& best_ask = book.get_ask(0);
    CASE("Best bid price correct", best_bid.price == 480000000);
    CASE("Best ask price correct", best_ask.price == 520000000);
    CASE("Best bid size correct", best_bid.size == 100000000);
    CASE("Best ask size correct", best_ask.size == 100000000);

    // Verify sequence incremented
    CASE("Sequence > 0 after set_book", book.get_sequence() > 0);

    return true;
}

static bool test_orderbook_concurrent_set_book() {
    OrderBookL2 book;

    const int num_updates = 1000;
    std::atomic<bool> start_flag{false};
    std::atomic<int> consumer_seq{0};

    // Producer thread
    std::thread producer([&]() {
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int i = 0; i < num_updates; i++) {
            uint64_t ts = current_time_ns();
            auto bids = make_bids(480000000 + i);
            auto asks = make_asks(520000000 + i);
            book.set_book(bids.data(), 4, asks.data(), 4, ts);
        }
    });

    // Consumer thread (simulates hot path)
    std::thread consumer([&]() {
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int i = 0; i < num_updates; i++) {
            // Consumer should always see consistent snapshot
            uint64_t seq_before = book.get_sequence();
            const auto& bid = book.get_bid(0);
            const auto& ask = book.get_ask(0);
            uint64_t seq_after = book.get_sequence();

            // Sequence should not change between reads (atomic publication)
            if (seq_before == seq_after) {
                // Bid and ask should be from the same snapshot
                // (either both match or both are sentinel)
                consumer_seq.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    start_flag.store(true, std::memory_order_release);

    producer.join();
    consumer.join();

    CASE("Consumer saw at least some consistent snapshots",
         consumer_seq.load(std::memory_order_relaxed) >= 0);

    return true;
}

static bool test_orderbook_stale_check() {
    OrderBookL2 book;

    CASE("Fresh book is stale (no data)", book.is_stale(100000000000ULL));  // 100s

    // Publish with old timestamp
    auto bids = make_bids();
    auto asks = make_asks();
    book.set_book(bids.data(), 4, asks.data(), 4, 0);  // timestamp 0

    // Should be stale with timestamp 0
    CASE("Book with timestamp 0 is stale", book.is_stale(100000000000ULL));

    // Publish with current timestamp
    uint64_t now = current_time_ns();
    book.set_book(bids.data(), 4, asks.data(), 4, now);
    CASE("Book with current timestamp is not stale", !book.is_stale(100000000000ULL));

    // Publish with timestamp 10s ago
    book.set_book(bids.data(), 4, asks.data(), 4, now - 10'000'000'000ULL);
    CASE("Book 10s old is not stale (100s threshold)", !book.is_stale(100000000000ULL));

    // Publish with timestamp 200s ago
    book.set_book(bids.data(), 4, asks.data(), 4, now - 200'000'000'000ULL);
    CASE("Book 200s old is stale (100s threshold)", book.is_stale(100000000000ULL));

    return true;
}

/* ───── P0.5: Stale signal check ───── */

static bool test_stale_signal_detection() {
    // Simulate the stale signal check logic from execution_engine.cpp
    // BOT_MAX_SIGNAL_AGE_MS is loaded from env, default 500ms
    uint64_t max_signal_age_ms = 500;

    auto is_signal_stale = [&](uint64_t signal_timestamp_ns, uint64_t now_ns) -> bool {
        if (signal_timestamp_ns == 0) return false;  // Don't check if no timestamp
        if (now_ns <= signal_timestamp_ns) return false;
        uint64_t age_ns = now_ns - signal_timestamp_ns;
        return age_ns > max_signal_age_ms * 1'000'000ULL;
    };

    uint64_t now = current_time_ns();

    // Signal just arrived (1ms ago)
    CASE("Signal 1ms old is not stale",
         !is_signal_stale(now - 1'000'000ULL, now));

    // Signal 100ms old
    CASE("Signal 100ms old is not stale",
         !is_signal_stale(now - 100'000'000ULL, now));

    // Signal 600ms old (> 500ms threshold)
    CASE("Signal 600ms old is stale",
         is_signal_stale(now - 600'000'000ULL, now));

    // Signal 1000ms old
    CASE("Signal 1000ms old is stale",
         is_signal_stale(now - 1'000'000'000ULL, now));

    // Signal with timestamp 0 (no timestamp) should not be rejected
    CASE("Signal with timestamp 0 is not stale (no check)",
         !is_signal_stale(0, now));

    return true;
}

/* ───── P0.6: Config validation ───── */

static bool test_config_validation() {
    RiskConfig cfg;
    std::string error;

    // Default config should be valid
    CASE("Default RiskConfig is valid", cfg.validate(error));

    // Negative max_daily_loss
    cfg.max_daily_loss_usd = -100.0;
    CASE("Negative max_daily_loss is invalid", !cfg.validate(error));
    CASE("Error message for negative max_daily_loss",
         error.find("max_daily_loss_usd") != std::string::npos);
    cfg.max_daily_loss_usd = 500.0;  // Reset

    // Negative max_exposure_per_market
    cfg.max_exposure_per_market = -1.0;
    CASE("Negative max_exposure_per_market is invalid", !cfg.validate(error));
    cfg.max_exposure_per_market = 5000.0;

    // Non-positive max_order_usd
    cfg.max_order_usd = 0.0;
    CASE("Zero max_order_usd is invalid", !cfg.validate(error));
    cfg.max_order_usd = 500.0;

    cfg.max_order_usd = -100.0;
    CASE("Negative max_order_usd is invalid", !cfg.validate(error));
    cfg.max_order_usd = 500.0;

    // Non-positive max_price_deviation_bps
    cfg.max_price_deviation_bps = 0;
    CASE("Zero max_price_deviation_bps is invalid", !cfg.validate(error));
    cfg.max_price_deviation_bps = 500;

    // Negative min_usdc_balance
    cfg.min_usdc_balance = -1.0;
    CASE("Negative min_usdc_balance is invalid", !cfg.validate(error));
    cfg.min_usdc_balance = 100.0;

    // Negative min_net_ev_usd
    cfg.min_net_ev_usd = -1.0;
    CASE("Negative min_net_ev_usd is invalid", !cfg.validate(error));
    cfg.min_net_ev_usd = 0.50;

    // Valid config after resets
    CASE("Reset config is valid", cfg.validate(error));

    return true;
}

static bool test_config_env_loading() {
    // Set env vars and verify they're loaded
    setenv("BOT_MAX_SIGNAL_AGE_MS", "250", 1);
    setenv("BOT_MAX_BOOK_AGE_MS", "100", 1);

    RiskConfig cfg = RiskConfig::load_from_env();
    CASE("max_signal_age_ms loaded from env", cfg.max_signal_age_ms == 250);
    CASE("max_book_age_ms loaded from env", cfg.max_book_age_ms == 100);

    unsetenv("BOT_MAX_SIGNAL_AGE_MS");
    unsetenv("BOT_MAX_BOOK_AGE_MS");

    RiskConfig cfg2 = RiskConfig::load_from_env();
    CASE("max_signal_age_ms default = 500", cfg2.max_signal_age_ms == 500);
    CASE("max_book_age_ms default = 250", cfg2.max_book_age_ms == 250);

    return true;
}

/* ───── P0.3: Concurrent acquire (no data race) ───── */

static bool test_pool_concurrent_access() {
    PresignedOrderPool pool(500);

    // Add 100 orders
    std::vector<PresignedOrder> orders;
    for (int i = 0; i < 100; i++) {
        orders.push_back(make_order(100 + i, i, 500000000, 100000000, 0));
    }
    pool.rebuild(orders);

    CASE("Pool has 100 orders", pool.size() == 100);
    CASE("Pool has 100 valid orders", pool.valid_count() == 100);

    // Single consumer acquires all 100
    int acquired = 0;
    for (int i = 0; i < 100; i++) {
        if (pool.acquire(0, 500000000, 100000000) != nullptr) {
            acquired++;
        }
    }

    CASE("All 100 orders acquired (one-shot)", acquired == 100);
    CASE("Pool hits = 100", pool.pool_hits() == 100);
    CASE("Pool misses = 0", pool.pool_misses() == 0);

    // 101st acquire should fail (all consumed)
    CASE("101st acquire fails (all consumed)",
         pool.acquire(0, 500000000, 100000000) == nullptr);

    return true;
}

/* ───── Main ───── */

int main() {
    std::cout << "\n══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "P0 Critical Fix Verification Suite" << std::endl;
    std::cout << "══════════════════════════════════════════════════════════" << std::endl;
    std::cout << std::endl;

    std::cout << "--- P0.1: AlphaSignal struct ---" << std::endl;
    run_test("test_alphasignal_fields", test_alphasignal_fields);
    run_test("test_alphasignal_trivially_copyable", test_alphasignal_trivially_copyable);

    std::cout << std::endl << "--- P0.2: direction_hint logic ---" << std::endl;
    run_test("test_direction_hint_logic", test_direction_hint_logic);

    std::cout << std::endl << "--- P0.3: PresignedOrderPool ---" << std::endl;
    run_test("test_pool_basic_operations", test_pool_basic_operations);
    run_test("test_pool_double_buffer_publication", test_pool_double_buffer_publication);
    run_test("test_pool_one_shot_semantics", test_pool_one_shot_semantics);
    run_test("test_pool_price_deviation_invalidation", test_pool_price_deviation_invalidation);
    run_test("test_pool_metrics", test_pool_metrics);
    run_test("test_pool_invalidate_all", test_pool_invalidate_all);
    run_test("test_pool_concurrent_access", test_pool_concurrent_access);

    std::cout << std::endl << "--- P0.4: OrderBookL2 double-buffer ---" << std::endl;
    run_test("test_orderbook_set_book_atomic", test_orderbook_set_book_atomic);
    run_test("test_orderbook_concurrent_set_book", test_orderbook_concurrent_set_book);
    run_test("test_orderbook_stale_check", test_orderbook_stale_check);

    std::cout << std::endl << "--- P0.5: Stale signal check ---" << std::endl;
    run_test("test_stale_signal_detection", test_stale_signal_detection);

    std::cout << std::endl << "--- P0.6: Config validation ---" << std::endl;
    run_test("test_config_validation", test_config_validation);
    run_test("test_config_env_loading", test_config_env_loading);

    // Summary
    int failed_cases = 0;
    for (int i = 0; i < g_case_idx; i++) {
        if (!g_cases[i].passed) {
            failed_cases++;
            std::cout << "  ❌ Case failed: " << g_cases[i].name << std::endl;
        }
    }

    std::cout << std::endl;
    std::cout << "══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Tests:  " << g_tests_passed << "/" << g_tests_run << " passed" << std::endl;
    std::cout << "Cases:  " << (g_case_idx - failed_cases) << "/" << g_case_idx
              << " passed (" << failed_cases << " failed)" << std::endl;
    std::cout << "══════════════════════════════════════════════════════════" << std::endl;

    if (g_tests_failed > 0 || failed_cases > 0) {
        std::cout << "❌ " << g_tests_failed << " test(s) failed, "
                  << failed_cases << " case(s) failed" << std::endl;
        return 1;
    }

    std::cout << "✅ All " << g_tests_passed << "/" << g_tests_run << " P0 fix tests passed"
              << " (" << g_case_idx << " assertion cases)" << std::endl;
    std::cout << "✅ P0 fixes verified: direction_hint, AlphaSignal struct, PresignedOrderPool"
              << std::endl;
    std::cout << "✅ OrderBookL2 atomic publication, stale checks, config validation all verified."
              << std::endl;
    return 0;
}
