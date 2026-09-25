/**
 * test_order_manager.cpp — Phase 3: Order Manager Test Suite
 *
 * Verifies (Phase 3: Order Manager, Reconciliación y Positions):
 *   T3-1: client_order_id generation is unique (salt + timestamp + counter)
 *   T3-1: register_order creates PENDING state
 *   T3-4: should_retry parses exchange response correctly (RETRY, SKIP, DUPLICATE)
 *   T3-4: should_retry consults local status before re-submitting
 *   T3-6: has_open_order detects self-trade (same market, same side)
 *
 * Also covers:
 *   - update_status
 *   - apply_fill (quantity, cash_value, status transitions)
 *   - find_order_copy
 *   - get_open_order_count
 *   - parse_exchange_status (static, extracted for testability)
 *
 * Dependencies: order_manager.hpp, lightweight_client.hpp (mockable via virtual),
 *   eip712_signer.hpp (OrderParams), market_config.hpp
 *
 * CRITICAL: Does NOT modify eip712_signer.hpp.
 */

#include "order_manager.hpp"
#include "lightweight_client.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

/* ───── Test framework (minimal, no external deps) ───── */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

struct TestCase {
    const char* name;
    bool passed;
};
static TestCase g_cases[8192];
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

/* ───── Mock CLOB Client ───── */

/**
 * MockCLOBClient: Inherits from LightweightCLOBClient to override
 * query_order_status and submit_order_with_response for testing.
 *
 * The base constructor with dummy credentials is safe — curl_easy_init
 * creates a handle but does NOT connect. No network I/O occurs at construction.
 */
class MockCLOBClient : public LightweightCLOBClient {
public:
    MockCLOBClient()
        : LightweightCLOBClient("test_key", "test_secret", "test_passphrase",
                                 "http://localhost:9999")
        , mock_status_response_(std::nullopt)
        , mock_submit_response_(std::nullopt)
    {}

    void set_status_response(const std::string& response) {
        mock_status_response_ = response;
    }
    void set_status_response_nullopt() {
        mock_status_response_ = std::nullopt;
    }

    void set_submit_response(const HttpResponse& response) {
        mock_submit_response_ = response;
    }

    std::optional<std::string> query_order_status(const std::string& client_order_id) override {
        (void)client_order_id;  // ignore real ID
        return mock_status_response_;
    }

    std::optional<HttpResponse> submit_order_with_response(const SignedOrder& order) override {
        (void)order;
        return mock_submit_response_;
    }

private:
    std::optional<std::string> mock_status_response_;
    std::optional<HttpResponse> mock_submit_response_;
};

/* ───── Test helpers ───── */

static OrderParams make_order_params(uint64_t salt = 123456789,
                                      uint64_t nonce = 987654321,
                                      uint64_t price = 500000000,  // $500.00 (* 1e6)
                                      uint64_t size = 100000000,   // 100 units (* 1e6)
                                      uint8_t side = 0) {           // 0 = buy, 1 = sell
    OrderParams params;
    params.salt = salt;
    params.nonce = nonce;
    params.price = price;
    params.size = size;
    params.side = side;
    memset(params.maker, 0x11, 20);
    memset(params.taker, 0x00, 20);
    return params;
}

static constexpr const char* MARKET_SLUG = "BTC-USD-UP";

/* ───── T3-1: client_order_id generation is unique ───── */

static bool test_client_order_id_uniqueness() {
    MockCLOBClient client;
    OrderManager mgr(client);

    // Generate multiple IDs with the same salt — they should all be unique
    // because the counter component increments atomically.
    std::set<std::string> ids;
    for (int i = 0; i < 1000; i++) {
        std::string id = mgr.generate_client_order_id(42);
        CASE("client_order_id is unique (set.insert succeeds)",
             ids.insert(id).second);
        // Verify format: salt-timestamp-counter (3 hex segments)
        int dash_count = 0;
        for (char c : id) {
            if (c == '-') dash_count++;
        }
        CASE("client_order_id has 2 dashes (salt-ts-counter format)", dash_count == 2);
    }

    CASE("generated 1000 unique IDs", ids.size() == 1000);

    // Verify the salt component is present
    std::string first_id = mgr.generate_client_order_id(0xDEADBEEF);
    CASE("client_order_id contains salt hex", first_id.find("deadbeef") != std::string::npos);

    return true;
}

static bool test_client_order_id_concurrent() {
    MockCLOBClient client;
    OrderManager mgr(client);

    // Generate IDs from multiple threads — all must be unique
    std::vector<std::thread> threads;
    std::vector<std::set<std::string>> thread_ids(4);
    std::atomic<int> idx{0};

    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 1000; i++) {
                std::string id = mgr.generate_client_order_id(t);
                thread_ids[t].insert(id);
            }
        });
    }

    for (auto& th : threads) th.join();

    // Collect all IDs and verify uniqueness
    std::set<std::string> all_ids;
    for (auto& s : thread_ids) {
        all_ids.insert(s.begin(), s.end());
    }

    CASE("concurrent client_order_id generation: 4000 IDs unique",
         all_ids.size() == 4000);

    return true;
}

/* ───── T3-1: register_order creates PENDING state ───── */

static bool test_register_order_pending() {
    MockCLOBClient client;
    OrderManager mgr(client);

    OrderParams params = make_order_params();
    std::string id = mgr.register_order(params, params.nonce, MARKET_SLUG);

    // Verify the returned ID is valid
    CASE("register_order returns non-empty ID", !id.empty());

    // Verify the order exists and is in PENDING state
    auto order_opt = mgr.find_order_copy(id);
    CASE("register_order creates order that is findable", order_opt.has_value());
    CASE("register_order creates PENDING status",
         order_opt.has_value() && order_opt->status == OrderStatus::PENDING);

    // Verify the order has correct field values
    if (order_opt) {
        CASE("order has correct market_slug", order_opt->market_slug == MARKET_SLUG);
        CASE("order has correct nonce", order_opt->nonce == params.nonce);
        CASE("order has correct salt", order_opt->params.salt == params.salt);
        CASE("order fills_quantity is 0 after registration", order_opt->fills_quantity == 0);
        CASE("order fills_cash_value is 0 after registration", order_opt->fills_cash_value == 0);
        CASE("order is_open() is false for PENDING", !order_opt->is_open());
        CASE("order is_terminal() is false for PENDING", !order_opt->is_terminal());
    }

    return true;
}

/* ───── update_status ───── */

static bool test_update_status() {
    MockCLOBClient client;
    OrderManager mgr(client);

    OrderParams params = make_order_params();
    std::string id = mgr.register_order(params, params.nonce, MARKET_SLUG);

    // Update to OPEN
    mgr.update_status(id, OrderStatus::OPEN);
    auto order = mgr.find_order_copy(id);
    CASE("update_status to OPEN", order.has_value() && order->status == OrderStatus::OPEN);
    CASE("OPEN order is_open() is true", order.has_value() && order->is_open());

    // Update to PARTIAL
    mgr.update_status(id, OrderStatus::PARTIAL);
    order = mgr.find_order_copy(id);
    CASE("update_status to PARTIAL", order.has_value() && order->status == OrderStatus::PARTIAL);
    CASE("PARTIAL order is_open() is true", order.has_value() && order->is_open());

    // Update to FILLED
    mgr.update_status(id, OrderStatus::FILLED);
    order = mgr.find_order_copy(id);
    CASE("update_status to FILLED", order.has_value() && order->status == OrderStatus::FILLED);
    CASE("FILLED order is_terminal() is true", order.has_value() && order->is_terminal());
    CASE("FILLED order is_open() is false", order.has_value() && !order->is_open());

    return true;
}

/* ───── apply_fill ───── */

static bool test_apply_fill() {
    MockCLOBClient client;
    OrderManager mgr(client);

    OrderParams params = make_order_params(
        123456, 789,
        500000000,  // price $500.00 (* 1e6)
        100000000   // size 100 units (* 1e6)
    );
    std::string id = mgr.register_order(params, params.nonce, MARKET_SLUG);

    // Apply a partial fill (50 out of 100 units)
    // fill_quantity is in * 1e6 units: 50 * 1e6 = 50000000
    mgr.apply_fill(id, 50000000, 25000000);  // 50 units, $25 cash value

    auto order = mgr.find_order_copy(id);
    CASE("apply_fill updates fills_quantity",
         order.has_value() && order->fills_quantity == 50000000);
    CASE("apply_fill updates fills_cash_value",
         order.has_value() && order->fills_cash_value == 25000000);
    CASE("apply_fill partial → PARTIAL status",
         order.has_value() && order->status == OrderStatus::PARTIAL);

    // Apply more fills to complete the order (remaining 50 units)
    mgr.apply_fill(id, 50000000, 25000000);
    order = mgr.find_order_copy(id);
    CASE("apply_fill completed → FILLED status",
         order.has_value() && order->status == OrderStatus::FILLED);
    CASE("apply_fill total fills_quantity == params.size",
         order.has_value() && order->fills_quantity == params.size);

    return true;
}

/* ───── T3-6: has_open_order / self-trade detection ───── */

static bool test_has_open_order_self_trade() {
    MockCLOBClient client;
    OrderManager mgr(client);

    // Register a BUY order for market "BTC-USD-UP"
    OrderParams buy_order = make_order_params(111, 100, 500000000, 100000000, 0); // side=0 (buy)
    std::string buy_id = mgr.register_order(buy_order, buy_order.nonce, MARKET_SLUG);
    mgr.update_status(buy_id, OrderStatus::OPEN);

    // Self-trade: another BUY order for the same market → should detect
    CASE("has_open_order detects buy/buy self-trade",
         mgr.has_open_order(MARKET_SLUG, true) == true);

    // SELL order for the same market → should NOT detect (different side)
    CASE("has_open_order does NOT detect buy/sell (different side)",
         mgr.has_open_order(MARKET_SLUG, false) == false);

    // Different market → should NOT detect
    CASE("has_open_order does NOT detect different market",
         mgr.has_open_order("ETH-USD-UP", true) == false);

    // After cancelling the buy order → should NOT detect
    mgr.update_status(buy_id, OrderStatus::CANCELLED);
    CASE("has_open_order after cancel → false",
         mgr.has_open_order(MARKET_SLUG, true) == false);

    // Register a SELL order
    OrderParams sell_order = make_order_params(222, 200, 500000000, 100000000, 1); // side=1 (sell)
    std::string sell_id = mgr.register_order(sell_order, sell_order.nonce, MARKET_SLUG);
    mgr.update_status(sell_id, OrderStatus::OPEN);

    // Self-trade: another SELL order for same market → should detect
    CASE("has_open_order detects sell/sell self-trade",
         mgr.has_open_order(MARKET_SLUG, false) == true);
    CASE("has_open_order does NOT detect sell/buy (different side)",
         mgr.has_open_order(MARKET_SLUG, true) == false);

    return true;
}

/* ───── T3-6: has_open_order with PARTIAL status ───── */

static bool test_has_open_order_partial() {
    MockCLOBClient client;
    OrderManager mgr(client);

    OrderParams params = make_order_params(333, 300, 500000000, 100000000, 0);
    std::string id = mgr.register_order(params, params.nonce, "ETH-USD-UP");

    // PENDING → not open
    CASE("PENDING order is not 'open' for self-trade",
         mgr.has_open_order("ETH-USD-UP", true) == false);

    // PARTIAL → open
    mgr.update_status(id, OrderStatus::PARTIAL);
    CASE("PARTIAL order is open for self-trade",
         mgr.has_open_order("ETH-USD-UP", true) == true);

    // FILLED → not open
    mgr.update_status(id, OrderStatus::FILLED);
    CASE("FILLED order is not open for self-trade",
         mgr.has_open_order("ETH-USD-UP", true) == false);

    return true;
}

/* ───── find_order_copy ───── */

static bool test_find_order_copy() {
    MockCLOBClient client;
    OrderManager mgr(client);

    OrderParams params = make_order_params(444, 400);
    std::string id = mgr.register_order(params, params.nonce, "TEST-MARKET");

    // Found
    auto found = mgr.find_order_copy(id);
    CASE("find_order_copy returns order for valid ID", found.has_value());

    // Not found
    auto not_found = mgr.find_order_copy("nonexistent_id");
    CASE("find_order_copy returns nullopt for invalid ID", !not_found);

    // Returned copy is independent (modifying copy doesn't affect stored order)
    if (found) {
        found->status = OrderStatus::CANCELLED;
        auto refound = mgr.find_order_copy(id);
        CASE("find_order_copy returns a copy (not a reference)",
             refound->status == OrderStatus::PENDING);
    }

    return true;
}

/* ───── get_open_order_count ───── */

static bool test_get_open_order_count() {
    MockCLOBClient client;
    OrderManager mgr(client);

    CASE("get_open_order_count starts at 0", mgr.get_open_order_count() == 0);

    // Register 5 orders
    for (int i = 0; i < 5; i++) {
        OrderParams params = make_order_params(1000 + i, 2000 + i);
        std::string id = mgr.register_order(params, params.nonce, "MARKET-" + std::to_string(i));
        mgr.update_status(id, OrderStatus::OPEN);
    }
    CASE("get_open_order_count is 5 after opening 5 orders",
         mgr.get_open_order_count() == 5);

    // Cancel 2
    auto open_orders = mgr.get_open_orders();
    CASE("get_open_orders returns 5 pointers", open_orders.size() == 5);
    if (open_orders.size() >= 2) {
        mgr.update_status(open_orders[0]->client_order_id, OrderStatus::CANCELLED);
        mgr.update_status(open_orders[1]->client_order_id, OrderStatus::CANCELLED);
    }
    CASE("get_open_order_count is 3 after cancelling 2", mgr.get_open_order_count() == 3);

    return true;
}

/* ───── T3-4: parse_exchange_status (static, testable) ───── */

static bool test_parse_exchange_status() {
    // Test each status string mapping
    CASE("parse_exchange_status: filled",
         OrderManager::parse_exchange_status("{\"status\":\"filled\",\"order_id\":\"abc\"}") == OrderStatus::FILLED);
    CASE("parse_exchange_status: partially_filled",
         OrderManager::parse_exchange_status("{\"status\":\"partially_filled\",\"size\":50}") == OrderStatus::PARTIAL);
    CASE("parse_exchange_status: open",
         OrderManager::parse_exchange_status("{\"status\":\"open\",\"size\":100}") == OrderStatus::OPEN);
    CASE("parse_exchange_status: canceled",
         OrderManager::parse_exchange_status("{\"status\":\"canceled\",\"reason\":\"user\"}") == OrderStatus::CANCELLED);
    CASE("parse_exchange_status: rejected",
         OrderManager::parse_exchange_status("{\"status\":\"rejected\",\"error\":\"bad sig\"}") == OrderStatus::REJECTED);
    CASE("parse_exchange_status: error",
         OrderManager::parse_exchange_status("{\"status\":\"error\",\"message\":\"oops\"}") == OrderStatus::REJECTED);

    // Unknown / empty
    CASE("parse_exchange_status: unknown string → UNKNOWN",
         OrderManager::parse_exchange_status("{\"status\":\"weird\"}") == OrderStatus::UNKNOWN);
    CASE("parse_exchange_status: empty string → UNKNOWN",
         OrderManager::parse_exchange_status("") == OrderStatus::UNKNOWN);
    CASE("parse_exchange_status: no status field → UNKNOWN",
         OrderManager::parse_exchange_status("{\"order_id\":\"abc\"}") == OrderStatus::UNKNOWN);

    return true;
}

/* ───── T3-4: should_retry — RETRY when order not found ───── */

static bool test_should_retry_order_not_found() {
    MockCLOBClient client;
    OrderManager mgr(client);

    // Order not registered locally, exchange returns nullopt (network failure)
    // → should_retry returns RETRY (not found anywhere → safe to retry)
    std::string id = mgr.generate_client_order_id(42);

    // Simulate exchange returning nullopt (network failure / no response)
    client.set_status_response_nullopt();
    auto decision = mgr.should_retry(id);
    CASE("should_retry: order not found + nullopt → RETRY",
         decision == OrderManager::RetryDecision::RETRY);

    return true;
}

/* ───── T3-4: should_retry — DUPLICATE when exchange says FILLED ───── */

static bool test_should_retry_filled_on_exchange() {
    MockCLOBClient client;
    OrderManager mgr(client);

    // Register an order locally
    OrderParams params = make_order_params(555, 600);
    std::string id = mgr.register_order(params, params.nonce, MARKET_SLUG);

    // Exchange says it's filled
    client.set_status_response("{\"status\":\"filled\",\"order_id\":\"" + id + "\"}");
    auto decision = mgr.should_retry(id);
    CASE("should_retry: exchange FILLED → DUPLICATE",
         decision == OrderManager::RetryDecision::DUPLICATE);

    // Verify local state was updated to FILLED
    auto order = mgr.find_order_copy(id);
    CASE("should_retry synced local status to FILLED",
         order.has_value() && order->status == OrderStatus::FILLED);

    return true;
}

/* ───── T3-4: should_retry — SKIP when exchange says OPEN ───── */

static bool test_should_retry_open_on_exchange() {
    MockCLOBClient client;
    OrderManager mgr(client);

    OrderParams params = make_order_params(777, 800);
    std::string id = mgr.register_order(params, params.nonce, MARKET_SLUG);

    // Exchange says it's still open
    client.set_status_response("{\"status\":\"open\",\"size\":100}");
    auto decision = mgr.should_retry(id);
    CASE("should_retry: exchange OPEN → SKIP",
         decision == OrderManager::RetryDecision::SKIP);

    // Verify local state was synced to OPEN
    auto order = mgr.find_order_copy(id);
    CASE("should_retry synced local status to OPEN",
         order.has_value() && order->status == OrderStatus::OPEN);

    return true;
}

/* ───── T3-4: should_retry — local FILLED overrides nullopt exchange ───── */

static bool test_should_retry_local_filled() {
    MockCLOBClient client;
    OrderManager mgr(client);

    OrderParams params = make_order_params(888, 900);
    std::string id = mgr.register_order(params, params.nonce, MARKET_SLUG);

    // Mark locally as FILLED (e.g., we received fill via user-channel)
    mgr.update_status(id, OrderStatus::FILLED);
    mgr.apply_fill(id, params.size, params.size * params.price / 1e6);

    // Exchange returns nullopt (network timeout)
    client.set_status_response_nullopt();
    auto decision = mgr.should_retry(id);
    CASE("should_retry: local FILLED + nullopt exchange → DUPLICATE",
         decision == OrderManager::RetryDecision::DUPLICATE);

    return true;
}

/* ───── T3-4: should_retry — PARTIAL on exchange → DUPLICATE ───── */

static bool test_should_retry_partial_on_exchange() {
    MockCLOBClient client;
    OrderManager mgr(client);

    OrderParams params = make_order_params(999, 1000);
    std::string id = mgr.register_order(params, params.nonce, MARKET_SLUG);

    // Apply a partial fill locally
    mgr.apply_fill(id, 50000000, 25000000);  // 50 out of 100 units

    // Exchange says partially_filled
    // PARTIAL is not terminal, so should_retry returns SKIP (order still on exchange)
    client.set_status_response("{\"status\":\"partially_filled\",\"size\":50}");
    auto decision = mgr.should_retry(id);
    CASE("should_retry: exchange PARTIAL → SKIP (order still on exchange)",
         decision == OrderManager::RetryDecision::SKIP);

    // Verify local state was synced to PARTIAL
    auto order = mgr.find_order_copy(id);
    CASE("should_retry synced local status to PARTIAL",
         order.has_value() && order->status == OrderStatus::PARTIAL);

    return true;
}

/* ───── T3-4: should_retry — CANCELLED on exchange → DUPLICATE ───── */

static bool test_should_retry_cancelled() {
    MockCLOBClient client;
    OrderManager mgr(client);

    OrderParams params = make_order_params(1111, 1200);
    std::string id = mgr.register_order(params, params.nonce, MARKET_SLUG);

    client.set_status_response("{\"status\":\"canceled\"}");
    auto decision = mgr.should_retry(id);
    CASE("should_retry: exchange CANCELLED → DUPLICATE",
         decision == OrderManager::RetryDecision::DUPLICATE);

    return true;
}

/* ───── T3-4: should_retry — REJECTED on exchange → DUPLICATE ───── */

static bool test_should_retry_rejected() {
    MockCLOBClient client;
    OrderManager mgr(client);

    OrderParams params = make_order_params(1212, 1300);
    std::string id = mgr.register_order(params, params.nonce, MARKET_SLUG);

    // Exchange says rejected — order exists on exchange but was rejected.
    // REJECTED without fills → SKIP (order known to exchange, not a duplicate fill)
    client.set_status_response("{\"status\":\"rejected\",\"error\":\"invalid signature\"}");
    auto decision = mgr.should_retry(id);
    CASE("should_retry: exchange REJECTED (no fills) → SKIP",
         decision == OrderManager::RetryDecision::SKIP);

    // Verify local state was synced to REJECTED
    auto order = mgr.find_order_copy(id);
    CASE("should_retry synced local status to REJECTED",
         order.has_value() && order->status == OrderStatus::REJECTED);

    return true;
}

/* ───── T3-4: should_retry — order not in local tracker but on exchange ───── */

static bool test_should_retry_not_local_but_on_exchange() {
    MockCLOBClient client;
    OrderManager mgr(client);

    // Don't register the order locally
    std::string id = mgr.generate_client_order_id(777);

    // But exchange says it's open
    client.set_status_response("{\"status\":\"open\"}");
    auto decision = mgr.should_retry(id);
    CASE("should_retry: not local + exchange OPEN → SKIP",
         decision == OrderManager::RetryDecision::SKIP);

    return true;
}

/* ───── Main ───── */

int main() {
    std::cout << "📋 OrderManager — Phase 3 Test Suite" << std::endl;
    std::cout << "══════════════════════════════════════════════════════════" << std::endl;
    std::cout << std::endl;

    // T3-1: client_order_id tracking
    run_test("T3-1: generate_client_order_id_unique",      test_client_order_id_uniqueness);
    run_test("T3-1: generate_client_order_id_concurrent",  test_client_order_id_concurrent);
    run_test("T3-1: register_order_pending_state",         test_register_order_pending);

    // Status management
    run_test("T3-2: update_status_transitions",            test_update_status);
    run_test("T3-2: apply_fill_partial_and_complete",       test_apply_fill);

    // T3-4: Anti-retry
    run_test("T3-4: parse_exchange_status",                  test_parse_exchange_status);
    run_test("T3-4: should_retry_order_not_found",           test_should_retry_order_not_found);
    run_test("T3-4: should_retry_filled_on_exchange",        test_should_retry_filled_on_exchange);
    run_test("T3-4: should_retry_open_on_exchange",          test_should_retry_open_on_exchange);
    run_test("T3-4: should_retry_local_filled",              test_should_retry_local_filled);
    run_test("T3-4: should_retry_partial_on_exchange",       test_should_retry_partial_on_exchange);
    run_test("T3-4: should_retry_cancelled_on_exchange",     test_should_retry_cancelled);
    run_test("T3-4: should_retry_rejected_on_exchange",      test_should_retry_rejected);
    run_test("T3-4: should_retry_not_local_but_on_exchange", test_should_retry_not_local_but_on_exchange);

    // T3-6: Self-trade detection
    run_test("T3-6: has_open_order_self_trade_detection",    test_has_open_order_self_trade);
    run_test("T3-6: has_open_order_partial_status",          test_has_open_order_partial);

    // Additional
    run_test("T3-2: find_order_copy",                        test_find_order_copy);
    run_test("T3-6: get_open_order_count",                   test_get_open_order_count);

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
    std::cout << "✅ OrderManager: client_order_id, register, apply_fill, should_retry, "
              << "self-trade detection all verified." << std::endl;
    return 0;
}
