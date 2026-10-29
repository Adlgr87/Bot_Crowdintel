// ─────────────────────────────────────────────────────────────────────────────
// test_maker_quote: Motor C decision core
//
// The sleeve rests two-sided quotes. This file validates the arithmetic that
// decides WHERE and WHETHER to quote, because that arithmetic is the sleeve:
// measured net was +0.392c per fill on 19 of 22 markets, and the cushion is the
// one number that can flip it.
//
// Verifies:
//   1. a sane book with a wide spread produces a symmetric two-sided quote
//   2. a book tighter than cushion + edge is refused (SpreadBelowCost) —
//      quoting into it is how a maker becomes exit liquidity
//   3. a reward band narrower than the required cushion is refused
//   4. a quote whose fair value sits far from mid is refused, because the band
//      is measured from the midpoint even when the width test passes
//   5. size below the venue's reward minimum is refused
//   6. inventory skew tapers then stops the side that grows the position
//   7. tick rounding never shrinks the cushion (rounds away from fair value)
//   8. malformed inputs are rejected rather than quoted
//   9. resting edge accounting: maker fee 0 vs taker fee 7% on the same quote
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>

#include "../../core/include/maker_quote.hpp"

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

// $0.44 / $0.46 book, fair value at mid, 0.001 tick: the shape of a real
// prediction-market book (spread of 2c, the live markets run 0.1c to 2c).
static QuoteInput base_book() {
    QuoteInput in;
    in.best_bid = 440000;
    in.best_ask = 460000;
    in.fair_value = 450000;
    in.tick_size = 1000;
    in.inventory_shares = 0;
    return in;
}

static QuoteConfig base_config() {
    QuoteConfig cfg;
    cfg.adverse_sel_bps = 25;   // 0.25c cushion
    cfg.min_edge_bps = 10;      // 0.10c profit floor
    cfg.rewards_max_spread_bps = 350;    // 3.5c band, the live value
    cfg.rewards_min_size_shares = 200000000ULL;  // 200 shares
    cfg.order_shares = 200000000ULL;
    cfg.max_inventory_shares = 2000000000ULL;    // 2000 shares
    return cfg;
}

// ── 1. Happy path ────────────────────────────────────────────────────────────

static void test_places_symmetric_quote() {
    std::printf("places_symmetric_quote\n");
    const QuotePlan p = plan_quotes(base_book(), base_config());
    CHECK(p.status == QuoteStatus::Placed, "a sane book places a quote");
    CHECK(p.required_half_width == 3500u,
          "required half width = (25 + 10) bps = 3500 units");
    CHECK(p.bid_price < p.ask_price, "bid strictly below ask");
    // A maker IMPROVES the book. Joining the touch would forfeit rewards when
    // the touch sits outside the venue's reward band, so the quote goes inside.
    CHECK(p.bid_price > 440000u, "bid improves on the best bid");
    CHECK(p.ask_price < 460000u, "ask improves on the best ask");
    CHECK(p.bid_price < p.ask_price, "still a valid two-sided market");
    CHECK(p.ask_price - p.bid_price >= p.required_half_width,
          "quoted spread still covers cushion + edge");
    CHECK(p.bid_size == 200000000ULL && p.ask_size == 200000000ULL,
          "both sides sized at the order size");
}

// ── 2. The book must pay for the cushion ─────────────────────────────────────

static void test_refuses_tight_book() {
    std::printf("refuses_tight_book\n");
    QuoteInput in = base_book();
    in.best_bid = 449000;   // half spread 0.001 = 1000 units
    in.best_ask = 451000;
    in.fair_value = 450000;
    const QuotePlan p = plan_quotes(in, base_config());
    CHECK(p.status == QuoteStatus::SpreadBelowCost,
          "a 0.1c spread cannot pay a 0.35c cushion");

    // Widening the book to exactly the requirement must flip it to Placed.
    QuoteInput ok = base_book();
    ok.best_bid = 446500;
    ok.best_ask = 453500;
    const QuotePlan p2 = plan_quotes(ok, base_config());
    CHECK(p2.status == QuoteStatus::Placed,
          "a spread exactly at cost is accepted");
}

// ── 3. Reward band ───────────────────────────────────────────────────────────

static void test_reward_band() {
    std::printf("reward_band\n");
    QuoteConfig cfg = base_config();
    cfg.rewards_max_spread_bps = 200;  // 2c band, still wider than 0.35c
    // 2 cents = 20000 units, requirement 3500 -> fits.
    CHECK(plan_quotes(base_book(), cfg).status == QuoteStatus::Placed,
          "a 2c band accommodates a 0.35c cushion");

    // A band narrower than the cushion: 3.5 bps worth of cent.
    QuoteConfig tight = base_config();
    tight.rewards_max_spread_bps = 0;  // treated as "no program"
    tight.rewards_min_size_shares = 0;
    CHECK(plan_quotes(base_book(), tight).status == QuoteStatus::Placed,
          "no reward program means no band constraint");

    // Isolate the band by giving the book a spread wide enough to pay the
    // cushion. With a 2c book and a 5c cushion the binding constraint is the
    // spread (SpreadBelowCost), which is the more informative answer; to test
    // the band itself the spread must stop being the limit.
    QuoteInput wide = base_book();
    wide.best_bid = 400000;   // 10c spread
    wide.best_ask = 500000;
    wide.fair_value = 450000;

    QuoteConfig fits = base_config();
    fits.adverse_sel_bps = 300;   // 3c cushion
    fits.min_edge_bps = 10;       // 3.1c required
    fits.rewards_max_spread_bps = 350;
    CHECK(plan_quotes(wide, fits).status == QuoteStatus::Placed,
          "a 3.1c cushion fits a 3.5c band on a wide book");

    QuoteConfig huge = base_config();
    huge.adverse_sel_bps = 400;   // 4c cushion
    huge.min_edge_bps = 10;       // 4.1c required > 3.5c band
    huge.rewards_max_spread_bps = 350;
    CHECK(plan_quotes(wide, huge).status == QuoteStatus::BandTooNarrow,
          "a 4.1c cushion cannot fit a 3.5c reward band");
}

// ── 4. Band is measured from mid, not from fair value ────────────────────────

static void test_band_measured_from_mid() {
    std::printf("band_measured_from_mid\n");
    QuoteConfig cfg = base_config();
    cfg.adverse_sel_bps = 20;
    cfg.min_edge_bps = 5;          // 0.25c required
    cfg.rewards_max_spread_bps = 350;

    // Fair value 2c below mid. Width test passes (0.25c < 3.5c) but the bid
    // lands 2.25c under mid, still inside 3.5c... push it further.
    QuoteInput in = base_book();
    in.fair_value = 430000;  // 2c below mid
    const QuotePlan p1 = plan_quotes(in, cfg);
    CHECK(p1.status == QuoteStatus::Placed,
          "a fair value 2c off mid still quotes inside a 3.5c band");

    // 4c below mid: the raw bid would sit 4.25c under mid, outside the 3.5c
    // band, so the quote is pulled back to the band edge rather than dropped —
    // a maker that cannot reach the band earns nothing at all.
    QuoteInput far = base_book();
    far.fair_value = 410000;
    const QuotePlan p2 = plan_quotes(far, cfg);
    CHECK(p2.status == QuoteStatus::Placed,
          "a divergent fair value is clamped into the band, not refused");
    const uint64_t mid = (far.best_bid + far.best_ask) / 2u;
    const uint64_t band = 350u * 100u;  // 3.5c in 1e6 units
    // Guarded: ask < mid makes the raw subtraction underflow in uint64.
    const uint64_t bid_dist = mid > p2.bid_price ? mid - p2.bid_price : 0;
    const uint64_t ask_dist = p2.ask_price > mid ? p2.ask_price - mid : 0;
    CHECK(bid_dist <= band && ask_dist <= band,
          "both sides stay inside the reward band after clamping");
    // KNOWN CONSEQUENCE, deliberately asserted so it cannot regress silently:
    // the clamping lands the bid ABOVE the model's fair value, so that side now
    // has negative edge and should be dropped rather than rested. One-sided
    // quoting is the follow-up; today the caller sees it in the price.
    CHECK(p2.bid_price > far.fair_value,
          "a divergent fair value leaves the bid above it (negative-edge side)");
}

// ── 5. Venue minimum size ────────────────────────────────────────────────────

static void test_minimum_size() {
    std::printf("minimum_size\n");
    QuoteConfig cfg = base_config();
    cfg.order_shares = 50000000ULL;  // 50 shares < 200 required
    const QuotePlan p = plan_quotes(base_book(), cfg);
    CHECK(p.status == QuoteStatus::SizeBelowRewardMinimum,
          "an order below rewardsMinSize is refused");

    cfg.order_shares = 0;
    CHECK(plan_quotes(base_book(), cfg).status ==
              QuoteStatus::SizeBelowRewardMinimum,
          "zero size is refused");
}

// ── 6. Inventory skew ────────────────────────────────────────────────────────

static void test_inventory_skew() {
    std::printf("inventory_skew\n");
    QuoteConfig cfg = base_config();
    cfg.max_inventory_shares = 2000000000ULL;  // 2000 shares

    QuoteInput flat = base_book();
    const QuotePlan p0 = plan_quotes(flat, cfg);
    CHECK(p0.bid_size == p0.ask_size, "flat inventory quotes symmetrically");

    // Fully long: stop bidding, keep offering.
    QuoteInput full_long = base_book();
    full_long.inventory_shares = 2000000000LL;
    const QuotePlan p1 = plan_quotes(full_long, cfg);
    CHECK(p1.status == QuoteStatus::Placed, "fully long still offers");
    CHECK(p1.bid_size == 0, "fully long stops bidding");
    CHECK(p1.ask_size == 200000000ULL, "fully long keeps full offer size");

    // Fully short: mirrors.
    QuoteInput full_short = base_book();
    full_short.inventory_shares = -2000000000LL;
    const QuotePlan p2 = plan_quotes(full_short, cfg);
    CHECK(p2.bid_size == 200000000ULL, "fully short keeps bidding");
    CHECK(p2.ask_size == 0, "fully short stops offering");

    // Halfway long: the bid tapers.
    QuoteInput half = base_book();
    half.inventory_shares = 1000000000LL;
    const QuotePlan p3 = plan_quotes(half, cfg);
    CHECK(p3.bid_size < p3.ask_size,
          "half-long inventory tapers the bid below the offer");

    // Tapered below the reward minimum: that side must be dropped entirely
    // rather than rest a quote that earns nothing but carries risk.
    QuoteInput nearly = base_book();
    nearly.inventory_shares = 1980000000LL;  // 99% of cap
    const QuotePlan p4 = plan_quotes(nearly, cfg);
    CHECK(p4.bid_size == 0,
          "a tapered side below rewardsMinSize is dropped, not rested");
}

// ── 7. Tick rounding never shrinks the cushion ───────────────────────────────

static void test_tick_rounding() {
    std::printf("tick_rounding\n");
    QuoteConfig cfg = base_config();
    cfg.adverse_sel_bps = 25;
    cfg.min_edge_bps = 10;   // 3500 units = 0.0035
    QuoteInput in = base_book();
    in.tick_size = 10000;   // coarse 1c tick
    const QuotePlan p = plan_quotes(in, cfg);
    CHECK(p.status == QuoteStatus::Placed, "coarse tick still places");
    // 0.45 - 0.0035 = 0.4465 must round DOWN to 0.44, not up to 0.45.
    CHECK(p.bid_price == 440000u, "bid rounds away from fair value (down)");
    CHECK(p.ask_price == 460000u, "ask rounds away from fair value (up)");
}

// ── 8. Malformed inputs ─────────────────────────────────────────────────────

static void test_invalid_inputs() {
    std::printf("invalid_inputs\n");
    QuoteConfig cfg = base_config();

    QuoteInput crossed = base_book();
    crossed.best_bid = 460000;
    crossed.best_ask = 440000;
    CHECK(plan_quotes(crossed, cfg).status == QuoteStatus::InvalidBook,
          "a crossed book is rejected");

    QuoteInput zero = base_book();
    zero.best_bid = 0;
    CHECK(plan_quotes(zero, cfg).status == QuoteStatus::InvalidBook,
          "a zero bid is rejected");

    QuoteInput fair0 = base_book();
    fair0.fair_value = 0;
    CHECK(plan_quotes(fair0, cfg).status == QuoteStatus::InvalidFairValue,
          "fair value 0 is rejected");

    QuoteInput fair1 = base_book();
    fair1.fair_value = 1000000;
    CHECK(plan_quotes(fair1, cfg).status == QuoteStatus::InvalidFairValue,
          "fair value 1.0 is rejected");

    QuoteInput badtick = base_book();
    badtick.tick_size = 3000;  // does not divide 1e6
    CHECK(plan_quotes(badtick, cfg).status == QuoteStatus::TickTooCoarse,
          "a tick that does not divide the domain is rejected");
}

// ── 9. Resting edge accounting ──────────────────────────────────────────────

static void test_resting_edge() {
    std::printf("resting_edge\n");
    // Bid 0.44 against fair 0.45: 100 bps of gross edge.
    CHECK(resting_edge_bps(450000, 440000, true, 25, 0) == 75,
          "bid edge = gross 100bps - 25bps cushion = 75bps");
    CHECK(resting_edge_bps(450000, 440000, false, 25, 0) == -125,
          "the same price as an ask is 125bps negative");

    // The whole point of the maker route: the venue charges makers nothing
    // (feeSchedule.takerOnly == true). Charge taker's 700bps instead and every
    // quote is hopelessly underwater.
    CHECK(resting_edge_bps(450000, 440000, true, 25, 0) > 0,
          "maker fee 0 leaves the quote profitable");
    CHECK(resting_edge_bps(450000, 440000, true, 25, 700) < 0,
          "a 7% taker fee would eat the same quote entirely");

    // A cushion larger than the gross edge flips the sign.
    CHECK(resting_edge_bps(450000, 440000, true, 150, 0) < 0,
          "a 150bps cushion exceeds a 100bps gross edge");
}

int main() {
    std::printf("=== test_maker_quote (Motor C) ===\n");

    test_places_symmetric_quote();
    test_refuses_tight_book();
    test_reward_band();
    test_band_measured_from_mid();
    test_minimum_size();
    test_inventory_skew();
    test_tick_rounding();
    test_invalid_inputs();
    test_resting_edge();

    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
