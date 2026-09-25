/**
 * test_neg_risk_engine.cpp — Phase C: Combinatorial Neg-Risk Arbitrage Tests
 *
 * Verifies (Fase C: Alpha Temporal):
 *   C-1: Price deviation detection (sum(P(Yes)) != 1.00)
 *   C-2: Simple Dutch book arbitrage (buy all when sum < 1.0)
 *   C-3: Reverse Dutch book arbitrage (sell all when sum > 1.0)
 *   C-4: Combinatorial enumeration of 2^N outcome combinations
 *   C-5: Profitability verification across ALL outcome combinations
 *   C-6: Fee, gas, and slippage deduction from gross profit
 *   C-7: Liquidity constraint enforcement (no oversized positions)
 *   C-8: Single-block atomic execution requirement (OrderBundle)
 *   C-9: Stress test: 1000 random market sets, verify no false positives
 *   C-10: Lock-free validation: LockFreeArbitrageResult push/pop integrity
 *
 * Mathematical verification:
 *   profit(ω) = Σ y_i · (ω_i - p_i)
 *   Arbitrage exists iff: profit(ω) > 0 for ALL ω ∈ {0,1}^N
 *
 * Dependencies: neg_risk_engine.hpp, order_book.hpp, fee_model.hpp, risk_engine.hpp
 */

#include "neg_risk_engine.hpp"
#include "order_book.hpp"
#include "fee_model.hpp"
#include "risk_engine.hpp"
#include "market_config.hpp"
#include "kelly_engine.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <random>

/* ───── Test framework (minimal, no external deps) ───── */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

struct TestCase {
    const char* name;
    bool passed;
};
static TestCase g_cases[1024];
static int g_case_idx = 0;

#define CASE(name_, cond_) do { \
    g_cases[g_case_idx].name = name_; \
    g_cases[g_case_idx].passed = (cond_); \
    g_case_idx++; \
} while(0)

static void run_test(const char* name, bool (*fn)()) {
    g_tests_run++;
    std::cout << "▶ " << name << " ..." << std::flush;
    bool ok = fn();
    if (ok) {
        g_tests_passed++;
        std::cout << " ✅ PASS" << std::endl;
    } else {
        g_tests_failed++;
        std::cout << " ❌ FAIL" << std::endl;
    }
}

/* ───── Test helpers ───── */

/**
 * Create N mutually exclusive markets with given prices.
 * Theoretical sum = 1.0.
 */
static std::vector<NegRiskMarket> make_markets(
    const std::vector<double>& prices_yes,
    double liquidity = 10000.0) {

    std::vector<NegRiskMarket> markets;
    for (size_t i = 0; i < prices_yes.size(); i++) {
        NegRiskMarket m;
        m.market_slug = "market_" + std::to_string(i);
        m.question = "Outcome " + std::to_string(i);
        m.price_yes = prices_yes[i];
        m.price_no = 1.0 - prices_yes[i];
        m.liquidity = liquidity;
        m.max_yes_size = liquidity * 0.5;
        m.max_no_size = liquidity * 0.5;
        m.is_active = true;
        m.market_id = i + 1;
        markets.push_back(m);
    }
    return markets;
}

/* ───── Test 1: Price deviation detection ───── */

static bool test_price_deviation_detection() {
    NegRiskArbitrageEngine::Config cfg;
    cfg.price_deviation_threshold = 0.02;
    NegRiskArbitrageEngine engine(cfg);

    // No deviation (sum = 1.0)
    auto no_deviation = make_markets({0.3, 0.3, 0.4});
    double dev = 0.0;
    bool has_deviation = engine.check_price_deviation(no_deviation, dev);
    CASE("no deviation (sum=1.0) → no signal", !has_deviation);
    CASE("deviation is 0.0", dev == 0.0);

    // Small deviation (sum = 1.01, just above threshold)
    auto small_dev = make_markets({0.34, 0.33, 0.34});
    dev = 0.0;
    has_deviation = engine.check_price_deviation(small_dev, dev);
    CASE("small deviation (sum=1.01, dev=0.01 < 0.02) → no signal", !has_deviation);

    // Large deviation (sum = 1.10, above threshold)
    auto large_dev = make_markets({0.40, 0.35, 0.35});
    dev = 0.0;
    has_deviation = engine.check_price_deviation(large_dev, dev);
    CASE("large deviation (sum=1.10) → signal detected", has_deviation);
    CASE("deviation value is 0.10", std::abs(dev - 0.10) < 0.001);

    // Undervalued case (sum = 0.90, below 1.0)
    auto undervalued = make_markets({0.30, 0.30, 0.30});
    dev = 0.0;
    has_deviation = engine.check_price_deviation(undervalued, dev);
    CASE("undervalued (sum=0.90) → signal detected", has_deviation);
    CASE("undervalued deviation is 0.10", std::abs(dev - 0.10) < 0.001);

    return true;
}

/* ───── Test 2: Simple Dutch book arbitrage (buy all) ───── */

static bool test_dutch_book_buy() {
    NegRiskArbitrageEngine::Config cfg;
    cfg.price_deviation_threshold = 0.01;
    cfg.min_roi = 0.001;
    cfg.min_position_size_usd = 1.0;
    cfg.max_position_size_usd = 1000.0;
    cfg.grid_step = 0.5;
    NegRiskArbitrageEngine engine(cfg);

    // sum = 0.90 (undervalued, buy all)
    auto markets = make_markets({0.30, 0.30, 0.30});

    auto opp = engine.find_arbitrage(markets);
    CASE("arbitrage opportunity found (sum<1.0)", opp.has_value());

    if (opp) {
        CASE("positions are all positive (buy Yes)",
             opp->positions[0] > 0 && opp->positions[1] > 0 && opp->positions[2] > 0);
        CASE("guaranteed profit > 0", opp->bundle.guaranteed_profit > 0);
        CASE("edge_usd > 0", opp->edge_usd > 0);
        CASE("requires atomic execution", opp->bundle.requires_atomic_execution);
        CASE("bundle has orders", opp->bundle.orders.size() == 3);
        CASE("price deviation is 0.10",
             std::abs(opp->price_deviation - 0.10) < 0.001);
    } else {
        CASE("positions are all positive", false);
        CASE("guaranteed profit > 0", false);
        CASE("edge_usd > 0", false);
        CASE("requires atomic execution", false);
        CASE("bundle has orders", false);
        CASE("price deviation is 0.10", false);
    }

    return true;
}

/* ───── Test 3: Reverse Dutch book (sell all) ───── */

static bool test_dutch_book_sell() {
    NegRiskArbitrageEngine::Config cfg;
    cfg.price_deviation_threshold = 0.01;
    cfg.min_roi = 0.001;
    cfg.min_position_size_usd = 1.0;
    cfg.max_position_size_usd = 1000.0;
    cfg.grid_step = 0.5;
    NegRiskArbitrageEngine engine(cfg);

    // sum = 1.30 (overvalued, sell all)
    auto markets = make_markets({0.45, 0.45, 0.40});

    auto opp = engine.find_arbitrage(markets);
    CASE("arbitrage opportunity found (sum>1.0)", opp.has_value());

    if (opp) {
        CASE("positions are all negative (sell Yes)",
             opp->positions[0] < 0 && opp->positions[1] < 0 && opp->positions[2] < 0);
        CASE("guaranteed profit > 0", opp->bundle.guaranteed_profit > 0);
        CASE("edge_usd > 0", opp->edge_usd > 0);
    } else {
        CASE("positions are all negative", false);
        CASE("guaranteed profit > 0", false);
        CASE("edge_usd > 0", false);
    }

    return true;
}

/* ───── Test 4: Combinatorial outcome enumeration ───── */

static bool test_combinatorial_enumeration() {
    // 3 markets — enumerate all 2^3 = 8 outcomes
    auto markets = make_markets({0.30, 0.30, 0.30});

    NegRiskArbitrageEngine engine;
    std::vector<double> positions = {100.0, 100.0, 100.0};

    auto outcomes = engine.enumerate_outcomes(markets, positions);

    CASE("2^3 = 8 outcomes enumerated", outcomes.payouts.size() == 8);
    CASE("max payout >= min payout", outcomes.max_payout >= outcomes.min_payout);
    CASE("avg payout is between min and max",
         outcomes.avg_payout >= outcomes.min_payout &&
         outcomes.avg_payout <= outcomes.max_payout);

    // Verify formula: profit(ω) = Σ y_i * (ω_i - p_i)
    // Outcome 000 (omega=0): profit = 100*(-0.3) + 100*(-0.3) + 100*(-0.3) = -90
    CASE("outcome (0,0,0) profit = -90",
         std::abs(outcomes.payouts[0] - (-90.0)) < 0.01);

    // Outcome 111 (omega=7): profit = 100*0.7 + 100*0.7 + 100*0.7 = 210
    CASE("outcome (1,1,1) profit = 210",
         std::abs(outcomes.payouts[7] - 210.0) < 0.01);

    // Outcome 100 (omega=4): profit = 100*0.7 + 100*(-0.3) + 100*(-0.3) = 10
    CASE("outcome (1,0,0) profit = 10",
         std::abs(outcomes.payouts[4] - 10.0) < 0.01);

    // 2 markets — 2^2 = 4 outcomes
    auto markets2 = make_markets({0.4, 0.6});
    auto outcomes2 = engine.enumerate_outcomes(markets2, {50.0, 50.0});
    CASE("2^2 = 4 outcomes enumerated", outcomes2.payouts.size() == 4);

    return true;
}

/* ───── Test 5: Profitability verification formula ───── */

static bool test_profitability_formula() {
    NegRiskArbitrageEngine engine;

    // 2 markets: p0=0.45, p1=0.45
    auto markets = make_markets({0.45, 0.45});
    std::vector<double> positions = {100.0, -50.0};  // Buy Yes on 0, sell Yes on 1

    auto outcomes = engine.enumerate_outcomes(markets, positions);

    // Outcome (0,0): 100*(-0.45) + (-50)*(-0.45) = -45 + 22.5 = -22.5
    CASE("formula: outcome (0,0) = -22.5",
         std::abs(outcomes.payouts[0] - (-22.5)) < 0.01);

    // Outcome (1,0): ω0=1, ω1=0 → 100*(0.55) + (-50)*(-0.45) = 55 + 22.5 = 77.5
    CASE("formula: outcome (1,0) = 77.5",
         std::abs(outcomes.payouts[1] - 77.5) < 0.01);

    // Outcome (0,1): ω0=0, ω1=1 → 100*(-0.45) + (-50)*(0.55) = -45 - 27.5 = -72.5
    CASE("formula: outcome (0,1) = -72.5",
         std::abs(outcomes.payouts[2] - (-72.5)) < 0.01);

    // Outcome (1,1): 100*(0.55) + (-50)*(0.55) = 55 - 27.5 = 27.5
    CASE("formula: outcome (1,1) = 27.5",
         std::abs(outcomes.payouts[3] - 27.5) < 0.01);

    return true;
}

/* ───── Test 6: Fee and gas deduction ───── */

static bool test_fee_gas_deduction() {
    RiskConfig risk_cfg;
    risk_cfg.gas_cost_usd = 0.005;
    risk_cfg.maker_fee_rate = 0.020;
    risk_cfg.base_commission_usd = 0.10;

    NegRiskArbitrageEngine::Config cfg;
    cfg.price_deviation_threshold = 0.01;
    cfg.min_roi = 0.001;
    cfg.min_position_size_usd = 1.0;
    cfg.max_position_size_usd = 100.0;
    cfg.grid_step = 0.5;

    NegRiskArbitrageEngine engine(cfg, risk_cfg);

    // Large deviation to ensure profitability after fees
    auto markets = make_markets({0.20, 0.20, 0.20});  // sum = 0.60

    auto opp = engine.find_arbitrage(markets);
    CASE("arbitrage found with fee deduction", opp.has_value());

    if (opp) {
        CASE("net_profit_after_fees > 0", opp->bundle.net_profit_after_fees > 0);
        CASE("estimated_gas_usd > 0", opp->bundle.estimated_gas_usd > 0);
        CASE("net_profit >= guaranteed (no double fee)",
             opp->bundle.net_profit_after_fees >= opp->bundle.guaranteed_profit - 1.0);

        FeeModel fee_model(risk_cfg);
        double test_fee = fee_model.compute_fee(100.0, true, 0.20);
        CASE("fee model produces positive fee", test_fee > 0);
    }

    return true;
}

/* ───── Test 7: Liquidity constraint enforcement ───── */

static bool test_liquidity_constraints() {
    NegRiskArbitrageEngine::Config cfg;
    cfg.price_deviation_threshold = 0.01;
    cfg.min_roi = 0.001;
    cfg.min_position_size_usd = 1.0;
    cfg.max_position_size_usd = 100.0;
    cfg.grid_step = 0.5;
    NegRiskArbitrageEngine engine(cfg);

    // Markets with limited liquidity
    auto markets = make_markets({0.20, 0.20, 0.20}, 50.0);

    auto opp = engine.find_arbitrage(markets);

    if (opp) {
        for (size_t i = 0; i < markets.size(); i++) {
            CASE("position <= liquidity (market 0)",
                 std::abs(opp->positions[i]) <= 50.0 + 1e-6);
        }
    } else {
        CASE("low liquidity correctly prevents arbitrage", true);
    }

    return true;
}

/* ───── Test 8: OrderBundle atomicity and structure ───── */

static bool test_order_bundle_structure() {
    NegRiskArbitrageEngine::Config cfg;
    cfg.price_deviation_threshold = 0.01;
    cfg.min_roi = 0.001;
    cfg.min_position_size_usd = 1.0;
    cfg.max_position_size_usd = 1000.0;
    cfg.grid_step = 0.5;
    NegRiskArbitrageEngine engine(cfg);

    auto markets = make_markets({0.20, 0.20, 0.20});
    auto opp = engine.find_arbitrage(markets);

    if (opp) {
        CASE("bundle requires atomic execution", opp->bundle.requires_atomic_execution);
        CASE("bundle has unique ID", !opp->bundle.bundle_id.empty());
        CASE("bundle ID contains prefix",
             opp->bundle.bundle_id.find("negrisk_arb_") != std::string::npos);
        CASE("bundle has 3 orders", opp->bundle.orders.size() == 3);

        for (const auto& order : opp->bundle.orders) {
            CASE("order market_slug not empty", !order.market_slug.empty());
            CASE("order nonce > 0", order.nonce > 0);
            CASE("order salt > 0", order.salt > 0);
            CASE("order price > 0", order.price > 0.0);
            CASE("order amount > 0", order.amount > 0);
        }

        CASE("required capital >= 0", opp->bundle.required_capital >= 0.0);
        CASE("expected_roi > 0", opp->bundle.expected_roi > 0);
    } else {
        CASE("bundle requires atomic execution", false);
        CASE("bundle has unique ID", false);
        CASE("bundle has 3 orders", false);
    }

    return true;
}

/* ───── Test 9: Stress test — 1000 random market sets ───── */

static bool test_stress_random_markets() {
    NegRiskArbitrageEngine::Config cfg;
    cfg.price_deviation_threshold = 0.05;
    cfg.min_roi = 0.1;
    cfg.min_position_size_usd = 100.0;
    cfg.max_position_size_usd = 5000.0;
    NegRiskArbitrageEngine engine(cfg);

    std::mt19937 rng(0xDEADBEEF);
    std::uniform_real_distribution<double> price_dist(0.1, 0.9);

    int opportunities_found = 0;
    int false_positives = 0;

    for (int i = 0; i < 1000; i++) {
        int n = 2 + (i % 3);
        std::vector<double> prices;
        for (int j = 0; j < n; j++) {
            prices.push_back(price_dist(rng));
        }
        auto markets = make_markets(prices, 10000.0);

        auto opp = engine.find_arbitrage(markets);

        if (opp.has_value()) {
            opportunities_found++;
            if (!engine.verify_arbitrage(markets, opp->positions)) {
                false_positives++;
            }
        }
    }

    CASE("stress: processed 1000 market sets", true);
    CASE("stress: no false positives from verification", false_positives == 0);

    return true;
}

/* ───── Test 10: Lock-free ArbitrageResult push/pop ───── */

static bool test_lockfree_arbitrage_result() {
    const int ITERATIONS = 10000;

    LockFreeArbitrageResult queue;
    std::atomic<uint64_t> pushed{0};
    std::atomic<uint64_t> popped{0};
    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
        for (int i = 0; i < ITERATIONS; i++) {
            ArbitrageOpportunity opp;
            opp.edge_usd = 100.0 + i;
            opp.sum_prices = 0.95 + (i % 5) * 0.01;
            opp.price_deviation = 0.05;
            opp.confidence = 0.8;
            opp.detection_timestamp_ns = i;

            while (!queue.try_push(opp)) {
                std::this_thread::yield();
            }
            pushed.fetch_add(1, std::memory_order_relaxed);
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        uint64_t expected_ts = 0;
        bool seq_ok = true;
        while (!producer_done.load(std::memory_order_acquire)) {
            auto result = queue.try_pop();
            if (result) {
                if (result->detection_timestamp_ns != expected_ts) seq_ok = false;
                expected_ts++;
                popped.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
        while (auto result = queue.try_pop()) {
            if (result->detection_timestamp_ns != expected_ts) seq_ok = false;
            expected_ts++;
            popped.fetch_add(1, std::memory_order_relaxed);
        }
        CASE("lockfree arbitrage: timestamp sequence preserved", seq_ok);
    });

    producer.join();
    consumer.join();

    CASE("lockfree arbitrage: pushed == 10000", pushed.load() == ITERATIONS);
    CASE("lockfree arbitrage: popped == 10000 (no lost items)", popped.load() == ITERATIONS);
    CASE("lockfree arbitrage: queue empty after drain", queue.empty());

    return true;
}

/* ───── Test 11: Theoretical sum computation ───── */

static bool test_theoretical_sum() {
    CASE("1 market: theoretical = 1.0",
         NegRiskArbitrageEngine::compute_theoretical_sum(1) == 1.0);
    CASE("2 markets: theoretical = 1.0",
         NegRiskArbitrageEngine::compute_theoretical_sum(2) == 1.0);
    CASE("3 markets: theoretical = 1.0",
         NegRiskArbitrageEngine::compute_theoretical_sum(3) == 1.0);
    CASE("5 markets: theoretical = 1.0",
         NegRiskArbitrageEngine::compute_theoretical_sum(5) == 1.0);

    return true;
}

/* ───── Test 12: Market validation ───── */

static bool test_market_validation() {
    NegRiskArbitrageEngine engine;

    auto single = make_markets({0.5});
    CASE("single market rejected", engine.find_arbitrage(single) == std::nullopt);

    auto inactive = make_markets({0.3, 0.7});
    inactive[0].is_active = false;
    CASE("inactive market rejected", engine.find_arbitrage(inactive) == std::nullopt);

    auto invalid_price = make_markets({0.0, 1.0});
    CASE("invalid price rejected", engine.find_arbitrage(invalid_price) == std::nullopt);

    auto low_liq = make_markets({0.3, 0.7}, 1.0);
    CASE("low liquidity rejected", engine.find_arbitrage(low_liq) == std::nullopt);

    return true;
}

/* ───── Main ───── */

int main() {
    std::cout << "🧮 NegRiskArbitrageEngine — Phase C Test Suite" << std::endl;
    std::cout << "══════════════════════════════════════════════════════════" << std::endl;
    std::cout << std::endl;

    run_test("C-1: price_deviation_detection",    test_price_deviation_detection);
    run_test("C-2: dutch_book_buy",                test_dutch_book_buy);
    run_test("C-3: dutch_book_sell",               test_dutch_book_sell);
    run_test("C-4: combinatorial_enumeration",     test_combinatorial_enumeration);
    run_test("C-5: profitability_formula",         test_profitability_formula);
    run_test("C-6: fee_gas_deduction",             test_fee_gas_deduction);
    run_test("C-7: liquidity_constraints",         test_liquidity_constraints);
    run_test("C-8: order_bundle_structure",        test_order_bundle_structure);
    run_test("C-9: stress_random_markets_1000",    test_stress_random_markets);
    run_test("C-10: lockfree_arbitrage_result",    test_lockfree_arbitrage_result);
    run_test("C-11: theoretical_sum",              test_theoretical_sum);
    run_test("C-12: market_validation",            test_market_validation);

    std::cout << std::endl;
    std::cout << "══════════════════════════════════════════════════════════" << std::endl;

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
    std::cout << "✅ NegRiskArbitrageEngine, combinatorial enumeration, and lock-free validation verified." << std::endl;

    return 0;
}
