// ─────────────────────────────────────────────────────────────────────────────
// test_reconcile: the position-diff decision that can latch the kill switch.
//
// This logic had zero coverage: it lived inside #if defined(CROWDINTEL_LIVE) and
// its original test source was lost. It decides whether the bot adopts the
// venue's inventory and whether the disagreement stops trading, so an untested
// boundary here is a capital risk, not a coverage gap.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include "../../core/include/reconcile.hpp"

static int g_failures = 0, g_tests = 0;
#define CHECK(cond, name)                                                     \
    do { ++g_tests; if (cond) { std::printf("  PASS %s\n", name); }            \
    else { std::printf("  FAIL %s (line %d)\n", name, __LINE__); ++g_failures; } } while (0)

using namespace crowdintel;

static ReconcileInput base() {
    ReconcileInput in;
    in.local_yes_shares = 100000000ULL;   // 100 shares
    in.venue_yes_shares = 100000000ULL;
    in.max_drift_shares = 0.01;           // 0.01 shares
    return in;
}

static void test_no_drift_is_silent() {
    std::printf("no_drift_is_silent\n");
    const ReconcileOutcome o = reconcile_positions(base());
    CHECK(o.drift == 0, "identical inventory means zero drift");
    CHECK(!o.restate, "and no restate: an idle market must not spam the queue");
    CHECK(!o.drift_exceeded, "and no kill");
}

static void test_small_drift_restates_without_killing() {
    std::printf("small_drift_restates_without_killing\n");
    ReconcileInput in = base();
    in.venue_yes_shares = 100000001ULL;  // 0.000001 shares off
    const ReconcileOutcome o = reconcile_positions(in);
    CHECK(o.drift == 1, "drift is measured exactly");
    CHECK(o.restate, "a non-zero drift is adopted");
    CHECK(!o.drift_exceeded, "below the threshold it is not a kill");
}

static void test_boundary_is_strictly_greater() {
    std::printf("boundary_is_strictly_greater\n");
    ReconcileInput in = base();
    in.max_drift_shares = 1.0;           // 1e6 units
    in.venue_yes_shares = 100000000ULL + 1000000ULL;  // exactly at max
    ReconcileOutcome o = reconcile_positions(in);
    CHECK(o.drift == 1000000ULL, "drift equals the maximum exactly");
    CHECK(!o.drift_exceeded,
          "drift == max is TOLERATED: the rule is strictly greater");

    in.venue_yes_shares = 100000000ULL + 1000001ULL;
    o = reconcile_positions(in);
    CHECK(o.drift_exceeded, "one unit past the maximum kills");
}

static void test_zero_maximum_disables_the_kill() {
    std::printf("zero_maximum_disables_the_kill\n");
    ReconcileInput in = base();
    in.max_drift_shares = 0.0;
    in.venue_yes_shares = 0;             // venue says nothing held at all
    const ReconcileOutcome o = reconcile_positions(in);
    CHECK(o.drift == 100000000ULL, "the disagreement is still measured");
    CHECK(o.restate, "and still adopted");
    CHECK(!o.drift_exceeded,
          "a zero maximum disables the kill explicitly, not accidentally");
}

static void test_hedge_leg_counts() {
    std::printf("hedge_leg_counts\n");
    ReconcileInput in = base();
    in.local_hedge_shares = 50000000ULL;
    in.venue_hedge_shares = 40000000ULL;
    const ReconcileOutcome o = reconcile_positions(in);
    CHECK(o.drift == 10000000ULL, "the hedge leg contributes to the total");
}

static void test_local_only_inventory_is_the_dangerous_case() {
    std::printf("local_only_inventory_is_the_dangerous_case\n");
    // We believe we hold 100 shares; the venue says we hold none. Adopting is
    // right, and the disagreement is far past the threshold — this is the shape
    // of a restart that lost its inventory view.
    ReconcileInput in = base();
    in.venue_yes_shares = 0;
    const ReconcileOutcome o = reconcile_positions(in);
    CHECK(o.restate, "the venue's truth is adopted");
    CHECK(o.drift_exceeded, "and a 100-share disagreement latches the kill");
    CHECK(o.drift == 100000000ULL, "with the full magnitude reported");
}

int main() {
    std::printf("=== test_reconcile ===\n");
    test_no_drift_is_silent();
    test_small_drift_restates_without_killing();
    test_boundary_is_strictly_greater();
    test_zero_maximum_disables_the_kill();
    test_hedge_leg_counts();
    test_local_only_inventory_is_the_dangerous_case();
    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
