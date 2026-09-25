/**
 * test_kelly_slippage.cpp — Unit tests for the depth-aware Kelly engine.
 *
 * Covered scenarios:
 *   1. Backward-compatible calculate_fractional_kelly still works
 *   2. Slippage increases with order size / depth ratio
 *   3. Kelly shrinks position when slippage approaches edge
 *   4. Unprofitable after slippage → position = 0
 *   5. Depth ratio penalty — position capped relative to book depth
 *   6. High confidence + deep liquidity → full Kelly fraction
 *   7. Low confidence → reduced position
 *   8. Spread sensitivity — wider spreads reduce position
 *   9. KellyResult fields are internally consistent
 *  10. Edge case: zero liquidity → position = 0
 *
 * Dependencies: kelly_engine.hpp, order_book.hpp
 * Headers are header-only — no curl/secp256k1 needed.
 */

#include "kelly_engine.hpp"
#include "order_book.hpp"

#include <cstdint>
#include <cstdio>
#include <cmath>

/* ───── Test framework (minimal, no external deps) ───── */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_BEGIN(name_) do { \
    g_tests_run++; \
    printf("  ⏳ [" #name_ "] ... "); \
    fflush(stdout); \
} while(0)

#define TEST_CHECK(cond, detail) do { \
    if (cond) { \
        g_tests_passed++; \
        printf("✅ PASS\n"); \
    } else { \
        g_tests_failed++; \
        printf("❌ FAIL — %s\n", detail); \
        return 1; \
    } \
} while(0)

struct TestCase {
    const char* name;
    bool passed;
};
static TestCase g_cases[80];
static int g_case_idx = 0;

#define CASE(name_, cond_) do { \
    g_cases[g_case_idx].name = name_; \
    g_cases[g_case_idx].passed = (cond_); \
    if (cond_) g_tests_passed++; else g_tests_failed++; \
    g_case_idx++; \
} while(0)

static bool approx_eq_rel(double a, double b, double rel_eps = 1e-6) {
    double denom = (std::fabs(a) + std::fabs(b)) / 2.0;
    if (denom < 1e-12) return std::fabs(a - b) < 1e-12;
    return std::fabs(a - b) / denom < rel_eps;
}

static KellyEngine::SlippageParams make_default_params() {
    KellyEngine::SlippageParams params;
    params.spread_bps = 20.0;           // 20 bps spread
    params.available_liquidity = 5000.0; // $5k at top of book
    params.gas_cost_usd = 0.005;        // $0.005 Polygon gas
    params.maker_fee_rate = 0.020;      // 2% maker
    params.taker_fee_rate = 0.035;      // 3.5% taker
    params.base_commission_usd = 0.10;  // $0.10 commission
    params.min_net_ev_usd = 0.50;       // $0.50 min
    params.probability = 0.5;           // 50% market prob
    params.dynamic_fees_enabled = false;
    params.dynamic_C = 0.075;
    params.is_maker = false;            // Default: taker (market order)
    return params;
}

/* ───── Test 1: Backward-compatible Kelly still works ───── */

static void test_backward_compatible_kelly() {
    TEST_BEGIN("backward_compatible_kelly");

    double f = KellyEngine::calculate_fractional_kelly(0.05, 0.9, 0.1);
    printf("    f* = %.6f (ev=0.05, conf=0.9, fraction=0.1)\n", f);

    CASE("fractional_kelly > 0 for positive edge", f > 0.0);
    CASE("fractional_kelly < 1.0 (fractional)", f < 1.0);
    CASE("fractional_kelly ≈ 0.05*0.9*0.1 = 0.0045", approx_eq_rel(f, 0.0045));

    // Negative EV → zero
    double f2 = KellyEngine::calculate_fractional_kelly(-0.05, 0.9, 0.1);
    CASE("fractional_kelly = 0 for negative edge", f2 == 0.0);

    // Position size
    double pos = KellyEngine::calculate_position_size(0.0045, 10000.0);
    CASE("position_size = 0.0045 * 10000 = 45", approx_eq_rel(pos, 45.0));
}

/* ───── Test 2: Slippage increases with order size ───── */

static void test_slippage_increases_with_size() {
    TEST_BEGIN("slippage_increases_with_size");

    double spread = 20.0;      // 20 bps
    double liq = 5000.0;        // $5k

    double slip_small = KellyEngine::estimate_slippage(100.0, spread, liq);
    double slip_large = KellyEngine::estimate_slippage(2500.0, spread, liq);

    printf("    slippage($100)  = $%.4f  (depth_ratio=%.2f)\n",
           slip_small, 100.0/5000.0);
    printf("    slippage($2500) = $%.4f  (depth_ratio=%.2f)\n",
           slip_large, 2500.0/5000.0);

    CASE("slippage($2500) > slippage($100)", slip_large > slip_small);

    // At size = depth (ratio=1), impact adds 50% on top of spread cost
    double slip_at_depth = KellyEngine::estimate_slippage(5000.0, spread, liq);
    double spread_cost = 5000.0 * (spread / 10000.0);  // $5 * (1 + 0.5 * 1.0) = $7.50
    printf("    slippage at depth=$5000 = $%.4f (expected $%.4f)\n",
           slip_at_depth, spread_cost * 1.5);
    CASE("slippage at depth ≈ 1.5× spread cost", approx_eq_rel(slip_at_depth, spread_cost * 1.5));
}

/* ───── Test 3: Kelly shrinks position when slippage is high ───── */

static void test_slippage_penalty_reduces_position() {
    TEST_BEGIN("slippage_penalty_reduces_position");

    // Scenario: high EV, deep liquidity → large position
    auto params_deep = make_default_params();
    params_deep.available_liquidity = 100000.0;  // Very deep
    params_deep.spread_bps = 10.0;               // Tight spread

    auto result_deep = KellyEngine::calculate_kelly_with_slippage(
        0.1, 0.9, 10000.0, params_deep, 0.25);
    printf("    Deep liq:  pos=$%.2f  kelly_f=%.4f  adj_f=%.4f  net_ev=$%.2f\n",
           result_deep.position_size_usd,
           result_deep.kelly_fraction,
           result_deep.adjusted_fraction,
           result_deep.net_edge_usd);

    // Scenario: same EV, shallow liquidity → much smaller position
    auto params_shallow = make_default_params();
    params_shallow.available_liquidity = 1000.0;  // Shallow
    params_shallow.spread_bps = 100.0;            // Wide spread

    auto result_shallow = KellyEngine::calculate_kelly_with_slippage(
        0.1, 0.9, 10000.0, params_shallow, 0.25);
    printf("    Shallow:  pos=$%.2f  kelly_f=%.4f  adj_f=%.4f  net_ev=$%.2f\n",
           result_shallow.position_size_usd,
           result_shallow.kelly_fraction,
           result_shallow.adjusted_fraction,
           result_shallow.net_edge_usd);

    CASE("shallow liquidity → smaller position",
         result_shallow.position_size_usd < result_deep.position_size_usd);

    // Adjusted fraction should be less than raw kelly_f
    CASE("adjusted_f <= kelly_f (penalty applied)",
         result_shallow.adjusted_fraction <= result_shallow.kelly_fraction);

    // Deep liquidity position should be larger
    CASE("deep position > $100", result_deep.position_size_usd > 100.0);
}

/* ───── Test 4: Unprofitable after slippage → position = 0 ───── */

static void test_unprofitable_eliminates_position() {
    TEST_BEGIN("unprofitable_eliminates_position");

    auto params = make_default_params();
    // Very shallow liquidity + wide spread + high fee
    params.available_liquidity = 50.0;     // Only $50 at top of book
    params.spread_bps = 500.0;             // 500 bps spread
    params.min_net_ev_usd = 50.0;          // High min EV threshold

    // Small edge, small max position
    auto result = KellyEngine::calculate_kelly_with_slippage(
        0.01, 0.5, 1000.0, params, 0.5);
    printf("    pos=$%.2f  gross_edge=$%.2f  net_edge=$%.2f  profitable=%d\n",
           result.position_size_usd,
           result.gross_edge_usd,
           result.net_edge_usd,
           result.is_profitable);

    CASE("unprofitable → position_size = 0", result.position_size_usd == 0.0);
    CASE("unprofitable → not is_profitable", !result.is_profitable);
    CASE("unprofitable → rejection_reason set", result.rejection_reason[0] != '\0');
}

/* ───── Test 5: Depth ratio capping (size_to_depth) ───── */

static void test_depth_ratio_capping() {
    TEST_BEGIN("depth_ratio_capping");

    // Kelly fraction suggests $5000, but available liquidity is only $1000
    // Max depth ratio = 0.3 → max safe = $300
    double pos = KellyEngine::size_to_depth(0.5, 10000.0, 1000.0, 0.3);
    printf("    size_to_depth(0.5, $10k, $1k liq, 0.3) = $%.2f (expected $300)\n", pos);
    CASE("position capped to 30% of depth", approx_eq_rel(pos, 300.0));

    // When gross position < cap, it's not reduced
    double pos2 = KellyEngine::size_to_depth(0.01, 10000.0, 100000.0, 0.3);
    printf("    size_to_depth(0.01, $10k, $100k liq, 0.3) = $%.2f (expected $100)\n", pos2);
    CASE("position below cap = not reduced", approx_eq_rel(pos2, 100.0));

    // Zero liquidity → 0
    double pos3 = KellyEngine::size_to_depth(0.5, 10000.0, 0.0, 0.3);
    CASE("zero liquidity → position = 0", pos3 == 0.0);
}

/* ───── Test 6: High confidence + deep liquidity → full Kelly fraction ───── */

static void test_high_confidence_deep_liquidity() {
    TEST_BEGIN("high_confidence_deep_liquidity");

    auto params = make_default_params();
    params.available_liquidity = 500000.0;  // Very deep ($500k)
    params.spread_bps = 5.0;                // Very tight spread
    params.is_maker = true;                 // Maker (lower fees)
    // Use minimal fees to isolate slippage effects
    params.maker_fee_rate = 0.001;          // 0.1% maker
    params.base_commission_usd = 0.001;     // Minimal commission
    params.gas_cost_usd = 0.001;            // Minimal gas

    auto result = KellyEngine::calculate_kelly_with_slippage(
        0.05, 0.95, 10000.0, params, 0.25);

    printf("    pos=$%.2f  kelly_f=%.4f  adj_f=%.4f  net_ev=$%.2f  profitable=%d\n",
           result.position_size_usd,
           result.kelly_fraction,
           result.adjusted_fraction,
           result.net_edge_usd,
           result.is_profitable);

    CASE("high conf + deep liq → is_profitable", result.is_profitable);
    // With deep liquidity and minimal fees, slippage should be negligible
    // Adjusted fraction should be close to raw (penalty near 1.0)
    double ratio = result.adjusted_fraction / result.kelly_fraction;
    printf("    adjusted/raw ratio = %.4f\n", ratio);
    CASE("adjusted fraction ≈ raw (low slippage)", ratio > 0.9);
}

/* ───── Test 7: Low confidence → reduced position ───── */

static void test_low_confidence() {
    TEST_BEGIN("low_confidence");

    auto params = make_default_params();
    params.available_liquidity = 10000.0;

    auto result_high = KellyEngine::calculate_kelly_with_slippage(
        0.05, 0.95, 10000.0, params, 0.25);

    auto result_low = KellyEngine::calculate_kelly_with_slippage(
        0.05, 0.30, 10000.0, params, 0.25);

    printf("    high conf: pos=$%.2f  f=%.4f\n",
           result_high.position_size_usd, result_high.kelly_fraction);
    printf("    low conf:  pos=$%.2f  f=%.4f\n",
           result_low.position_size_usd, result_low.kelly_fraction);

    CASE("low conf → smaller kelly_f", result_low.kelly_fraction < result_high.kelly_fraction);
    CASE("low conf → smaller position", result_low.position_size_usd < result_high.position_size_usd);
}

/* ───── Test 8: Spread sensitivity ───── */

static void test_spread_sensitivity() {
    TEST_BEGIN("spread_sensitivity");

    auto params_tight = make_default_params();
    params_tight.spread_bps = 5.0;    // Tight spread

    auto params_wide = make_default_params();
    params_wide.spread_bps = 200.0;   // Wide spread

    // Same EV, same confidence, same max position
    auto result_tight = KellyEngine::calculate_kelly_with_slippage(
        0.05, 0.9, 10000.0, params_tight, 0.25);

    auto result_wide = KellyEngine::calculate_kelly_with_slippage(
        0.05, 0.9, 10000.0, params_wide, 0.25);

    printf("    tight spread: pos=$%.2f  net_ev=$%.2f\n",
           result_tight.position_size_usd, result_tight.net_edge_usd);
    printf("    wide spread:  pos=$%.2f  net_ev=$%.2f\n",
           result_wide.position_size_usd, result_wide.net_edge_usd);

    CASE("wide spread → smaller position",
         result_wide.position_size_usd < result_tight.position_size_usd);
    CASE("wide spread → lower net_ev",
         result_wide.net_edge_usd < result_tight.net_edge_usd);
}

/* ───── Test 9: KellyResult internal consistency ───── */

static void test_kelly_result_consistency() {
    TEST_BEGIN("kelly_result_consistency");

    auto params = make_default_params();

    auto result = KellyEngine::calculate_kelly_with_slippage(
        0.05, 0.8, 10000.0, params, 0.25);

    printf("    gross_edge=$%.2f  net_edge=$%.2f  cost=$%.2f\n",
           result.gross_edge_usd, result.net_edge_usd, result.expected_slippage_usd);
    printf("    kelly_f=%.4f  adj_f=%.4f  depth_ratio=%.4f\n",
           result.kelly_fraction, result.adjusted_fraction, result.depth_ratio);

    // net_edge = gross_edge - cost (at adjusted size)
    double recomputed_net = result.gross_edge_usd - result.expected_slippage_usd;
    CASE("net_edge ≈ gross_edge - cost", approx_eq_rel(result.net_edge_usd, recomputed_net, 1e-3));

    // adjusted_fraction <= kelly_fraction (penalty can only shrink)
    CASE("adjusted_fraction <= kelly_fraction", result.adjusted_fraction <= result.kelly_fraction + 1e-12);

    // position_size = adjusted_fraction * max_position
    double expected_pos = result.adjusted_fraction * 10000.0;
    CASE("position_size ≈ adj_fraction * max_pos", approx_eq_rel(result.position_size_usd, expected_pos, 1e-3));

    // depth_ratio = position / available_liquidity (when liq > 0)
    double expected_depth = result.position_size_usd / params.available_liquidity;
    CASE("depth_ratio ≈ pos / liquidity", approx_eq_rel(result.depth_ratio, expected_depth, 1e-3));

    // is_profitable should be consistent with net_edge vs min_net_ev
    CASE("is_profitable consistent", result.is_profitable == (result.net_edge_usd >= params.min_net_ev_usd));
}

/* ───── Test 10: Zero liquidity → position = 0 ───── */

static void test_zero_liquidity() {
    TEST_BEGIN("zero_liquidity");

    auto params = make_default_params();
    params.available_liquidity = 0.0;

    auto result = KellyEngine::calculate_kelly_with_slippage(
        0.05, 0.9, 10000.0, params, 0.25);

    printf("    pos=$%.2f  net_ev=$%.2f  profitable=%d\n",
           result.position_size_usd, result.net_edge_usd, result.is_profitable);

    CASE("zero liquidity → position = 0", result.position_size_usd == 0.0);
    CASE("zero liquidity → not profitable", !result.is_profitable);
    CASE("zero liquidity → rejection_reason set", result.rejection_reason[0] != '\0');
}

/* ───── Test 11: Fee cost composition ───── */

static void test_fee_composition() {
    TEST_BEGIN("fee_composition");

    auto params = make_default_params();
    params.maker_fee_rate = 0.001;      // 0.1% maker
    params.taker_fee_rate = 0.003;      // 0.3% taker
    params.base_commission_usd = 0.10;
    params.gas_cost_usd = 0.005;
    params.dynamic_fees_enabled = false;

    double notional = 1000.0;
    double cost_taker = KellyEngine::estimate_total_cost(notional, params);

    printf("    cost breakdown at $1000 (taker):\n");
    printf("      maker fee:        $%.4f\n", notional * params.taker_fee_rate);
    printf("      commission:       $%.2f\n", params.base_commission_usd);
    printf("      gas:              $%.3f\n", params.gas_cost_usd);
    printf("      slippage:         $%.4f\n",
           KellyEngine::estimate_slippage(notional, params.spread_bps,
                                          params.available_liquidity));
    printf("      total:            $%.4f\n", cost_taker);

    // Manual computation (for reference)
    double expected2 = notional * params.taker_fee_rate
                     + params.base_commission_usd
                     + params.gas_cost_usd
                     + KellyEngine::estimate_slippage(notional, params.spread_bps,
                                                      params.available_liquidity);

    CASE("cost = fee + commission + gas + slippage", approx_eq_rel(cost_taker, expected2, 1e-6));

    // Dynamic fees: at p=0.5, should add ~C × 0.25 × (0.25)² × notional
    params.dynamic_fees_enabled = true;
    params.dynamic_C = 0.075;
    double cost_dyn = KellyEngine::estimate_total_cost(notional, params);
    double expected_dynamic = expected2 + 0.075 * 0.25 * (0.5*0.5) * (0.5*0.5) * notional;
    printf("    with dynamic fee (p=0.5): $%.4f (expected $%.4f)\n",
           cost_dyn, expected_dynamic);
    CASE("dynamic fee adds probability-weighted cost", approx_eq_rel(cost_dyn, expected_dynamic, 1e-6));
    CASE("dynamic cost > base cost", cost_dyn > cost_taker);
}

/* ───── Main ───── */

int main() {
    printf("========================================\n");
    printf(" KellyEngine — Depth-Aware Slippage Model\n");
    printf("========================================\n\n");

    test_backward_compatible_kelly();
    test_slippage_increases_with_size();
    test_slippage_penalty_reduces_position();
    test_unprofitable_eliminates_position();
    test_depth_ratio_capping();
    test_high_confidence_deep_liquidity();
    test_low_confidence();
    test_spread_sensitivity();
    test_kelly_result_consistency();
    test_zero_liquidity();
    test_fee_composition();

    printf("\n========================================\n");

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
    printf(" Summary: %d cases, %d passed, %d failed\n",
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
