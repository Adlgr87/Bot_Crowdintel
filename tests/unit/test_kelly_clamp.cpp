// ─────────────────────────────────────────────────────────────────────────────
// test_kelly_clamp: unit tests for OBS-02 ofi_adjust clamps and CalibrationGuard
//
// Verifies:
//   1. ofi_adjust is clamped to ofi_adjust_max (prevents over-bet when ofi_mult > 1)
//   2. CalibrationGuard reduces kelly_fraction when ECE > threshold
//   3. vol_adjust does NOT amplify when vol_z < 0 (existing behavior, regression)
//
// VER-02 / OBS-02 fix
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdio>
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

static bool approx(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

// ── Test 1: ofi_adjust clamp ──────────────────────────────────────────────────

static void test_ofi_adjust_clamp() {
    std::printf("ofi_adjust_clamp\n");
    // Build config with explicit ofi_adjust_max
    Config cfg;
    cfg.ofi_adjust_max = 1.5;
    KellySizer sizer(cfg);

    // with ofi_mult=2.0, should clamp to 1.5 — no crash, should trade
    auto r1 = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, true,
                            /*ofi_mult=*/2.0, /*calibration_ece=*/0.0);
    CHECK(r1.should_trade, "ofi_mult=2.0 should still trade (clamped to 1.5x)");
    CHECK(r1.size_shares > 0, "size > 0 with clamped ofi");

    // without clamp (ofi_mult=1.0), should also trade
    auto r2 = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, true,
                            /*ofi_mult=*/1.0, /*calibration_ece=*/0.0);
    CHECK(r2.should_trade, "ofi_mult=1.0 should trade");

    // clamped result should be >= baseline (since ofi=2.0 clamped to 1.5x)
    CHECK(r1.size_usdc >= r2.size_usdc - 1e-6,
          "clamped ofi=2.0 >= ofi=1.0 (within clamp)");

    // ofi_mult=10.0 with clamp should still be within bankroll cap
    auto r3 = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, true,
                            /*ofi_mult=*/10.0, /*calibration_ece=*/0.0);
    CHECK(r3.size_usdc <= 1000.0 * 0.05 + 1e-6,
          "bankroll cap 5% respected even with ofi=10.0");
}

// ── Test 2: CalibrationGuard ──────────────────────────────────────────────────

static void test_calibration_gate() {
    std::printf("calibration_gate\n");
    Config cfg;
    cfg.kelly_fraction = 0.25;
    cfg.ece_threshold = 0.05;
    cfg.ece_decay_rate = 0.1;
    KellySizer sizer(cfg);

    // With ECE=0 (perfect), should trade normally
    auto r1 = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, true, 1.0, 0.0);
    CHECK(r1.should_trade, "ECE=0 should trade");
    CHECK(r1.size_shares > 0, "ECE=0 should have size");

    // With ECE=0.10 (above threshold), should reduce size
    auto r2 = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, true, 1.0, 0.10);
    CHECK(r2.should_trade, "ECE=0.10 should still trade");
    CHECK(r2.size_shares < r1.size_shares,
          "ECE=0.10 should reduce size below ECE=0");

    // With very high ECE (0.50), should reduce more
    auto r3 = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, true, 1.0, 0.50);
    CHECK(r3.size_shares < r2.size_shares,
          "ECE=0.50 should reduce size more than ECE=0.10");
}

// ── Test 3: vol_adjust does not amplify ────────────────────────────────────────

static void test_vol_adjust_no_amplification() {
    std::printf("vol_adjust_no_amplification\n");
    KellySizer sizer;  // default config (vol_adjust uses max(0, vol_z))

    // With negative vol_z, should NOT amplify (vol_adjust=1.0)
    auto r1 = sizer.compute(0.62, 0.50, 1000.0, 0.0, -2.0, true, 1.0, 0.0);
    CHECK(r1.should_trade, "vol_z=-2.0 should still trade");

    // With vol_z=0, baseline
    auto r2 = sizer.compute(0.62, 0.50, 1000.0, 0.0, 0.0, true, 1.0, 0.0);
    CHECK(r2.should_trade, "vol_z=0 should trade");

    // With positive vol_z, should attenuate
    auto r3 = sizer.compute(0.62, 0.50, 1000.0, 0.0, 2.0, true, 1.0, 0.0);
    CHECK(r3.should_trade, "vol_z=2.0 should still trade");

    // Negative vol_z should NOT amplify beyond baseline
    CHECK(r1.size_usdc <= r2.size_usdc + 1e-6,
          "vol_z=-2.0 should not amplify beyond vol_z=0");

    // Positive vol_z should reduce
    CHECK(r3.size_usdc < r2.size_usdc,
          "vol_z=2.0 should reduce size below vol_z=0");
}

// ── Test 4: Default config safety ─────────────────────────────────────────────

static void test_default_config_safety() {
    std::printf("default_config_safety\n");
    Config cfg;  // default-constructed, should have safe values
    CHECK(approx(cfg.ofi_adjust_max, 1.5), "default ofi_adjust_max=1.5");
    CHECK(approx(cfg.ece_threshold, 0.05), "default ece_threshold=0.05");
    CHECK(approx(cfg.ece_decay_rate, 0.1), "default ece_decay_rate=0.1");

    // Verify KellySizer can be built with default config
    KellySizer sizer(cfg);
    auto r = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, true, 1.0, 0.0);
    CHECK(r.should_trade, "default config should trade");
}

int main() {
    std::printf("=== test_kelly_clamp (OBS-02) ===\n");

    test_ofi_adjust_clamp();
    test_calibration_gate();
    test_vol_adjust_no_amplification();
    test_default_config_safety();

    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);

    return g_failures == 0 ? 0 : 1;
}
