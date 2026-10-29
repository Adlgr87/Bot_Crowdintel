// ─────────────────────────────────────────────────────────────────────────────
// test_resting_mock: the transport that makes adverse selection observable
//
// The whole point: an order RESTS, and fills only when the market crosses it.
// A fill therefore happens exactly when the price moved against the maker, which
// is the conditional mark-out the unconditional 0.092c figure cannot express.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>

#include "../../core/include/resting_mock.hpp"

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

static void test_places_and_rests() {
    std::printf("places_and_rests\n");
    RestingOrderBookMock m;
    const MockRestOutcome o = m.place(1, MOCK_SIDE_BID, 440000, 200000000ULL);
    CHECK(o.event == MockRestEvent::Opened, "a placement OPENS, it does not fill");
    CHECK(m.live_count() == 1, "the order is resting");
    CHECK(m.fills() == 0, "no fill was synthesised");
    CHECK(m.opens() == 1, "one open counted");
}

static void test_fills_only_on_cross() {
    std::printf("fills_only_on_cross\n");
    RestingOrderBookMock m;
    (void)m.place(1, MOCK_SIDE_BID, 440000, 200000000ULL);

    // Market above our bid: no fill.
    MockRestOutcome o = m.observe_book(445000, 100000000ULL, 455000, 90000000ULL);
    CHECK(o.event == MockRestEvent::None, "an untouched rest does not fill");
    CHECK(m.fills() == 0, "still no fill");

    // Ask comes down to our bid: a seller hits us. This is the adverse event.
    o = m.observe_book(430000, 100000000ULL, 440000, 50000000ULL);
    CHECK(o.event == MockRestEvent::Filled, "the crossed rest fills");
    CHECK(o.fill_price == 440000u, "the maker is paid its own quote");
    CHECK(o.filled_size == 50000000ULL, "filled by the liquidity that crossed");
    CHECK(m.fills() == 1, "one fill counted");
}

static void test_partial_then_full() {
    std::printf("partial_then_full\n");
    RestingOrderBookMock m;
    (void)m.place(2, MOCK_SIDE_BID, 440000, 200000000ULL);

    MockRestOutcome o = m.observe_book(430000, 0, 440000, 50000000ULL);
    CHECK(o.event == MockRestEvent::Filled && o.filled_size == 50000000ULL,
          "first cross fills 50 of 200");
    CHECK(m.live_count() == 1, "the remainder is still resting");
    CHECK(m.rest_size(MOCK_SIDE_BID) && *m.rest_size(MOCK_SIDE_BID) == 150000000ULL,
          "the rest shrank to 150");

    o = m.observe_book(430000, 0, 440000, 500000000ULL);
    CHECK(o.filled_size == 150000000ULL, "second cross takes the remainder");
    CHECK(m.live_count() == 0, "the rest is gone after the final fill");
}

static void test_ask_side() {
    std::printf("ask_side\n");
    RestingOrderBookMock m;
    (void)m.place(3, MOCK_SIDE_ASK, 460000, 100000000ULL);

    MockRestOutcome o = m.observe_book(440000, 90000000ULL, 470000, 0);
    CHECK(o.event == MockRestEvent::None, "an ask above the bid does not fill");

    o = m.observe_book(460000, 30000000ULL, 470000, 0);
    CHECK(o.event == MockRestEvent::Filled && o.filled_size == 30000000ULL,
          "a buyer lifting our ask fills by their size");
    CHECK(o.fill_price == 460000u, "the ask maker is paid its quote");
}

static void test_cancel() {
    std::printf("cancel\n");
    RestingOrderBookMock m;
    (void)m.place(4, MOCK_SIDE_BID, 440000, 100000000ULL);
    const MockRestOutcome o = m.cancel(4);
    CHECK(o.event == MockRestEvent::Cancelled, "cancel reports itself");
    CHECK(m.live_count() == 0, "the rest is withdrawn");
    CHECK(m.cancels() == 1, "one cancel counted");

    // A cancelled rest must never fill.
    const MockRestOutcome after = m.observe_book(430000, 0, 440000, 100000000ULL);
    CHECK(after.event == MockRestEvent::None, "a cancelled rest cannot fill");

    CHECK(m.cancel(999).event == MockRestEvent::None,
          "cancelling an absent id is a no-op");
}

static void test_supersede_and_reject() {
    std::printf("supersede_and_reject\n");
    RestingOrderBookMock m;
    (void)m.place(5, MOCK_SIDE_BID, 440000, 100000000ULL);
    (void)m.place(6, MOCK_SIDE_BID, 442000, 100000000ULL);
    CHECK(m.live_count() == 1, "only one live rest per side");
    CHECK(m.rest_size(MOCK_SIDE_BID) && *m.rest_size(MOCK_SIDE_BID) == 100000000ULL,
          "the newer rest replaced the older");

    CHECK(m.place(0, MOCK_SIDE_BID, 440000, 1).event == MockRestEvent::Rejected,
          "id 0 is rejected");
    CHECK(m.place(7, MOCK_SIDE_BID, 440000, 0).event == MockRestEvent::Rejected,
          "zero size is rejected");
    CHECK(m.place(8, MOCK_SIDE_BID, 0, 1).event == MockRestEvent::Rejected,
          "zero price is rejected");
    CHECK(m.place(9, 9, 440000, 1).event == MockRestEvent::Rejected,
          "an invalid side is rejected");
    CHECK(m.observe_book(0, 0, 0, 0).event == MockRestEvent::None,
          "an invalid book produces no fill");
}

int main() {
    std::printf("=== test_resting_mock ===\n");
    test_places_and_rests();
    test_fills_only_on_cross();
    test_partial_then_full();
    test_ask_side();
    test_cancel();
    test_supersede_and_reject();
    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
