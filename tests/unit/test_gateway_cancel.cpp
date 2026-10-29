// ─────────────────────────────────────────────────────────────────────────────
// test_gateway_cancel: per-order withdrawal through the egress worker.
//
// The gateway used to expose only cancel_all — the nuclear option. A maker
// replacing one stale quote needs to pull ONE order, and the cancel must not be
// able to overtake the placement it targets, which is why both go through the
// same worker queue.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../../core/src/order_gateway.hpp"

static int g_failures = 0, g_tests = 0;
#define CHECK(cond, name)                                                     \
    do { ++g_tests; if (cond) { std::printf("  PASS %s\n", name); }            \
    else { std::printf("  FAIL %s (line %d)\n", name, __LINE__); ++g_failures; } } while (0)

// Minimal client satisfying the gateway's concept.
struct FakeClient {
    std::atomic<int> submits{0};
    std::atomic<int> cancels{0};
    char last_cancel[72]{};
    bool accept_cancel = true;

    SubmitResult submit(const WireBody&) { submits.fetch_add(1); return {}; }
    bool cancel_order(const char* id) {
        if (!accept_cancel) return false;
        cancels.fetch_add(1);
        std::snprintf(last_cancel, sizeof(last_cancel), "%s", id);
        return true;
    }
    size_t rest_get(const char*, char*, size_t) { return 0; }
    bool cancel_all() { return true; }
    uint64_t submissions() const { return (uint64_t)submits.load(); }
};

static const char* kId = "0x0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static void test_cancel_reaches_the_client() {
    std::printf("cancel_reaches_the_client\n");
    FakeClient c;
    OrderGateway<FakeClient> gw(c);
    gw.start();
    CHECK(gw.cancel_order(kId), "a valid id is enqueued");
    for (int i = 0; i < 100 && c.cancels.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(c.cancels.load() == 1, "the worker called cancel_order exactly once");
    CHECK(std::strcmp(c.last_cancel, kId) == 0, "with the right id");
    CHECK(gw.cancelled_orders() == 1, "and the counter reflects it");
    gw.stop(false);
}

static void test_invalid_ids_are_refused() {
    std::printf("invalid_ids_are_refused\n");
    FakeClient c;
    OrderGateway<FakeClient> gw(c);
    gw.start();
    CHECK(!gw.cancel_order(nullptr), "null is refused");
    CHECK(!gw.cancel_order(""), "empty is refused");
    CHECK(!gw.cancel_order("0x"), "bare prefix is refused");
    CHECK(!gw.cancel_order("abcdef"), "missing 0x is refused");
    CHECK(!gw.cancel_order("0xzz"), "non-hex is refused");
    CHECK(!gw.cancel_order("0x12\",\"evil\":\"1"), "JSON injection is refused");
    c.submits.load();
    CHECK(c.cancels.load() == 0, "no cancel ever reached the client");
    gw.stop(false);
}

static void test_cancel_survives_a_disabled_kill_switch() {
    std::printf("cancel_survives_disabled_kill_switch\n");
    // Withdrawing is risk-reducing. A kill switch that also blocks cancels would
    // trap open quotes exactly when the operator wants them gone.
    std::atomic<bool> trading_enabled{false};
    FakeClient c;
    OrderGateway<FakeClient> gw(c, &trading_enabled);
    gw.start();
    CHECK(gw.cancel_order(kId), "the cancel is still enqueued");
    for (int i = 0; i < 100 && c.cancels.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(c.cancels.load() == 1,
          "the cancel is executed even though trading is disabled");
    gw.stop(false);
}

static void test_rejected_cancel_is_not_counted() {
    std::printf("rejected_cancel_is_not_counted\n");
    FakeClient c;
    c.accept_cancel = false;
    OrderGateway<FakeClient> gw(c);
    gw.start();
    (void)gw.cancel_order(kId);
    for (int i = 0; i < 60; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(c.cancels.load() == 0, "the client refused");
    CHECK(gw.cancelled_orders() == 0,
          "a refused cancel is not counted as withdrawn");
    gw.stop(false);
}

int main() {
    std::printf("=== test_gateway_cancel ===\n");
    test_cancel_reaches_the_client();
    test_invalid_ids_are_refused();
    test_cancel_survives_a_disabled_kill_switch();
    test_rejected_cancel_is_not_counted();
    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
