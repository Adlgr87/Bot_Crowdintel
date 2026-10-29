// ─────────────────────────────────────────────────────────────────────────────
// test_resting_client: the engine-facing transport
//
// MockCLOBClient emits a FILL on every accepted submit. This one emits OPEN and
// only fills when the market crosses the quote, so a canary sees what a real
// maker sees.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>
#include <cstring>

#include "../../core/include/resting_mock_client.hpp"

static int g_failures = 0;
static int g_tests = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);          \
                ++g_failures; }                                                \
    } while (0)

using namespace crowdintel;

static WireBody make_body(const char* side, uint64_t maker_usdc,
                          uint64_t taker_shares, uint64_t price) {
    WireBody b{};
    std::snprintf(b.buf, sizeof(b.buf),
                  R"({"orderType":"GTC","side":"%s","price":%llu,)"
                  R"("makerAmount":%llu,"takerAmount":%llu})",
                  side, static_cast<unsigned long long>(price),
                  static_cast<unsigned long long>(maker_usdc),
                  static_cast<unsigned long long>(taker_shares));
    b.len = std::strlen(b.buf);
    return b;
}

static void test_submit_rests_not_fills() {
    std::printf("submit_rests_not_fills\n");
    SPSC_RingBuffer<AccountEvent> q;
    RestingMockCLOBClient c;
    c.attach_fill_queue(&q);

    // BUY: makerAmount = USDC cost, takerAmount = shares.
    const WireBody b = make_body("BUY", 88000000ULL, 200000000ULL, 440000);
    const auto r = c.submit(b);
    CHECK(r.ok, "the order is accepted");
    CHECK(c.paper_fills() == 0, "NO fill was synthesised");
    CHECK(c.resting() == 1, "the order is resting");
    CHECK(c.opens() == 1, "one OPEN counted");

    AccountEvent ev{};
    CHECK(q.try_pop(ev), "the venue emitted an event");
    CHECK(ev.type == AccountEvent::Type::OPEN, "and that event is OPEN, not FILL");
    CHECK(ev.side == K_SIDE_BUY, "carrying the BUY side");
    CHECK(ev.price == 440000u, "and the quoted price");
    CHECK(ev.size == 200000000ULL, "and the size");
}

static void test_fill_only_when_crossed() {
    std::printf("fill_only_when_crossed\n");
    SPSC_RingBuffer<AccountEvent> q;
    RestingMockCLOBClient c;
    c.attach_fill_queue(&q);
    (void)c.submit(make_body("BUY", 88000000ULL, 200000000ULL, 440000));
    AccountEvent ev{};
    (void)q.try_pop(ev);  // drain the OPEN

    CHECK(!c.observe_book(445000, 100000000ULL, 455000, 90000000ULL),
          "an untouched rest produces no fill");
    CHECK(c.paper_fills() == 0, "still no fill");

    CHECK(c.observe_book(430000, 100000000ULL, 440000, 50000000ULL),
          "a crossing book fills the rest");
    CHECK(c.paper_fills() == 1, "one fill counted");
    CHECK(q.try_pop(ev), "a fill event was emitted");
    CHECK(ev.type == AccountEvent::Type::FILL, "of type FILL");
    CHECK(ev.price == 440000u, "at the maker's own price");
    CHECK(ev.size == 50000000ULL, "for the crossed size");
    CHECK(ev.remaining == 150000000ULL, "reporting the outstanding remainder");

    // The remainder keeps resting and can fill again.
    while (c.observe_book(430000, 0, 440000, 500000000ULL)) {
    }
    CHECK(c.resting() == 0, "the rest is exhausted after the final fill");
}

static void test_cancel_emits_event() {
    std::printf("cancel_emits_event\n");
    SPSC_RingBuffer<AccountEvent> q;
    RestingMockCLOBClient c;
    c.attach_fill_queue(&q);
    (void)c.submit(make_body("SELL", 200000000ULL, 92000000ULL, 460000));
    AccountEvent ev{};
    (void)q.try_pop(ev);  // OPEN

    CHECK(c.cancel_order_id(1), "cancel by submit id succeeds");
    CHECK(c.resting() == 0, "the rest is withdrawn");
    CHECK(q.try_pop(ev) && ev.type == AccountEvent::Type::CANCEL,
          "a CANCEL event was emitted");
    CHECK(!c.observe_book(470000, 100000000ULL, 480000, 100000000ULL),
          "a cancelled rest cannot fill afterwards");
}

static void test_rejects_malformed() {
    std::printf("rejects_malformed\n");
    SPSC_RingBuffer<AccountEvent> q;
    RestingMockCLOBClient c;
    c.attach_fill_queue(&q);

    WireBody empty{};
    std::snprintf(empty.buf, sizeof(empty.buf), "{}");
    empty.len = std::strlen(empty.buf);
    const auto r = c.submit(empty);
    CHECK(!r.ok, "a body with no side/amounts is rejected");
    AccountEvent ev{};
    CHECK(q.try_pop(ev) && ev.type == AccountEvent::Type::REJECT,
          "and a REJECT event is emitted");
    CHECK(c.resting() == 0, "nothing was left resting");
}

int main() {
    std::printf("=== test_resting_client ===\n");
    test_submit_rests_not_fills();
    test_fill_only_when_crossed();
    test_cancel_emits_event();
    test_rejects_malformed();
    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
