// ─────────────────────────────────────────────────────────────────────────────
// test_metrics: Fase 3 business instrumentation
//
// Verifies:
//   1. the decision histogram records into the correct slot
//   2. an out-of-range verdict folds into the last slot instead of overflowing
//   3. edge / slippage means are exact fixed-point arithmetic
//   4. Brier accumulates (p_win - label)^2 and reports the mean
//   5. NaN and infinities cannot corrupt the accumulators
//   6. clear() returns the object to a pristine state
//
// There is no allocation, lock or I/O anywhere in the recorder path; the
// hot-path cost contract is "one relaxed fetch_add per event".
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdio>
#include <limits>

#include "../../core/src/metrics.hpp"

static int g_failures = 0;
static int g_tests = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);          \
                ++g_failures; }                                                \
    } while (0)

static bool approx(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

using crowdintel::Metrics;
using crowdintel::METRICS_RESULT_SLOTS;
using crowdintel::MetricsSnapshot;

// ── 1. Histogram ─────────────────────────────────────────────────────────────

static void test_histogram() {
    std::printf("histogram\n");
    Metrics m;

    m.on_decision(0);
    m.on_decision(0);
    m.on_decision(3);
    m.on_decision(7);

    MetricsSnapshot s = m.snapshot();
    CHECK(s.decisions == 4, "four decisions counted");
    CHECK(s.reject_reason[0] == 2, "slot 0 counted twice");
    CHECK(s.reject_reason[3] == 1, "slot 3 counted once");
    CHECK(s.reject_reason[7] == 1, "slot 7 counted once");
    CHECK(s.reject_reason[1] == 0, "untouched slot stays zero");

    // Out of range must not write out of bounds: it folds into the last slot.
    const uint32_t too_big = static_cast<uint32_t>(METRICS_RESULT_SLOTS) + 100;
    m.on_decision(too_big);
    s = m.snapshot();
    CHECK(s.decisions == 5, "out-of-range verdict still counts as a decision");
    CHECK(s.reject_reason[METRICS_RESULT_SLOTS - 1] == 1,
          "out-of-range verdict folds into the last slot");
}

// ── 2. Edge and slippage ─────────────────────────────────────────────────────

static void test_edge_and_slippage() {
    std::printf("edge_and_slippage\n");
    Metrics m;

    CHECK(approx(m.snapshot().edge_mean, 0.0), "empty edge mean is zero");
    CHECK(m.snapshot().edge_samples == 0, "empty edge sample count is zero");

    m.on_order_submitted(0.010);
    m.on_order_submitted(0.020);
    MetricsSnapshot s = m.snapshot();
    CHECK(s.orders_requested == 2, "two orders requested");
    CHECK(s.edge_samples == 2, "two edge samples");
    CHECK(approx(s.edge_mean, 0.015), "edge mean == 0.015");

    m.on_fill(12.5);   // basis points, positive == worse
    m.on_fill(-2.5);   // a price improvement
    s = m.snapshot();
    CHECK(s.fills == 2, "two fills");
    CHECK(s.slip_samples == 2, "two slippage samples");
    CHECK(approx(s.slip_mean_bps, 5.0), "slippage mean == +5 bps");

    // on_order_requested is the zero-edge variant used before an edge exists.
    m.on_order_requested();
    CHECK(m.snapshot().orders_requested == 3, "bare order request counted");
    CHECK(m.snapshot().edge_samples == 2, "bare request adds no edge sample");
}

// ── 3. Brier ─────────────────────────────────────────────────────────────────

static void test_brier() {
    std::printf("brier\n");
    Metrics m;

    CHECK(m.snapshot().brier_samples == 0, "no Brier samples initially");

    // Perfect prediction: (1.0 - 1.0)^2 = 0
    m.on_outcome(1.0, 1.0);
    CHECK(approx(m.snapshot().brier, 0.0), "perfect confident win scores 0");

    // Perfectly wrong confident prediction: (1.0 - 0.0)^2 = 1
    m.on_outcome(1.0, 0.0);
    CHECK(approx(m.snapshot().brier, 0.5), "one exact and one exactly wrong -> 0.5");

    m.clear();
    // 0.60 predicted, lost: (0.60)^2 = 0.36 ; 0.60 predicted, won: (0.40)^2 = 0.16
    m.on_outcome(0.60, 0.0);
    m.on_outcome(0.60, 1.0);
    CHECK(approx(m.snapshot().brier, 0.26),
          "Brier of 0.6/0 and 0.6/1 == 0.26");

    // The threshold the strategy gate cares about.
    m.clear();
    m.on_outcome(0.30, 0.0);   // 0.09
    m.on_outcome(0.30, 0.0);   // 0.09
    m.on_outcome(0.30, 0.0);   // 0.09
    m.on_outcome(0.30, 1.0);   // 0.49
    CHECK(approx(m.snapshot().brier, 0.19),
          "Brier of three misses and one hit at 0.30 == 0.19");
}

// ── 4. Adversarial inputs ────────────────────────────────────────────────────

static void test_adversarial() {
    std::printf("adversarial\n");
    Metrics m;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    // A NaN must not poison the accumulator: the conversion guard stores 0.
    m.on_outcome(nan, 1.0);
    m.on_outcome(1.0, nan);
    m.on_fill(nan);
    m.on_order_submitted(nan);
    MetricsSnapshot s = m.snapshot();
    CHECK(std::isfinite(s.brier), "NaN outcome leaves Brier finite");
    CHECK(std::isfinite(s.slip_mean_bps), "NaN fill leaves slippage finite");
    CHECK(std::isfinite(s.edge_mean), "NaN edge leaves edge mean finite");
    CHECK(s.brier_samples == 2, "NaN outcomes still counted as samples");

    // Infinity likewise.
    Metrics m2;
    m2.on_outcome(inf, -inf);
    m2.on_fill(inf);
    CHECK(std::isfinite(m2.snapshot().brier), "infinite outcome leaves Brier finite");
    CHECK(std::isfinite(m2.snapshot().slip_mean_bps),
          "infinite fill leaves slippage finite");
}

// ── 5. clear() ───────────────────────────────────────────────────────────────

static void test_clear() {
    std::printf("clear\n");
    Metrics m;
    m.on_decision(2);
    m.on_order_submitted(0.05);
    m.on_fill(7.0);
    m.on_outcome(0.9, 0.0);

    m.clear();
    MetricsSnapshot s = m.snapshot();
    CHECK(s.decisions == 0, "decisions cleared");
    CHECK(s.orders_requested == 0, "orders cleared");
    CHECK(s.fills == 0, "fills cleared");
    CHECK(s.edge_samples == 0, "edge samples cleared");
    CHECK(s.slip_samples == 0, "slippage samples cleared");
    CHECK(s.brier_samples == 0, "Brier samples cleared");
    for (size_t i = 0; i < METRICS_RESULT_SLOTS; ++i) {
        if (s.reject_reason[i] != 0) {
            CHECK(false, "histogram slot cleared");
            return;
        }
    }
    CHECK(true, "histogram fully cleared");
}

int main() {
    std::printf("=== test_metrics (Fase 3) ===\n");

    test_histogram();
    test_edge_and_slippage();
    test_brier();
    test_adversarial();
    test_clear();

    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
