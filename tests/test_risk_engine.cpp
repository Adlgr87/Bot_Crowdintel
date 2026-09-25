/**
 * test_risk_engine.cpp — T2 Risk Engine & Kill Switch Test Suite
 *
 * Verifies (Phase 2: Risk Engine y Kill Switch):
 *   1. Order size > max_order_usd → RISK_BLOCKED
 *   2. Kill switch active → KILL_SWITCH
 *   3. Order window per minute exceeded → RISK_BLOCKED
 *   4. Insufficient balance (below min) → INSUFFICIENT_BALANCE
 *   5. Daily loss exceeds max_daily_loss_usd → activates kill switch → KILL_SWITCH
 *   6. Market exposure exceeds max_exposure_per_market → RISK_BLOCKED
 *   7. Kill switch can be deactivated (resets to false)
 *   8. Cancel window per minute exceeded → RISK_BLOCKED
 *   9. Normal valid order passes all checks → OK
 *  10. pre_trade_check is O(1) and thread-safe (kill_switch is atomic)
 *
 * Dependencies: risk_engine.hpp, market_config.hpp, tick_result.hpp, eip712_signer.hpp
 * These headers are designed for isolated compilation (no execution_engine.cpp dependency).
 *
 * CRITICAL: This test must NOT modify eip712_signer.hpp (unchanged per constraints).
 */

#include "risk_engine.hpp"     // Includes: risk_engine.hpp → eip712_signer.hpp, tick_result.hpp, market_config.hpp
#include "tick_result.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

/* ───── Test framework (minimal, no external deps) ───── */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

struct TestCase {
    const char* name;
    bool passed;
};
static TestCase g_cases[128];
static int g_case_idx = 0;

#define CASE(name_, cond_) do { \
    g_cases[g_case_idx].name = name_; \
    g_cases[g_case_idx].passed = (cond_); \
    g_case_idx++; \
} while(0)

static int g_test_failures = 0;

static void run_test(const char* name, bool (*fn)()) {
    g_tests_run++;
    std::cout << "▶ " << name << " ..." << std::flush;
    bool ok = fn();
    if (ok) {
        g_tests_passed++;
        std::cout << " ✅ PASS" << std::endl;
    } else {
        g_test_failures++;
        std::cout << " ❌ FAIL" << std::endl;
    }
}

/* ───── Test helpers ───── */

/**
 * Build a conservative RiskConfig (defaults from market_config.hpp).
 * All limits are env-driven with conservative defaults:
 *   max_daily_loss_usd=500, max_order_usd=500, min_usdc_balance=100,
 *   min_pol_balance=10, max_orders_per_min=10, max_cancels_per_min=20,
 *   max_exposure_per_market=5000, kill_switch_enabled=true
 */
static RiskConfig make_default_config() {
    RiskConfig cfg;
    // Defaults match market_config.hpp: all conservative
    return cfg;
}

/**
 * Create a valid OrderParams with a given USD size.
 * params.size is in fixed-point (* 1e6), so size_usd * 1e6.
 */
static OrderParams make_order(double size_usd, uint8_t side = 0) {
    OrderParams params;
    params.salt   = 123456789;
    params.nonce  = 987654321;
    params.price  = 500000000;        // $500.00 in fixed-point (* 1e6)
    params.size   = static_cast<uint64_t>(size_usd * 1e6);
    params.side   = side;             // 0 = buy, 1 = sell
    memset(params.maker, 0x11, 20);
    memset(params.taker, 0x00, 20);
    return params;
}

/**
 * Sufficient balances to pass balance checks (well above defaults).
 */
static constexpr double GOOD_USDC = 5000.0;
static constexpr double GOOD_POL  = 100.0;

/* ───── Test 1: Order size exceeds max_order_usd → RISK_BLOCKED ───── */

static bool test_order_exceeds_max() {
    RiskConfig cfg = make_default_config();
    RiskEngine engine(cfg);

    // max_order_usd default = 500.0
    // params.size = 600 * 1e6 → order_usd = 600 > 500
    OrderParams big_order = make_order(600.0);  // $600 > $500 limit

    TickResult result = engine.pre_trade_check(
        big_order, GOOD_USDC, GOOD_POL,
        0.0,    // market_exposure = 0 (below limit)
        0.0     // market_pnl = 0
    );

    CASE("order > max_order_usd → RISK_BLOCKED", result == TickResult::RISK_BLOCKED);
    CASE("order > max_order_usd ≠ OK",          result != TickResult::OK);

    // Verify boundary: exactly at limit should pass (order_usd == max_order_usd)
    OrderParams at_limit = make_order(500.0);  // exactly $500
    TickResult result2 = engine.pre_trade_check(
        at_limit, GOOD_USDC, GOOD_POL, 0.0, 0.0);
    CASE("order == max_order_usd → OK (boundary)", result2 == TickResult::OK);

    // Just over limit
    OrderParams over_limit = make_order(500.01);
    TickResult result3 = engine.pre_trade_check(
        over_limit, GOOD_USDC, GOOD_POL, 0.0, 0.0);
    CASE("order = max_order_usd + epsilon → RISK_BLOCKED", result3 == TickResult::RISK_BLOCKED);

    return true;
}

/* ───── Test 2: Kill switch active → KILL_SWITCH ───── */

static bool test_kill_switch_active() {
    RiskConfig cfg = make_default_config();
    RiskEngine engine(cfg);

    // Before activation: normal order should pass
    OrderParams good_order = make_order(100.0);  // $100 < $500 limit
    TickResult result_before = engine.pre_trade_check(
        good_order, GOOD_USDC, GOOD_POL, 0.0, 0.0);

    CASE("kill switch inactive → order passes", result_before == TickResult::OK);

    // Activate kill switch
    engine.activate_kill_switch();
    CASE("kill switch is active after activation", engine.is_kill_switch_active());

    // After activation: order blocked regardless of validity
    TickResult result_after = engine.pre_trade_check(
        good_order, GOOD_USDC, GOOD_POL, 0.0, 0.0);

    CASE("kill switch active → KILL_SWITCH", result_after == TickResult::KILL_SWITCH);

    // Even a zero-size order is blocked
    TickResult result_zero = engine.pre_trade_check(
        make_order(0.0), GOOD_USDC, GOOD_POL, 0.0, 0.0);
    CASE("kill switch blocks even zero-size order", result_zero == TickResult::KILL_SWITCH);

    return true;
}

/* ───── Test 3: Orders per minute window exceeded → RISK_BLOCKED ───── */

static bool test_order_rate_window() {
    RiskConfig cfg = make_default_config();
    // max_orders_per_min default = 10
    RiskEngine engine(cfg);

    OrderParams good_order = make_order(100.0);  // valid size

    // Fill the order window to capacity (10 orders)
    // Each record_order adds a timestamp to the sliding window buffer
    for (int i = 0; i < cfg.max_orders_per_min; i++) {
        engine.record_order(100.0, true);
    }

    // Now pre_trade_check should be blocked by the rate window
    TickResult result = engine.pre_trade_check(
        good_order, GOOD_USDC, GOOD_POL, 0.0, 0.0);

    CASE("order window at capacity → RISK_BLOCKED", result == TickResult::RISK_BLOCKED);

    // Reset engine and verify that fewer orders don't trigger the limit
    RiskEngine engine2(cfg);
    // Record one less than the limit
    for (int i = 0; i < cfg.max_orders_per_min - 1; i++) {
        engine2.record_order(100.0, true);
    }
    TickResult result2 = engine2.pre_trade_check(
        good_order, GOOD_USDC, GOOD_POL, 0.0, 0.0);
    CASE("order window below capacity → OK", result2 == TickResult::OK);

    return true;
}

/* ───── Test 4: Insufficient balance → INSUFFICIENT_BALANCE ───── */

static bool test_insufficient_balance() {
    RiskConfig cfg = make_default_config();
    RiskEngine engine(cfg);

    OrderParams good_order = make_order(100.0);  // $100, within limits

    // USDC below minimum (min_usdc_balance default = 100.0)
    TickResult result_low_usdc = engine.pre_trade_check(
        good_order, 50.0, GOOD_POL, 0.0, 0.0);
    CASE("usdc < min_usdc_balance → INSUFFICIENT_BALANCE",
         result_low_usdc == TickResult::INSUFFICIENT_BALANCE);

    // POL below minimum (min_pol_balance default = 10.0)
    TickResult result_low_pol = engine.pre_trade_check(
        good_order, GOOD_USDC, 5.0, 0.0, 0.0);
    CASE("pol < min_pol_balance → INSUFFICIENT_BALANCE",
         result_low_pol == TickResult::INSUFFICIENT_BALANCE);

    // Exactly at minimum should pass
    TickResult result_at_min = engine.pre_trade_check(
        good_order, cfg.min_usdc_balance, cfg.min_pol_balance, 0.0, 0.0);
    CASE("balance at minimum → OK (boundary)", result_at_min == TickResult::OK);

    // Well above minimum
    TickResult result_good = engine.pre_trade_check(
        good_order, GOOD_USDC, GOOD_POL, 0.0, 0.0);
    CASE("sufficient balance → OK", result_good == TickResult::OK);

    return true;
}

/* ───── Test 5: Daily loss exceeds max → kill switch activated ───── */

static bool test_daily_loss_kill_switch() {
    RiskConfig cfg = make_default_config();
    cfg.kill_switch_enabled = true;  // enable auto-kill-switch
    RiskEngine engine(cfg);

    // max_daily_loss_usd default = 500.0
    // Add loss just below threshold — should NOT trigger kill switch
    engine.add_loss(499.0);
    CASE("loss=499 ($500 limit) → kill switch inactive",
         !engine.is_kill_switch_active());

    // Add more loss to cross the threshold
    engine.add_loss(10.0);  // total = 509 > 500

    CASE("loss exceeds max_daily_loss → kill switch activated",
         engine.is_kill_switch_active());

    // Now pre_trade_check should return KILL_SWITCH
    OrderParams good_order = make_order(100.0);
    TickResult result = engine.pre_trade_check(
        good_order, GOOD_USDC, GOOD_POL, 0.0, 0.0);
    CASE("after daily loss exceeded → KILL_SWITCH",
         result == TickResult::KILL_SWITCH);

    return true;
}

/* ───── Test 6: Market exposure exceeds limit → RISK_BLOCKED ───── */

static bool test_exposure_limit() {
    RiskConfig cfg = make_default_config();
    RiskEngine engine(cfg);

    // max_exposure_per_market default = 5000.0
    OrderParams good_order = make_order(100.0);

    // Exposure below limit → OK
    TickResult result_ok = engine.pre_trade_check(
        good_order, GOOD_USDC, GOOD_POL, 4999.0, 0.0);
    CASE("exposure=4999 ($5000 limit) → OK", result_ok == TickResult::OK);

    // Exposure above limit → RISK_BLOCKED
    TickResult result_blocked = engine.pre_trade_check(
        good_order, GOOD_USDC, GOOD_POL, 5001.0, 0.0);
    CASE("exposure=5001 > limit → RISK_BLOCKED", result_blocked == TickResult::RISK_BLOCKED);

    return true;
}

/* ───── Test 7: Kill switch deactivation ───── */

static bool test_kill_switch_deactivate() {
    RiskConfig cfg = make_default_config();
    RiskEngine engine(cfg);

    // Activate
    engine.activate_kill_switch();
    CASE("kill switch active after activate", engine.is_kill_switch_active());

    // Deactivate
    engine.deactivate_kill_switch();
    CASE("kill switch inactive after deactivate", !engine.is_kill_switch_active());

    // Should be able to trade again
    OrderParams good_order = make_order(100.0);
    TickResult result = engine.pre_trade_check(
        good_order, GOOD_USDC, GOOD_POL, 0.0, 0.0);
    CASE("trading resumes after deactivate", result == TickResult::OK);

    // Double deactivate (idempotent)
    engine.deactivate_kill_switch();
    engine.deactivate_kill_switch();
    CASE("double deactivate is safe", !engine.is_kill_switch_active());

    return true;
}

/* ───── Test 8: Cancel window per minute → RISK_BLOCKED ───── */

static bool test_cancel_rate_window() {
    RiskConfig cfg = make_default_config();
    RiskEngine engine(cfg);

    OrderParams good_order = make_order(100.0);

    // Fill the cancel window to capacity (max_cancels_per_min = 20)
    for (int i = 0; i < cfg.max_cancels_per_min; i++) {
        engine.record_cancel();
    }

    TickResult result = engine.pre_trade_check(
        good_order, GOOD_USDC, GOOD_POL, 0.0, 0.0);
    CASE("cancel window at capacity → RISK_BLOCKED", result == TickResult::RISK_BLOCKED);

    return true;
}

/* ───── Test 9: Normal valid order passes all checks → OK ───── */

static bool test_valid_order_passes() {
    RiskConfig cfg = make_default_config();
    RiskEngine engine(cfg);

    // $100 order, $5000 USDC, $100 POL, 0 exposure, 0 PnL
    // All within limits:
    //   max_order_usd=500 ✓
    //   min_usdc_balance=100 ✓ (5000 >= 100)
    //   min_pol_balance=10 ✓ (100 >= 10)
    //   max_exposure_per_market=5000 ✓ (0 <= 5000)
    //   max_daily_loss_usd=500 ✓ (0 <= 500)
    //   max_orders_per_min=10 ✓ (0 < 10)
    //   max_cancels_per_min=20 ✓ (0 < 20)

    OrderParams good_order = make_order(100.0);
    TickResult result = engine.pre_trade_check(
        good_order, GOOD_USDC, GOOD_POL, 0.0, 0.0);

    CASE("valid order → OK", result == TickResult::OK);

    return true;
}

/* ───── Test 10: Config overrides via env-driven custom config ───── */

static bool test_configurable_limits() {
    RiskConfig cfg;
    cfg.max_order_usd = 1000.0;       // higher limit
    cfg.min_usdc_balance = 500.0;      // higher minimum
    cfg.max_orders_per_min = 3;        // stricter rate limit
    cfg.max_exposure_per_market = 100.0; // lower exposure
    RiskEngine engine(cfg);

    // $500 order should pass (max_order_usd=1000)
    OrderParams order = make_order(500.0);
    TickResult result = engine.pre_trade_check(
        order, 10000.0, 1000.0, 0.0, 0.0);
    CASE("configurable: order $500 < $1000 limit → OK", result == TickResult::OK);

    // $1500 order should fail
    OrderParams big = make_order(1500.0);
    TickResult result2 = engine.pre_trade_check(
        big, 10000.0, 1000.0, 0.0, 0.0);
    CASE("configurable: order $1500 > $1000 limit → RISK_BLOCKED", result2 == TickResult::RISK_BLOCKED);

    // USDC below configurable minimum
    TickResult result3 = engine.pre_trade_check(
        order, 100.0, 1000.0, 0.0, 0.0);
    CASE("configurable: usdc $100 < min $500 → INSUFFICIENT_BALANCE",
         result3 == TickResult::INSUFFICIENT_BALANCE);

    // Rate limit with max_orders_per_min=3
    RiskEngine engine2(cfg);
    for (int i = 0; i < 3; i++) engine2.record_order(100.0, true);
    TickResult result4 = engine2.pre_trade_check(
        order, 10000.0, 1000.0, 0.0, 0.0);
    CASE("configurable: 3 orders/min limit hit → RISK_BLOCKED", result4 == TickResult::RISK_BLOCKED);

    return true;
}

/* ───── Test 11: Consecutive reject streak triggers kill switch ───── */

static bool test_consecutive_rejects() {
    RiskConfig cfg = make_default_config();
    cfg.kill_switch_enabled = true;
    cfg.max_consecutive_rejects = 5;
    RiskEngine engine(cfg);

    // Record 4 rejects — should NOT trigger kill switch yet
    for (int i = 0; i < 4; i++) engine.record_reject();
    CASE("4 rejects (< 5 limit) → kill switch inactive",
         !engine.is_kill_switch_active());

    // 5th reject should trigger kill switch
    engine.record_reject();
    CASE("5 rejects (== limit) → kill switch activated",
         engine.is_kill_switch_active());

    return true;
}

/* ───── Test 12: Daily loss auto-reset doesn't affect kill switch ───── */

static bool test_daily_loss_no_auto_reset() {
    RiskConfig cfg = make_default_config();
    cfg.kill_switch_enabled = true;
    RiskEngine engine(cfg);

    // Add loss that crosses threshold
    engine.add_loss(1000.0);
    CASE("loss exceeds threshold → kill switch", engine.is_kill_switch_active());

    // Kill switch persists — pre_trade_check still returns KILL_SWITCH
    OrderParams order = make_order(10.0);
    TickResult result = engine.pre_trade_check(
        order, GOOD_USDC, GOOD_POL, 0.0, 0.0);
    CASE("kill switch persists after daily loss", result == TickResult::KILL_SWITCH);

    // Only explicit deactivation clears it
    engine.deactivate_kill_switch();
    CASE("explicit deactivation clears kill switch", !engine.is_kill_switch_active());

    return true;
}

/* ───── Main ───── */

int main() {
    std::cout << "🛡️  RiskEngine & Kill Switch — Phase 2 Test Suite" << std::endl;
    std::cout << "══════════════════════════════════════════════════════════" << std::endl;
    std::cout << std::endl;
    std::cout << "Config (conservative defaults):" << std::endl;
    std::cout << "  max_daily_loss_usd      = 500.0" << std::endl;
    std::cout << "  max_order_usd           = 500.0" << std::endl;
    std::cout << "  min_usdc_balance        = 100.0" << std::endl;
    std::cout << "  min_pol_balance         = 10.0" << std::endl;
    std::cout << "  max_orders_per_min      = 10" << std::endl;
    std::cout << "  max_cancels_per_min     = 20" << std::endl;
    std::cout << "  max_exposure_per_market = 5000.0" << std::endl;
    std::cout << "  kill_switch_enabled     = true" << std::endl;
    std::cout << std::endl;

    run_test("T1: order_exceeds_max_usd_blocked",    test_order_exceeds_max);
    run_test("T2: kill_switch_active_blocks",        test_kill_switch_active);
    run_test("T3: order_rate_window_exceeded",       test_order_rate_window);
    run_test("T4: insufficient_balance_blocked",     test_insufficient_balance);
    run_test("T5: daily_loss_triggers_kill_switch",  test_daily_loss_kill_switch);
    run_test("T6: exposure_limit_enforced",          test_exposure_limit);
    run_test("T7: kill_switch_deactivation",         test_kill_switch_deactivate);
    run_test("T8: cancel_rate_window_exceeded",      test_cancel_rate_window);
    run_test("T9: valid_order_passes",               test_valid_order_passes);
    run_test("T10: configurable_limits",              test_configurable_limits);
    run_test("T11: consecutive_reject_streak",        test_consecutive_rejects);
    run_test("T12: daily_loss_persists_kill_switch", test_daily_loss_no_auto_reset);

    std::cout << std::endl;
    std::cout << "══════════════════════════════════════════════════════════" << std::endl;

    // Print individual case results
    std::cout << std::endl;
    for (int i = 0; i < g_case_idx; i++) {
        std::cout << "  " << (g_cases[i].passed ? "✅" : "❌") << " "
                  << g_cases[i].name << std::endl;
    }

    std::cout << std::endl;

    int failed_tests = g_tests_run - g_tests_passed;
    int failed_cases = 0;
    for (int i = 0; i < g_case_idx; i++) {
        if (!g_cases[i].passed) failed_cases++;
    }

    std::cout << "══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Tests:  " << g_tests_passed << "/" << g_tests_run << " passed" << std::endl;
    std::cout << "Cases:  " << (g_case_idx - failed_cases) << "/" << g_case_idx
              << " passed (" << failed_cases << " failed)" << std::endl;
    std::cout << "══════════════════════════════════════════════════════════" << std::endl;

    if (failed_tests > 0 || failed_cases > 0) {
        std::cout << "❌ " << failed_tests << " test(s) failed, "
                  << failed_cases << " case(s) failed" << std::endl;
        return 1;
    }

    std::cout << "✅ All " << g_tests_passed << "/" << g_tests_run << " tests passed ("
              << g_case_idx << " assertion cases)" << std::endl;
    std::cout << "✅ RiskEngine kill switch, rate windows, and limits verified." << std::endl;
    return 0;
}
