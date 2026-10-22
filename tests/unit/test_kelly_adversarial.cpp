// ─────────────────────────────────────────────────────────────────────────────
// test_kelly_adversarial: NaN/Inf/fuzz tests for KellySizer compute()
//
// Per ADR-0004: "Edge cases — overflow, underflow, NaN, inf, denormal"
// Tests that CRIT-FIX (std::isfinite guards) prevents NaN propagation:
//   1. NaN market_price → should_trade=false (not NaN trade)
//   2. NaN p_model → should_trade=false
//   3. NaN bankroll → should_trade=false
//   4. Inf inputs → should_trade=false
//   5. Mixed NaN on any parameter → should_trade=false
//   6. Boundary: market_price=0.0, p_model=0.0, 1.0
//   7. kelly_fractioned equals capped_fraction
//   8. fee_cost is finite for valid inputs
//
// ADR-0004 | anti/adversarial-nan-tests
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "../../core/src/kelly_sizer.hpp"

static int g_failures = 0;
static int g_tests = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);          \
                ++g_failures; }                                                \
    } while (0)

static constexpr double QNaN = std::numeric_limits<double>::quiet_NaN();
static constexpr double Inf = std::numeric_limits<double>::infinity();
static constexpr double NegInf = -std::numeric_limits<double>::infinity();

// ── Test 1: NaN on every input parameter ──────────────────────────────────────

static void test_nan_all_params() {
    std::printf("nan_all_params\n");
    Config cfg;
    KellySizer sizer(cfg);

    // NaN market_price
    auto r1 = sizer.compute(QNaN, 0.50, 1000.0, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(!r1.should_trade, "NaN market_price → !should_trade");

    // NaN p_model
    auto r2 = sizer.compute(0.60, QNaN, 1000.0, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(!r2.should_trade, "NaN p_model → !should_trade");

    // NaN bankroll_usdc
    auto r3 = sizer.compute(0.60, 0.50, QNaN, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(!r3.should_trade, "NaN bankroll → !should_trade");

    // NaN inventory
    auto r4 = sizer.compute(0.60, 0.50, 1000.0, QNaN, 0.0, false, 1.0, 0.0);
    CHECK(!r4.should_trade, "NaN inventory → !should_trade");

    // NaN vol_z_score
    auto r5 = sizer.compute(0.60, 0.50, 1000.0, 0.0, QNaN, false, 1.0, 0.0);
    CHECK(!r5.should_trade, "NaN vol_z → !should_trade");

    // NaN ofi_size_multiplier
    auto r6 = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, false, QNaN, 0.0);
    CHECK(!r6.should_trade, "NaN ofi_mult → !should_trade");

    // NaN calibration_ece
    auto r7 = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, false, 1.0, QNaN);
    CHECK(!r7.should_trade, "NaN ece → !should_trade");
}

// ── Test 2: Inf on all inputs ─────────────────────────────────────────────────

static void test_inf_all_params() {
    std::printf("inf_all_params\n");
    Config cfg;
    KellySizer sizer(cfg);

    auto r1 = sizer.compute(Inf, 0.50, 1000.0, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(!r1.should_trade, "Inf market_price → !should_trade");

    auto r2 = sizer.compute(0.60, Inf, 1000.0, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(!r2.should_trade, "Inf p_model → !should_trade");

    auto r3 = sizer.compute(0.60, 0.50, Inf, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(!r3.should_trade, "Inf bankroll → !should_trade");

    auto r4 = sizer.compute(0.60, 0.50, 1000.0, NegInf, 0.0, false, 1.0, 0.0);
    CHECK(!r4.should_trade, "-Inf inventory → !should_trade");

    auto r5 = sizer.compute(0.60, 0.50, 1000.0, 0.0, NegInf, false, 1.0, 0.0);
    CHECK(!r5.should_trade, "-Inf vol_z → !should_trade");
}

// ── Test 3: Combined NaN (NaN propagates only through one path) ────────────────

static void test_combined_nan() {
    std::printf("combined_nan\n");
    KellySizer sizer;

    // Two NaN inputs at once — still should be rejected
    auto r1 = sizer.compute(QNaN, QNaN, 1000.0, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(!r1.should_trade, "NaN market_price + NaN p_model → !should_trade");

    // All NaN
    auto r2 = sizer.compute(QNaN, QNaN, QNaN, QNaN, QNaN, false, QNaN, QNaN);
    CHECK(!r2.should_trade, "All NaN → !should_trade");

    // NaN + Inf mixed
    auto r3 = sizer.compute(Inf, 0.50, QNaN, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(!r3.should_trade, "Inf price + NaN bankroll → !should_trade");
}

// ── Test 4: Output sanity for valid inputs ─────────────────────────────────────

static void test_valid_output_is_finite() {
    std::printf("valid_output_is_finite\n");
    Config cfg;
    cfg.max_bankroll_pct = 0.05;
    KellySizer sizer(cfg);

    auto r = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, false, 1.0, 0.0);

    CHECK(r.should_trade, "valid inputs → should_trade");
    CHECK(std::isfinite(r.size_shares), "size_shares finite");
    CHECK(std::isfinite(r.size_usdc), "size_usdc finite");
    CHECK(std::isfinite(r.kelly_fractioned), "kelly_fractioned finite");
    CHECK(std::isfinite(r.fee_cost), "fee_cost finite");
    CHECK(std::isfinite(r.edge), "edge finite");

    // kelly_fractioned must respect max_bankroll_pct cap
    CHECK(r.kelly_fractioned <= cfg.max_bankroll_pct + 1e-9,
          "kelly_fractioned <= max_bankroll_pct (capped)");

    // size_usdc must match capped fraction * bankroll
    CHECK(std::fabs(r.size_usdc - r.kelly_fractioned * 1000.0) <= 1e-6,
          "size_usdc == kelly_fractioned * bankroll (post-cap)");
}

// ── Test 5: Boundary values ───────────────────────────────────────────────────

static void test_boundary_values() {
    std::printf("boundary_values\n");
    KellySizer sizer;

    // market_price = 0.0 → degenerate
    auto r1 = sizer.compute(0.0, 0.50, 1000.0, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(!r1.should_trade, "market_price=0.0 → !should_trade");

    // market_price = 1.0 → degenerate (odds = 0 → div-by-zero)
    auto r2 = sizer.compute(0.60, 1.0, 1000.0, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(!r2.should_trade, "market_price=1.0 → !should_trade (guard rejects)");

    // p_model = 0.0 → kelly_raw <= 0 → !should_trade
    auto r3 = sizer.compute(0.0, 0.50, 1000.0, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(!r3.should_trade, "p_model=0.0 → !should_trade");

    // p_model = 1.0 → valid (100% certainty), Kelly = full bet
    auto r4 = sizer.compute(1.0, 0.50, 1000.0, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(r4.should_trade, "p_model=1.0 → should_trade (valid)");

    // bankroll = 0.0 → should not trade (no capital)
    auto r5 = sizer.compute(0.60, 0.50, 0.0, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(!r5.should_trade || r5.size_usdc == 0.0,
          "bankroll=0.0 → no size");
}

// ── Test 6: ofi_adjust_max boundary (exactly 1.5) ─────────────────────────────

static void test_ofi_adjust_boundary() {
    std::printf("ofi_adjust_boundary\n");
    Config cfg;
    cfg.ofi_adjust_max = 1.5;
    KellySizer sizer(cfg);

    // ofi_mult exactly at max → uses max
    auto r1 = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, true, 1.5, 0.0);
    CHECK(r1.should_trade, "ofi_mult=1.5 (=max) → should_trade");

    // ofi_mult just above max → clamped
    auto r2 = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, true, 1.5001, 0.0);
    CHECK(r2.should_trade, "ofi_mult=1.5001 (clamped) → should_trade");

    // Verify clamping: r2 size should not exceed r1 size significantly
    // (both use capped ofi=1.5, so sizes should be equal)
    CHECK(std::fabs(r1.size_usdc - r2.size_usdc) < 1e-6,
          "ofi=1.5 and ofi=1.5001 (clamped) produce equal sizes");
}

// ── Test 7: CalibrationGuard exact reduction ───────────────────────────────────

static void test_calibration_exact() {
    std::printf("calibration_exact\n");
    Config cfg;
    cfg.kelly_fraction = 0.25;
    cfg.ece_threshold = 0.05;
    cfg.ece_decay_rate = 0.1;
    KellySizer sizer(cfg);

    // ECE=0 → no reduction
    auto r0 = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, false, 1.0, 0.0);

    // ECE=0.10 → reduction = (0.10 - 0.05) * 0.1 = 0.005 → fraction *= 0.995
    auto r1 = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, false, 1.0, 0.10);
    double expected_ratio = 0.995;
    double actual_ratio = r1.kelly_fractioned / r0.kelly_fractioned;
    CHECK(std::fabs(actual_ratio - expected_ratio) < 1e-4,
          "ECE=0.10 → fraction *= 0.995 (exact reduction)");
}

// ── Test 8: Floor boundary (exactly 1 share) ───────────────────────────────────

static void test_floor_boundary() {
    std::printf("floor_boundary\n");
    // With bankroll=3.0, price=0.50, kelly=0.25:
    // size_usdc = min(kelly_raw*0.25, 0.05) * 3.0 ≈ 0.15
    // size_shares = 0.15 / 0.50 = 0.3 → floor → should_trade=false
    KellySizer sizer;
    auto r = sizer.compute(0.60, 0.50, 3.0, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(!r.should_trade, "size < 1 share → !should_trade (floor)");
    CHECK(r.size_shares == 0.0, "size_shares=0 after floor");
}

// ── Test 9: fee_cost verification ──────────────────────────────────────────────

static void test_fee_cost() {
    std::printf("fee_cost\n");
    KellySizer sizer;

    // Taker fee: 0.072 * market_price * (1.0 - market_price)
    // market_price=0.50 → fee = 0.072 * 0.50 * 0.50 = 0.018
    auto r = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, true, 1.0, 0.0);
    CHECK(std::fabs(r.fee_cost - 0.018) < 1e-9,
          "taker fee = 0.072 * 0.50 * 0.50 = 0.018 (exact)");

    // Maker fee: 0.0 (is_taker=false)
    auto r2 = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, false, 1.0, 0.0);
    CHECK(r2.fee_cost == 0.0, "maker fee = 0.0");
}

// ── Test 10: Negative ofi_size_multiplier ──────────────────────────────────────

static void test_negative_ofi() {
    std::printf("negative_ofi\n");
    KellySizer sizer;
    // Negative ofi_size_multiplier should be clamped to 0.0 via max(0.0, ...)
    // This means ofi_adjust = 0 → kelly_fractioned *= 0 → size = 0 → !should_trade
    // (unless the floor is met, which it won't be with 0 size)
    auto r = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, true, -5.0, 0.0);
    CHECK(!r.should_trade || r.size_usdc == 0.0,
          "negative ofi_mult → clamped to 0 → no size");
}

int main() {
    std::printf("=== test_kelly_adversarial (ADR-0004) ===\n");

    test_nan_all_params();
    test_inf_all_params();
    test_combined_nan();
    test_valid_output_is_finite();
    test_boundary_values();
    test_ofi_adjust_boundary();
    test_calibration_exact();
    test_floor_boundary();
    test_fee_cost();
    test_negative_ofi();

    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);

    return g_failures == 0 ? 0 : 1;
}
