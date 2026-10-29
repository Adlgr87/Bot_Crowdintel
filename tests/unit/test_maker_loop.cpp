// ─────────────────────────────────────────────────────────────────────────────
// test_maker_loop: the full Motor C cycle
//   place -> keep -> replace (cancel THEN place) -> withdraw when unquotable
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include "../../core/src/maker_loop.hpp"

static int g_failures = 0, g_tests = 0;
#define CHECK(cond, name)                                                     \
    do { ++g_tests; if (cond) { std::printf("  PASS %s\n", name); }            \
    else { std::printf("  FAIL %s (line %d)\n", name, __LINE__); ++g_failures; } } while (0)

using namespace crowdintel;
static constexpr uint64_t NS = 1000000000ULL;

static QuoteConfig qcfg() {
    QuoteConfig c;
    c.adverse_sel_bps = 25;
    c.min_edge_bps = 10;
    c.rewards_max_spread_bps = 350;
    c.rewards_min_size_shares = 200000000ULL;
    c.order_shares = 200000000ULL;
    c.max_inventory_shares = 2000000000ULL;
    return c;
}
static ReplacePolicy rcfg() {
    ReplacePolicy p;
    p.replace_drift_ticks = 1;
    p.max_rest_ns = 30 * NS;
    p.tick_size = 1000;
    p.size_tolerance = 1000000ULL;
    return p;
}
static MakerTickInput book(uint64_t now) {
    MakerTickInput in;
    in.best_bid = 440000; in.bid_size = 100000000ULL;
    in.best_ask = 460000; in.ask_size = 100000000ULL;
    in.fair_value = 450000; in.tick_size = 1000;
    in.inventory_shares = 0; in.now_ns = now;
    return in;
}

static void test_first_tick_places_both() {
    std::printf("first_tick_places_both\n");
    MakerLoop loop(qcfg(), rcfg());
    const MakerDecision d = loop.on_tick(book(NS));
    CHECK(d.quotes.status == QuoteStatus::Placed, "the book is quotable");
    CHECK(d.place_bid && d.place_ask, "both sides are placed on the first tick");
    CHECK(d.cancel_bid_id == 0 && d.cancel_ask_id == 0, "nothing to cancel yet");
    CHECK(d.bid_price > 440000u && d.ask_price < 460000u, "the quote improves the book");
    CHECK(loop.resting() == 0, "nothing rests until the venue confirms");
}

static void test_keep_when_unchanged() {
    std::printf("keep_when_unchanged\n");
    MakerLoop loop(qcfg(), rcfg());
    MakerDecision d = loop.on_tick(book(NS));
    loop.on_open(11, QUOTE_SIDE_BID, d.bid_price, d.bid_size, NS);
    loop.on_open(12, QUOTE_SIDE_ASK, d.ask_price, d.ask_size, NS);
    CHECK(loop.resting() == 2, "both rests confirmed");

    d = loop.on_tick(book(2 * NS));
    CHECK(!d.place_bid && !d.place_ask, "an unchanged book places nothing");
    CHECK(d.cancel_bid_id == 0 && d.cancel_ask_id == 0, "and cancels nothing");
}

static void test_replace_cancels_first() {
    std::printf("replace_cancels_first\n");
    MakerLoop loop(qcfg(), rcfg());
    MakerDecision d = loop.on_tick(book(NS));
    loop.on_open(21, QUOTE_SIDE_BID, d.bid_price, d.bid_size, NS);
    loop.on_open(22, QUOTE_SIDE_ASK, d.ask_price, d.ask_size, NS);

    // The model moves: fair value up 1c, so the target drifts beyond a tick.
    MakerTickInput moved = book(2 * NS);
    moved.fair_value = 460000;
    d = loop.on_tick(moved);
    CHECK(d.cancel_bid_id != 0 || d.cancel_ask_id != 0,
          "a drifted target cancels the stale rest first");
    CHECK(d.place_bid || d.place_ask, "and places the new price");
    CHECK(d.cancel_bid_id == 21 || d.cancel_ask_id == 22,
          "the cancelled id is the resting one");
}

static void test_withdraw_when_unquotable() {
    std::printf("withdraw_when_unquotable\n");
    MakerLoop loop(qcfg(), rcfg());
    MakerDecision d = loop.on_tick(book(NS));
    loop.on_open(31, QUOTE_SIDE_BID, d.bid_price, d.bid_size, NS);
    loop.on_open(32, QUOTE_SIDE_ASK, d.ask_price, d.ask_size, NS);

    // The book tightens below the cost: no quote is justified.
    MakerTickInput tight = book(2 * NS);
    tight.best_bid = 449000;
    tight.best_ask = 451000;
    tight.fair_value = 450000;
    d = loop.on_tick(tight);
    CHECK(d.quotes.status == QuoteStatus::SpreadBelowCost, "the book can no longer pay");
    CHECK(!d.place_bid && !d.place_ask, "nothing new is placed");
    CHECK(d.cancel_bid_id == 31 && d.cancel_ask_id == 32,
          "both rests are withdrawn rather than left exposed");
    CHECK(loop.skips() == 1, "the skip is counted");
}

static void test_inventory_stops_a_side() {
    std::printf("inventory_stops_a_side\n");
    MakerLoop loop(qcfg(), rcfg());
    MakerTickInput full = book(NS);
    full.inventory_shares = 2000000000LL;  // at the cap
    const MakerDecision d = loop.on_tick(full);
    CHECK(!d.place_bid, "a long book places no bid");
    CHECK(d.place_ask, "but keeps offering");
    CHECK(d.bid_size == 0, "with zero bid size");
}

static void test_fill_reduces_then_replaces() {
    std::printf("fill_reduces_then_replaces\n");
    MakerLoop loop(qcfg(), rcfg());
    MakerDecision d = loop.on_tick(book(NS));
    loop.on_open(41, QUOTE_SIDE_BID, d.bid_price, d.bid_size, NS);

    loop.on_fill(41, 50000000ULL);  // 50 of 200 shares
    d = loop.on_tick(book(2 * NS));
    // Size drifted by 50 shares, beyond the 1-share tolerance.
    CHECK(d.place_bid, "a partial fill triggers a re-quote of that side");
    CHECK(d.cancel_bid_id == 41, "cancelling the shrunken rest first");
}

int main() {
    std::printf("=== test_maker_loop (Motor C cycle) ===\n");
    test_first_tick_places_both();
    test_keep_when_unchanged();
    test_replace_cancels_first();
    test_withdraw_when_unquotable();
    test_inventory_stops_a_side();
    test_fill_reduces_then_replaces();
    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
