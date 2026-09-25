/**
 * Test Suite for FeeModel (Phase 4: Commission, Gas & Slippage Model)
 *
 * Tests:
 *   1. Dynamic fee maximum near p=0.5
 *   2. Dynamic fee minimum at p=0 or p=1
 *   3. Cost increases with order size and spread
 *   4. Positive edge but net_ev < min_net_ev → NOT_PROFITABLE
 *   5. Configurable parameters (dynamic_C, dynamic_fees_enabled flag)
 *   6. Slippage estimation scales with depth ratio
 *   7. Gas cost estimation
 *
 * Dependencies: fee_model.hpp, market_config.hpp, tick_result.hpp
 * These headers are independent of order_manager.hpp / telemetry.hpp / execution_engine.cpp,
 * enabling isolated compilation for fast feedback.
 */
#include "fee_model.hpp"
#include "tick_result.hpp"

#include <cmath>
#include <cstdio>
#include <string>

/* ───── Test framework (minimal, no external deps) ───── */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_BEGIN(name_) do { \
    g_tests_run++; \
    printf("  [Test] %s ... ", name_); \
} while(0)

#define TEST_CHECK(cond, detail) do { \
    if (cond) { \
        g_tests_passed++; \
        printf("✅ PASS"); \
    } else { \
        g_tests_failed++; \
        printf("❌ FAIL — %s", detail); \
    } \
    printf("\n"); \
} while(0)

struct TestCase {
    const char* name;
    bool passed;
};
static TestCase g_cases[64];
static int g_case_idx = 0;

#define CASE(name_, cond_) do { \
    g_cases[g_case_idx].name = name_; \
    g_cases[g_case_idx].passed = (cond_); \
    if (cond_) g_tests_passed++; else g_tests_failed++; \
    g_case_idx++; \
} while(0)

static bool approx_equal(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

/* ───── Test helpers ───── */

/** Build a RiskConfig with dynamic fees enabled and sensible defaults. */
static RiskConfig make_default_config() {
    RiskConfig cfg;
    cfg.maker_fee_rate         = 0.020;   // 2.0%
    cfg.taker_fee_rate         = 0.035;   // 3.5%
    cfg.base_commission_usd    = 0.10;    // $0.10
    cfg.dynamic_fees_enabled   = true;    // ENABLED
    cfg.dynamic_C              = 0.075;  // C constant
    cfg.gas_cost_usd           = 0.005;   // $0.005 per tx (Polygon)
    cfg.min_net_ev_usd         = 0.50;   // $0.50 minimum net EV
    return cfg;
}

/* ───── Test 1: Fee maximum near p=0.5 ───── */

static void test_fee_maximum_at_p05() {
    RiskConfig cfg = make_default_config();
    FeeModel model(cfg);
    const double notional = 100.0;

    // The formula: fee = C × 0.25 × (p·(1−p))² × notional
    // At p=0.5: (0.5 × 0.5)² = 0.0625, fee = C × 0.25 × 0.0625 × notional
    double expected_max = cfg.dynamic_C * 0.25 * (0.5 * 0.5) * (0.5 * 0.5) * notional;

    double fee_03 = model.compute_dynamic_fee(notional, 0.3);
    double fee_05 = model.compute_dynamic_fee(notional, 0.5);
    double fee_07 = model.compute_dynamic_fee(notional, 0.7);

    printf("    p=0.30 → %.10f  |  p=0.50 → %.10f (expected max: %.10f)  |  p=0.70 → %.10f\n",
           fee_03, fee_05, expected_max, fee_07);

    CASE("dynamic_fee(0.5) > dynamic_fee(0.3)", fee_05 > fee_03);
    CASE("dynamic_fee(0.5) > dynamic_fee(0.7)", fee_05 > fee_07);
    CASE("dynamic_fee(0.5) ≈ expected C×0.25×(0.25)²×N", approx_equal(fee_05, expected_max));
    CASE("dynamic_fee(0.3) == dynamic_fee(0.7) (symmetry)", approx_equal(fee_03, fee_07));
}

/* ───── Test 2: Fee minimum at p=0 or p=1 ───── */

static void test_fee_minimum_at_p0_p1() {
    RiskConfig cfg = make_default_config();
    FeeModel model(cfg);
    const double notional = 100.0;

    double fee_00 = model.compute_dynamic_fee(notional, 0.0);
    double fee_10 = model.compute_dynamic_fee(notional, 1.0);
    double fee_50 = model.compute_dynamic_fee(notional, 0.5);

    printf("    p=0.00 → %.10f  |  p=1.00 → %.10f  |  p=0.50 → %.10f\n",
           fee_00, fee_10, fee_50);

    CASE("dynamic_fee(0.0) == 0", approx_equal(fee_00, 0.0));
    CASE("dynamic_fee(1.0) == 0", approx_equal(fee_10, 0.0));
    CASE("dynamic_fee(0.0) < dynamic_fee(0.5)", fee_00 < fee_50);
    CASE("dynamic_fee(1.0) < dynamic_fee(0.5)", fee_10 < fee_50);
}

/* ───── Test 3a: Cost increases with order size ───── */

static void test_cost_increases_with_size() {
    RiskConfig cfg = make_default_config();
    FeeModel model(cfg);
    const double p = 0.5;

    double fee_small = model.compute_fee(100.0, true, p);
    double fee_large = model.compute_fee(1000.0, true, p);

    printf("    fee($100, maker) = %.6f  |  fee($1000, maker) = %.6f\n",
           fee_small, fee_large);

    CASE("fee increases with notional (maker)", fee_large > fee_small);

    double fee_taker = model.compute_fee(1000.0, false, p);
    double fee_maker = model.compute_fee(1000.0, true, p);
    printf("    maker=$50 + fees → %.6f  |  taker=$85 + fees → %.6f\n",
           fee_maker - cfg.base_commission_usd - cfg.gas_cost_usd,
           fee_taker - cfg.base_commission_usd - cfg.gas_cost_usd);

    CASE("taker fee rate > maker fee rate (0.035 > 0.020)", fee_taker > fee_maker);
}

/* ───── Test 3b: Slippage increases with spread and size ───── */

static void test_slippage_increases() {
    RiskConfig cfg = make_default_config();
    FeeModel model(cfg);
    const double liq = 5000.0;  // $5k available liquidity

    // 3b-i: Slippage increases with spread
    double slip_low  = model.estimate_slippage(100.0, 10.0, liq);  // 10 bps spread
    double slip_high = model.estimate_slippage(100.0, 50.0, liq);  // 50 bps spread
    printf("    slippage @10bps=$%.6f  |  slippage @50bps=$%.6f\n", slip_low, slip_high);
    CASE("slippage increases with spread", slip_high > slip_low);

    // 3b-ii: Slippage increases with order size (depth ratio effect)
    double slip_small = model.estimate_slippage(100.0, 20.0, liq);
    double slip_large = model.estimate_slippage(2500.0, 20.0, liq);
    printf("    slippage @size=$100=$%.6f  |  slippage @size=$2500=$%.6f\n",
           slip_small, slip_large);
    CASE("slippage increases with order size", slip_large > slip_small);

    // 3b-iii: Fallback when liquidity is zero
    double slip_fallback = model.estimate_slippage(100.0, 20.0, 0.0);
    printf("    slippage @no-liquidity (1%% fallback) =$%.6f\n", slip_fallback);
    CASE("zero liquidity → 1% fallback slippage", approx_equal(slip_fallback, 100.0 * 0.01));
}

/* ───── Test 4: Positive edge but net_ev < min → NOT_PROFITABLE ───── */

static void test_net_ev_filter() {
    RiskConfig cfg = make_default_config();
    cfg.min_net_ev_usd = 0.50;
    FeeModel model(cfg);

    // Scenario: order with positive gross edge but high costs.
    // Edge = $1.0, notional = $100, maker, p=0.5, spread=500bps, liquidity=$100
    //
    // Breakdown:
    //   base_fee      = 100 × 0.020 = $2.00
    //   dynamic_fee   = 0.075 × 0.25 × (0.25)² × 100 = $0.117188
    //   commission    = $0.10
    //   gas           = $0.005
    //   slippage      = 100 × (500/10000) × (1 + 0.5 × (100/100)²) = $5.00 × (1 + 0.5) = $7.50
    //   total_costs   ≈ $9.72
    //   net_ev        = $1.00 - $9.72 ≈ -$8.72  → NOT_PROFITABLE
    //
    // The gross edge ($1) is positive, but after deducting fees + slippage + gas,
    // net_ev is deeply negative and far below the $0.50 min threshold.

    double edge_usd            = 1.0;    // positive gross edge
    double notional_usd        = 100.0;
    double p                   = 0.5;
    double spread_bps          = 500.0;  // very wide spread
    double available_liquidity = 100.0;  // shallow depth

    double net_ev = model.compute_net_ev(
        edge_usd, notional_usd, /*is_maker=*/true,
        p, spread_bps, available_liquidity
    );

    printf("    edge=$%.2f  notional=$%.2f  spread=%.0fbps  liquidity=$%.0f\n",
           edge_usd, notional_usd, spread_bps, available_liquidity);
    printf("    net_ev=$%.4f  min_net_ev=$%.2f\n", net_ev, cfg.min_net_ev_usd);

    CASE("edge > 0", edge_usd > 0.0);
    CASE("net_ev < edge (costs are deducted)", net_ev < edge_usd);
    CASE("net_ev < 0 (deeply negative)", net_ev < 0.0);

    // Simulate the ExecutionEngine filter logic
    TickResult result = (net_ev < cfg.min_net_ev_usd)
                        ? TickResult::NOT_PROFITABLE
                        : TickResult::OK;
    CASE("net_ev < min → NOT_PROFITABLE", result == TickResult::NOT_PROFITABLE);
    CASE("NOT_PROFITABLE != OK", result != TickResult::OK);
}

/* ───── Test 5: Configurable dynamic_C ───── */

static void test_configurable_dynamic_c() {
    RiskConfig cfg = make_default_config();

    // Double the C constant
    RiskConfig cfg2 = cfg;
    cfg2.dynamic_C = cfg.dynamic_C * 2.0;

    FeeModel m1(cfg);
    FeeModel m2(cfg2);

    double fee1 = m1.compute_dynamic_fee(100.0, 0.5);
    double fee2 = m2.compute_dynamic_fee(100.0, 0.5);
    printf("    dynamic_C=0.075 → $%.10f  |  dynamic_C=0.150 → $%.10f\n", fee1, fee2);
    CASE("fee scales linearly with C", approx_equal(fee2, fee1 * 2.0, 1e-10));
}

/* ───── Test 6: Dynamic fees toggle ───── */

static void test_dynamic_fees_toggle() {
    RiskConfig cfg = make_default_config();
    cfg.dynamic_fees_enabled = true;
    FeeModel m_dyn(cfg);

    RiskConfig cfg_off = cfg;
    cfg_off.dynamic_fees_enabled = false;
    FeeModel m_off(cfg_off);

    double with_dyn = m_dyn.compute_fee(100.0, true, 0.5);
    double no_dyn   = m_off.compute_fee(100.0, true, 0.5);
    printf("    fee with dynamic  =$%.6f  |  fee without dynamic=$%.6f\n", with_dyn, no_dyn);
    CASE("fee with dynamic > fee without (when p=0.5)", with_dyn > no_dyn);
}

/* ───── Test 7: Gas cost estimation ───── */

static void test_gas_cost() {
    RiskConfig cfg = make_default_config();
    cfg.gas_cost_usd = 0.005;
    FeeModel model(cfg);

    double gas_1 = model.estimate_gas_cost(1);   // 1 tx
    double gas_5 = model.estimate_gas_cost(5);   // 5 txs
    printf("    gas(1 tx)=$%.6f  |  gas(5 tx)=$%.6f\n", gas_1, gas_5);
    CASE("gas cost = base × n_txs", approx_equal(gas_1, 0.005));
    CASE("gas scales linearly with tx count", approx_equal(gas_5, 0.025));
}

/* ───── Main ───── */

int main() {
    printf("========================================\n");
    printf(" FeeModel — Phase 4 Test Suite\n");
    printf(" Formula: fee = C × 0.25 × (p·(1−p))²\n");
    printf("========================================\n\n");

    test_fee_maximum_at_p05();
    test_fee_minimum_at_p0_p1();
    test_cost_increases_with_size();
    test_slippage_increases();
    test_net_ev_filter();
    test_configurable_dynamic_c();
    test_dynamic_fees_toggle();
    test_gas_cost();

    printf("\n========================================\n");

    // Print individual case results and tally
    int passed_count = 0;
    for (int i = 0; i < g_case_idx; i++) {
        if (g_cases[i].passed) {
            passed_count++;
            printf("  ✅ %s\n", g_cases[i].name);
        } else {
            printf("  ❌ %s\n", g_cases[i].name);
        }
    }

    int failed_count = g_case_idx - passed_count;
    printf("========================================\n");
    printf(" Summary: %d cases run, %d passed, %d failed\n",
           g_case_idx, passed_count, failed_count);
    printf("========================================\n");

    if (failed_count == 0) {
        printf("\n✅ All %d cases PASSED\n", g_case_idx);
        return 0;
    } else {
        printf("\n❌ %d/%d cases FAILED\n", failed_count, g_case_idx);
        return 1;
    }
}
