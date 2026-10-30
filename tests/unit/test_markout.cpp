// ─────────────────────────────────────────────────────────────────────────────
// test_markout: the conditional adverse move, with the sign pinned.
//
// Getting the sign backwards would invert the conclusion of the only measurement
// left to run, so every direction is asserted explicitly.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include "../../core/include/markout_tracker.hpp"
#include "../../core/include/time_utils.hpp"

static int g_failures = 0, g_tests = 0;
#define CHECK(cond, name)                                                     \
    do { ++g_tests; if (cond) { std::printf("  PASS %s\n", name); }            \
    else { std::printf("  FAIL %s (line %d)\n", name, __LINE__); ++g_failures; } } while (0)

using namespace crowdintel;
static constexpr uint64_t NS = 1000000000ULL;

static bool approx(double a, double b, double eps = 1e-9) {
    const double d = a - b;
    return (d < 0 ? -d : d) <= eps;
}

static void test_buy_adverse() {
    std::printf("buy_adverse\n");
    MarkoutTracker t;
    t.configure(60 * NS);
    CHECK(t.record(MARKOUT_SIDE_BUY, 450000, 200000000ULL, 0), "buy recorded");
    // Before the horizon nothing settles.
    CHECK(t.observe(440000, 59 * NS) == 0, "not settled before the horizon");
    CHECK(t.pending() == 1, "sample still pending");
    // Mid fell 1c under our 0.45 buy: we lost -> adverse POSITIVE.
    CHECK(t.observe(440000, 60 * NS) == 1, "settled at the horizon");
    CHECK(approx(t.mean_adverse_cents(), 1.0), "buy with the price falling = +1.0c");
}

static void test_buy_favourable_is_negative() {
    std::printf("buy_favourable_is_negative\n");
    MarkoutTracker t;
    t.configure(60 * NS);
    (void)t.record(MARKOUT_SIDE_BUY, 450000, 200000000ULL, 0);
    // Mid rose to 0.46: a BUY that went our way is NEGATIVE adverse.
    (void)t.observe(460000, 60 * NS);
    CHECK(approx(t.mean_adverse_cents(), -1.0),
          "buy with the price rising = -1.0c (favourable)");
}

static void test_sell_adverse() {
    std::printf("sell_adverse\n");
    MarkoutTracker t;
    t.configure(60 * NS);
    (void)t.record(MARKOUT_SIDE_SELL, 450000, 200000000ULL, 0);
    // Mid rose after we sold: the price moved against us -> POSITIVE.
    (void)t.observe(460000, 60 * NS);
    CHECK(approx(t.mean_adverse_cents(), 1.0), "sell with the price rising = +1.0c");

    MarkoutTracker t2;
    t2.configure(60 * NS);
    (void)t2.record(MARKOUT_SIDE_SELL, 450000, 200000000ULL, 0);
    (void)t2.observe(440000, 60 * NS);
    CHECK(approx(t2.mean_adverse_cents(), -1.0),
          "sell with the price falling = -1.0c (favourable)");
}

static void test_aggregate() {
    std::printf("aggregate\n");
    MarkoutTracker t;
    t.configure(10 * NS);
    // Two buys: one loses 2c, one loses 0c -> mean +1.0c
    (void)t.record(MARKOUT_SIDE_BUY, 500000, 100000000ULL, 0);
    (void)t.record(MARKOUT_SIDE_BUY, 500000, 100000000ULL, 0);
    (void)t.observe(480000, 10 * NS);   // both settle at 0.48
    CHECK(t.settled() == 2, "two samples settled");
    CHECK(approx(t.mean_adverse_cents(), 2.0), "both lost 2c -> mean +2.0c");

    MarkoutTracker m;
    m.configure(10 * NS);
    (void)m.record(MARKOUT_SIDE_BUY, 500000, 100000000ULL, 0);
    (void)m.record(MARKOUT_SIDE_SELL, 500000, 100000000ULL, 0);
    (void)m.observe(490000, 10 * NS);
    // BUY lost 1c, SELL gained 1c -> mean 0
    CHECK(approx(m.mean_adverse_cents(), 0.0), "offsetting fills average to zero");
}

static void test_rejections_and_empty() {
    std::printf("rejections_and_empty\n");
    MarkoutTracker t;
    t.configure(10 * NS);
    CHECK(approx(t.mean_adverse_cents(), 0.0), "empty tracker has no mean");
    CHECK(!t.record(MARKOUT_SIDE_BUY, 0, 1, 0), "zero price refused");
    CHECK(!t.record(MARKOUT_SIDE_BUY, 1000000, 1, 0), "price >= 1 refused");
    CHECK(!t.record(MARKOUT_SIDE_BUY, 500000, 0, 0), "zero size refused");
    CHECK(!t.record(9, 500000, 1, 0), "invalid side refused");
    CHECK(t.observe(0, 100 * NS) == 0, "an invalid mid settles nothing");
    CHECK(t.pending() == 0, "nothing was left pending by the rejections");
}

static void test_clock_domain_contract() {
    std::printf("clock_domain_contract\n");
    // The engine MUST feed record() and observe() from the SAME clock. The
    // venue event stamp is realtime (epoch ns); observe() uses mono_ns(). Mixing
    // them makes `now < ts` always true, so nothing ever settles. This test
    // documents that trap and the fixed contract.
    MarkoutTracker trap;
    trap.configure(60 * NS);
    (void)trap.record(MARKOUT_SIDE_BUY, 450000, 200000000ULL,
                      crowdintel::realtime_ns());
    (void)trap.observe(440000, crowdintel::mono_ns());
    CHECK(trap.settled() == 0, "mixed clocks settle nothing (the trap)");

    MarkoutTracker ok;
    ok.configure(60 * NS);
    const uint64_t t0 = crowdintel::mono_ns();
    (void)ok.record(MARKOUT_SIDE_BUY, 450000, 200000000ULL, t0);
    (void)ok.observe(440000, t0 + 60 * NS);
    CHECK(ok.settled() == 1, "same clock settles at the horizon");
}

static void test_dropped_visible() {
    std::printf("dropped_visible\n");
    MarkoutTracker t;
    t.configure(10 * NS);
    size_t accepted = 0;
    for (size_t i = 0; i < MARKOUT_CAPACITY + 1; ++i) {
        if (t.record(MARKOUT_SIDE_BUY, 500000, 100000000ULL, 0)) ++accepted;
    }
    CHECK(accepted == MARKOUT_CAPACITY, "ring holds exactly capacity");
    CHECK(t.dropped() == 1, "the overflow sample is counted, not silent");
    CHECK(t.pending() == MARKOUT_CAPACITY, "pending reflects the ring fill");
}

static void test_sum_exact() {
    std::printf("sum_exact\n");
    MarkoutTracker t;
    t.configure(10 * NS);
    (void)t.record(MARKOUT_SIDE_BUY, 500000, 100000000ULL, 0);
    (void)t.record(MARKOUT_SIDE_SELL, 500000, 100000000ULL, 0);
    (void)t.observe(490000, 10 * NS);
    // BUY lost 10000 e6, SELL gained 10000 e6 -> exact integer sum 0, no FP.
    CHECK(t.sum_adverse_e6() == 0, "exact integer accumulator");
    CHECK(t.settled() == 2, "both settled");
}

int main() {
    std::printf("=== test_markout ===\n");
    test_buy_adverse();
    test_buy_favourable_is_negative();
    test_sell_adverse();
    test_aggregate();
    test_rejections_and_empty();
    test_clock_domain_contract();
    test_dropped_visible();
    test_sum_exact();
    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
