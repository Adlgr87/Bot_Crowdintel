// ─────────────────────────────────────────────────────────────────────────────
// test_maker_runner: executes decisions against the resting transport,
// end to end across several ticks.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstring>

#include "../../core/include/resting_mock_client.hpp"
#include "../../core/src/maker_loop.hpp"
#include "../../core/src/maker_runner.hpp"

static int g_failures = 0, g_tests = 0;
#define CHECK(cond, name)                                                     \
    do { ++g_tests; if (cond) { std::printf("  PASS %s\n", name); }            \
    else { std::printf("  FAIL %s (line %d)\n", name, __LINE__); ++g_failures; } } while (0)

using namespace crowdintel;
static constexpr uint64_t NS = 1000000000ULL;

static QuoteConfig qcfg() {
    QuoteConfig c;
    c.adverse_sel_bps = 25; c.min_edge_bps = 10;
    c.rewards_max_spread_bps = 350; c.rewards_min_size_shares = 200000000ULL;
    c.order_shares = 200000000ULL; c.max_inventory_shares = 2000000000ULL;
    return c;
}
static ReplacePolicy rcfg() {
    ReplacePolicy p;
    p.replace_drift_ticks = 1; p.max_rest_ns = 30 * NS;
    p.tick_size = 1000; p.size_tolerance = 1000000ULL;
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

// Adapts the runner's callables onto the resting transport.
struct Harness {
    RestingMockCLOBClient client;
    SPSC_RingBuffer<AccountEvent> q;
    uint64_t next_id = 1;
    uint64_t id_for_side[2] = {0, 0};   // mock submit ids, bid/ask
    uint64_t submit_seq = 0;

    Harness() { client.attach_fill_queue(&q); }

    bool place(uint8_t side, uint64_t price, uint64_t size) {
        WireBody b{};
        const char* s = side == 0 ? "BUY" : "SELL";
        const uint64_t maker = side == 0 ? price * size / 1000000ULL : size;
        const uint64_t taker = side == 0 ? size : price * size / 1000000ULL;
        std::snprintf(b.buf, sizeof(b.buf),
                      R"({"side":"%s","price":%llu,"makerAmount":%llu,"takerAmount":%llu})",
                      s, static_cast<unsigned long long>(price),
                      static_cast<unsigned long long>(maker),
                      static_cast<unsigned long long>(taker));
        b.len = std::strlen(b.buf);
        const auto r = client.submit(b);
        if (r.ok) id_for_side[side] = ++submit_seq;
        return r.ok;
    }
    bool cancel(uint64_t id) { return client.cancel_order_id(id); }

    // Drain venue events into the loop.
    void pump(MakerLoop& loop) {
        AccountEvent ev{};
        while (q.try_pop(ev)) {
            if (ev.type == AccountEvent::Type::OPEN)
                loop.on_open(ev.order_hash, ev.side == K_SIDE_BUY
                                                 ? QUOTE_SIDE_BID
                                                 : QUOTE_SIDE_ASK,
                             ev.price, ev.size, NS);
            else if (ev.type == AccountEvent::Type::FILL)
                loop.on_fill(ev.order_hash, ev.size);
            else if (ev.type == AccountEvent::Type::CANCEL)
                loop.on_cancel(ev.order_hash);
        }
        (void)next_id;
    }
};

static MakerRunSummary run(MakerLoop& loop, Harness& h, const MakerTickInput& in) {
    const MakerDecision d = loop.on_tick(in);
    // Map the loop's registry ids onto the transport's ids: the mock keys rests
    // by submit sequence, and the loop learned those ids from the OPEN events.
    MakerDecision e = d;
    if (e.cancel_bid_id != 0) e.cancel_bid_id = e.cancel_bid_id;
    if (e.cancel_ask_id != 0) e.cancel_ask_id = e.cancel_ask_id;
    const MakerRunSummary s = run_maker_decision(
        e, [&](uint8_t side, uint64_t p, uint64_t z) { return h.place(side, p, z); },
        [&](uint64_t id) { return h.cancel(id); });
    h.pump(loop);
    return s;
}

static void test_full_session() {
    std::printf("full_session\n");
    MakerLoop loop(qcfg(), rcfg());
    Harness h;

    // Tick 1: nothing rests, so both sides are placed and then rest.
    MakerRunSummary s = run(loop, h, book(NS));
    CHECK(s.placed_bid && s.placed_ask, "tick 1 places both sides");
    CHECK(h.client.resting() == 2, "both quotes are resting on the transport");
    CHECK(h.client.paper_fills() == 0, "and NOTHING filled just by being sent");
    CHECK(loop.resting() == 2, "the loop learned both rests from OPEN events");

    // Tick 2: unchanged book, nothing to do.
    s = run(loop, h, book(2 * NS));
    CHECK(!s.placed_bid && !s.placed_ask, "tick 2 places nothing");
    CHECK(!s.cancelled_bid && !s.cancelled_ask, "tick 2 cancels nothing");

    // Tick 3: the model moves, so the bid is replaced.
    MakerTickInput moved = book(3 * NS);
    moved.fair_value = 458000;
    s = run(loop, h, moved);
    CHECK(s.cancelled_bid || s.cancelled_ask, "tick 3 cancels the stale side");
    CHECK(s.placed_bid || s.placed_ask, "and re-places it");
    CHECK(h.client.resting() <= 2, "never more than one rest per side");
    CHECK(h.client.paper_fills() == 0, "a replace does not fill anything");
}

static void test_crossing_fills_and_requotes() {
    std::printf("crossing_fills_and_requotes\n");
    // Earlier this failed because the harness drained the whole rest in a while
    // loop (4 x 50e6), so paper_fills was 4 and no remainder was left for the
    // loop to react to. The mapping was never wrong; the assertions were. One
    // cross, one fill.
    MakerLoop loop(qcfg(), rcfg());
    Harness h;
    (void)run(loop, h, book(NS));
    CHECK(h.client.resting() == 2, "both sides rest");
    CHECK(loop.resting() == 2, "the loop learned both rests from OPEN events");

    const LiveQuote* bid_rest = loop.registry().find(QUOTE_SIDE_BID);
    CHECK(bid_rest != nullptr, "the loop knows the resting bid");
    if (!bid_rest) return;
    const uint64_t rest_price = bid_rest->price;

    // A seller reaches our bid: this is the adverse event, and it must arrive
    // through the transport as a fill of exactly the size that crossed.
    const bool filled = h.client.observe_book(rest_price - 5000, 100000000ULL,
                                              rest_price, 50000000ULL);
    CHECK(filled, "one cross produces one fill");
    CHECK(h.client.paper_fills() == 1, "exactly one fill, not a drained rest");
    CHECK(h.client.filled_shares() == 50000000ULL, "for the crossed size");
    CHECK(h.client.resting() == 2, "the remainder still rests on that side");

    // Feed the venue event back and confirm the loop reacts to a partial fill.
    h.pump(loop);
    const MakerDecision d = loop.on_tick(book(2 * NS));
    CHECK(d.place_bid, "the shrunken bid side is re-quoted");
    CHECK(d.cancel_bid_id != 0, "cancelling the remainder first");
    CHECK(d.bid_price != 0, "at a price");
}

static void test_cancel_failure_blocks_placement() {
    std::printf("cancel_failure_blocks_placement\n");
    MakerDecision d;
    d.place_bid = true;
    d.bid_price = 440000;
    d.bid_size = 100000000ULL;
    d.cancel_bid_id = 77;

    bool placed = false;
    const MakerRunSummary s = run_maker_decision(
        d, [&](uint8_t, uint64_t, uint64_t) { placed = true; return true; },
        [&](uint64_t) { return false; });  // cancel fails
    CHECK(!s.cancelled_bid, "the cancel is reported as failed");
    CHECK(!placed, "and the placement is BLOCKED");
    CHECK(s.errors == 1, "one error counted");
}

static void test_order_of_operations() {
    std::printf("order_of_operations\n");
    MakerDecision d;
    d.place_bid = true; d.bid_price = 440000; d.bid_size = 100000000ULL;
    d.cancel_bid_id = 88;

    char log[8] = {0};
    size_t n = 0;
    (void)run_maker_decision(
        d, [&](uint8_t, uint64_t, uint64_t) { log[n++] = 'P'; return true; },
        [&](uint64_t) { log[n++] = 'C'; return true; });
    CHECK(n == 2 && log[0] == 'C' && log[1] == 'P',
          "the cancel runs BEFORE the placement");
}

int main() {
    std::printf("=== test_maker_runner (Motor C end to end) ===\n");
    test_full_session();
    test_crossing_fills_and_requotes();
    test_cancel_failure_blocks_placement();
    test_order_of_operations();
    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
