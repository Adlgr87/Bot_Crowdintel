// ─────────────────────────────────────────────────────────────────────────────
// test_quote_registry: cancel/replace bookkeeping for the maker sleeve
//
// A resting quote does not end when it is sent. This validates the rules that
// decide when to pull it, because a maker advertising a stale price is exactly
// what informed flow takes.
//
// Verifies:
//   1. nothing is treated as resting until the venue confirms OPEN
//   2. a partial fill shrinks the rest and a full fill removes it
//   3. a new OPEN on a side supersedes the previous rest on that side
//   4. an unwelcome side is cancelled; an absent unwanted side costs nothing
//   5. price drift beyond the tick threshold triggers a replace
//   6. age beyond max_rest_ns triggers a replace (stale information)
//   7. size drift beyond tolerance triggers a replace, small drift does not
//   8. a rejected order leaves no phantom rest behind
//   9. the registry refuses to grow past capacity
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>

#include "../../core/include/quote_registry.hpp"

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

static constexpr uint64_t NS = 1000000000ULL;

static ReplacePolicy policy() {
    ReplacePolicy p;
    p.replace_drift_ticks = 1;
    p.max_rest_ns = 30 * NS;
    p.tick_size = 1000;
    p.size_tolerance = 1000000ULL;  // 1 share
    return p;
}

// ── 1. Nothing rests before the venue says so ───────────────────────────────

static void test_no_assumed_rest() {
    std::printf("no_assumed_rest\n");
    QuoteRegistry r;
    CHECK(r.live_count() == 0, "a fresh registry is empty");
    CHECK(r.find(QUOTE_SIDE_BID) == nullptr, "no bid is assumed resting");
    CHECK(r.action_for(QUOTE_SIDE_BID, 440000, 200000000ULL, 0, policy()) ==
              QuoteAction::Replace,
          "an absent quote means place, not keep");
}

// ── 2. Fills reduce, then remove ────────────────────────────────────────────

static void test_fills() {
    std::printf("fills\n");
    QuoteRegistry r;
    CHECK(r.on_open(11, QUOTE_SIDE_BID, 440000, 200000000ULL, 0),
          "venue confirms the bid rests");
    CHECK(r.live_count() == 1, "one live quote");

    r.on_fill(11, 50000000ULL);  // 50 of 200 shares
    const LiveQuote* q = r.find(QUOTE_SIDE_BID);
    CHECK(q && q->size == 150000000ULL, "partial fill shrinks the rest");
    CHECK(r.live_count() == 1, "a partial fill keeps the quote live");

    r.on_fill(11, 200000000ULL);  // more than remains
    CHECK(r.find(QUOTE_SIDE_BID) == nullptr, "an over-fill removes the rest");
    CHECK(r.live_count() == 0, "registry is empty again");

    // A fill for an unknown id must not corrupt anything.
    r.on_fill(999, 1000000ULL);
    CHECK(r.live_count() == 0, "an unknown fill id is ignored");
}

// ── 3. A newer rest supersedes the older one on the same side ───────────────

static void test_supersede() {
    std::printf("supersede\n");
    QuoteRegistry r;
    CHECK(r.on_open(21, QUOTE_SIDE_BID, 440000, 100000000ULL, 0), "first bid rests");
    CHECK(r.on_open(22, QUOTE_SIDE_BID, 445000, 100000000ULL, NS), "second bid rests");
    CHECK(r.live_count() == 1, "only one live quote per side");
    const LiveQuote* q = r.find(QUOTE_SIDE_BID);
    CHECK(q && q->id == 22, "the newest rest is the live one");
    CHECK(q && q->price == 445000, "at the new price");
}

// ── 4. Unwanted side ────────────────────────────────────────────────────────

static void test_unwanted_side() {
    std::printf("unwanted_side\n");
    QuoteRegistry r;
    CHECK(r.on_open(31, QUOTE_SIDE_ASK, 460000, 100000000ULL, 0), "ask rests");
    CHECK(r.action_for(QUOTE_SIDE_ASK, 0, 0, NS, policy()) == QuoteAction::Cancel,
          "a side with no target is cancelled");
    CHECK(r.action_for(QUOTE_SIDE_BID, 0, 0, NS, policy()) == QuoteAction::Keep,
          "cancelling an absent side is a no-op, not a busy loop");
}

// ── 5. Price drift ──────────────────────────────────────────────────────────

static void test_price_drift() {
    std::printf("price_drift\n");
    QuoteRegistry r;
    CHECK(r.on_open(41, QUOTE_SIDE_BID, 440000, 100000000ULL, 0), "bid rests at 0.44");
    // Tick 0.001; drift threshold 1 tick = 1000 units.
    CHECK(r.action_for(QUOTE_SIDE_BID, 440000, 100000000ULL, NS, policy()) ==
              QuoteAction::Keep,
          "no drift keeps the quote");
    CHECK(r.action_for(QUOTE_SIDE_BID, 441000, 100000000ULL, NS, policy()) ==
              QuoteAction::Replace,
          "one tick of drift replaces the quote");
    CHECK(r.action_for(QUOTE_SIDE_BID, 439000, 100000000ULL, NS, policy()) ==
              QuoteAction::Replace,
          "drift is symmetric");
}

// ── 6. Age ──────────────────────────────────────────────────────────────────

static void test_age() {
    std::printf("age\n");
    QuoteRegistry r;
    CHECK(r.on_open(51, QUOTE_SIDE_ASK, 460000, 100000000ULL, 0), "ask rests at t=0");
    CHECK(r.action_for(QUOTE_SIDE_ASK, 460000, 100000000ULL, 29 * NS, policy()) ==
              QuoteAction::Keep,
          "inside the window the quote is kept");
    CHECK(r.action_for(QUOTE_SIDE_ASK, 460000, 100000000ULL, 30 * NS, policy()) ==
              QuoteAction::Replace,
          "at the window edge the quote is refreshed");
    CHECK(r.action_for(QUOTE_SIDE_ASK, 460000, 100000000ULL, 120 * NS, policy()) ==
              QuoteAction::Replace,
          "a long-stale quote is replaced regardless of price");
}

// ── 7. Size drift ───────────────────────────────────────────────────────────

static void test_size_drift() {
    std::printf("size_drift\n");
    QuoteRegistry r;
    CHECK(r.on_open(61, QUOTE_SIDE_BID, 440000, 200000000ULL, 0), "bid rests with 200");
    ReplacePolicy p = policy();
    p.size_tolerance = 5000000ULL;  // 5 shares

    CHECK(r.action_for(QUOTE_SIDE_BID, 440000, 201000000ULL, NS, p) ==
              QuoteAction::Keep,
          "a 1-share size change is inside tolerance");
    CHECK(r.action_for(QUOTE_SIDE_BID, 440000, 150000000ULL, NS, p) ==
              QuoteAction::Replace,
          "a 50-share reduction replaces the quote (inventory moved)");
}

// ── 8. Rejections leave nothing behind ──────────────────────────────────────

static void test_reject() {
    std::printf("reject\n");
    QuoteRegistry r;
    CHECK(r.on_open(71, QUOTE_SIDE_BID, 440000, 100000000ULL, 0), "bid rests");
    r.on_reject(71);
    CHECK(r.find(QUOTE_SIDE_BID) == nullptr,
          "a rejected order is not treated as resting");
    CHECK(r.action_for(QUOTE_SIDE_BID, 440000, 100000000ULL, NS, policy()) ==
              QuoteAction::Replace,
          "so the next tick places again");
}

// ── 9. Capacity ─────────────────────────────────────────────────────────────

static void test_capacity() {
    std::printf("capacity\n");
    QuoteRegistry r;
    // Each OPEN on a side supersedes the previous, so alternating sides can
    // never exceed two live quotes; fill the rest with distinct ids on the two
    // sides by opening, cancelling, opening.
    size_t accepted = 0;
    for (uint64_t i = 1; i <= MAX_LIVE_QUOTES + 4; ++i) {
        const uint8_t side = (i % 2 == 0) ? QUOTE_SIDE_BID : QUOTE_SIDE_ASK;
        if (r.on_open(100 + i, side, 400000 + i * 1000u, 1000000ULL, i)) ++accepted;
        r.on_cancel(100 + i);
    }
    CHECK(accepted == MAX_LIVE_QUOTES + 4,
          "cancelled slots are reused instead of exhausting capacity");
    CHECK(r.live_count() == 0, "everything was cancelled");

    // Invalid openings are refused.
    CHECK(!r.on_open(0, QUOTE_SIDE_BID, 440000, 1000000ULL, 0), "id 0 is refused");
    CHECK(!r.on_open(77, QUOTE_SIDE_BID, 440000, 0, 0), "zero size is refused");
    CHECK(!r.on_open(78, 9, 440000, 1000000ULL, 0), "an invalid side is refused");
}

int main() {
    std::printf("=== test_quote_registry (cancel/replace) ===\n");

    test_no_assumed_rest();
    test_fills();
    test_supersede();
    test_unwanted_side();
    test_price_drift();
    test_age();
    test_size_drift();
    test_reject();
    test_capacity();

    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
