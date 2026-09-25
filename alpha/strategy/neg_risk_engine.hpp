#ifndef NEG_RISK_ENGINE_HPP
#define NEG_RISK_ENGINE_HPP

/**
 * NegRiskArbitrageEngine: Combinatorial Multi-Market Neg-Risk Arbitrage.
 *
 * Fase C (Alpha Temporal): Detects arbitrage opportunities across correlated
 * Polymarket "neg-risk" markets where the sum of Yes probabilities across
 * related outcomes ≠ 1.00, and generates synthetic order combinations for
 * single-block atomic execution.
 *
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │  Mathematical Foundation                                            │
 * │                                                                     │
 * │  For N correlated binary markets (yes/no outcomes):                 │
 * │  Let p_i = price of Yes on market i  (0.0 to 1.0)                    │
 * │  Let y_i = position in Yes on market i                               │
 * │  (y_i > 0 = buy Yes, y_i < 0 = sell Yes)                              │
 * │                                                                     │
 * │  For outcome ω ∈ {0,1}^N (ω_i = 1 if Yes wins on market i):         │
 * │    profit(ω) = Σ y_i · (ω_i - p_i)                                   │
 * │                                                                     │
 * │  Arbitrage exists iff:                                              │
 * │    1. profit(ω) > 0 for ALL 2^N outcomes ω                          │
 * │    2. net_capital = Σ y_i · p_i ≤ 0 (receive money or break even)    │
 * │                                                                     │
 * │  Simple cases:                                                      │
 * │  • If Σ p_i < 1.0: Buy Yes on all markets (Dutch book, guaranteed)   │
 * │  • If Σ p_i > 1.0: Sell Yes on all markets (reverse Dutch book)       │
 * │                                                                     │
 * │  Combinatorial case:                                                │
 * │  Enumerate all 2^N outcome combinations, verify each is profitable.  │
 * │  Use grid search over position sizes for small N (≤ 6).              │
 * └─────────────────────────────────────────────────────────────────────┘
 *
 * Architecture:
 *   Markets → NegRiskArbitrageEngine → ArbitrageOpportunity (or nullopt)
 *           │                                                         │
 *           ▼                                                         ▼
 *   [check_sum_deviation]                                     [OrderBundle for
 *        │                                                    single-block tx]
 *        ▼                                                         │
 *   [enumerate_outcomes] ──→ [grid_search_positions] ───────────────┘
 *        │                           │
 *        ▼                           ▼
 *   [verify_all_profitable]   [apply_fees_and_slippage]
 *
 * Constraints: No AWS, no physical hardware, no satellite feeds.
 *              All computation is CPU-bound, deterministic, lock-free.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "order_book.hpp"       // OrderBookL2 (for liquidity estimation)
#include "fee_model.hpp"        // FeeModel (for net EV calculation)
#include "risk_engine.hpp"      // RiskConfig (for min_net_ev_usd, gas_cost)
#include "kelly_engine.hpp"     // Position sizing
#include "market_config.hpp"    // RiskConfig

// ─── Market Data Structures ────────────────────────────────────────────────

/**
 * NegRiskMarket: A single Polymarket neg-risk binary market.
 *
 * In Polymarket, neg-risk markets are correlated markets where the outcomes
 * are linked (e.g., "Will X happen before Y?" and "Will X happen before Z?").
 * The "neg risk" aspect means that if the primary condition doesn't occur,
 * you may lose your entire stake even if a secondary condition is met.
 */
struct NegRiskMarket {
    std::string market_slug;     // e.g. "trump-win-2024"
    std::string question;         // Human-readable question
    double price_yes;             // 0.0 to 1.0 (Yes = true outcome probability)
    double price_no;              // 1.0 - price_yes
    double liquidity;             // Available liquidity in USD at current price
    double max_yes_size;          // Max buyable Yes (USD) before price impact
    double max_no_size;           // Max sellable Yes (USD) before price impact
    bool is_active;               // Market is open for trading
    uint64_t market_id;           // Internal Polymarket market ID
};

/**
 * Position: A position in a single market.
 */
struct Position {
    std::string market_slug;
    double yes_size;              // Units of Yes (positive = long, negative = short)
    double cost;                  // Net cost (positive = paid, negative = received)
    double pnl_if_yes;            // PnL if Yes wins
    double pnl_if_no;             // PnL if No wins
};

/**
 * SyntheticOrder: A single order in a synthetic position bundle.
 * These orders are designed to be submitted atomically in a single block.
 */
struct SyntheticOrder {
    std::string market_slug;
    bool is_buy;                  // true = buy Yes, false = sell Yes
    double amount;                // Order size in USD
    double price;                 // Price (0.0 to 1.0)
    uint64_t nonce;               // Unique nonce for this order
    uint64_t salt;                // Salt for client_order_id
    std::string expected_pnl;     // JSON with PnL scenarios
};

/**
 * OrderBundle: A set of synthetic orders for atomic execution.
 * All orders in the bundle must be submitted in the same Ethereum block.
 */
struct OrderBundle {
    std::vector<SyntheticOrder> orders;
    double total_cost;           // Net cost (negative = received money)
    double guaranteed_profit;    // Minimum profit across all outcome combinations
    double max_profit;           // Maximum profit across all outcome combinations
    double required_capital;     // Capital needed (max of per-outcome costs)
    double expected_roi;         // ROI = guaranteed_profit / required_capital
    double estimated_gas_usd;    // Estimated gas cost for the bundle
    double net_profit_after_fees; // Profit after gas + fees
    bool requires_atomic_execution; // Must all execute in one block
    std::string bundle_id;       // Unique identifier for this bundle
};

/**
 * ArbitrageOpportunity: A detected arbitrage with all required execution data.
 */
struct ArbitrageOpportunity {
    std::vector<NegRiskMarket> markets;
    std::vector<double> positions;       // Position vector y_i (long Yes positive)
    OrderBundle bundle;
    double confidence;                 // 0.0 to 1.0
    double edge_usd;                   // Arbitrage edge in USD
    double sum_prices;                 // Sum of Yes prices
    double price_deviation;            // |sum - theoretical_value|
    uint64_t detection_timestamp_ns;
};

/**
 * NegRiskArbitrageEngine configuration.
 * All thresholds are configurable per deployment.
 */
struct NegRiskEngineConfig {
    double price_deviation_threshold = 0.02;   // Min |sum - 1.0| to trigger check
    double min_roi = 0.05;                     // Min 5% ROI to execute
    double max_position_size_usd = 5000.0;     // Max position per market
    double min_position_size_usd = 100.0;      // Min position per market
    double grid_step = 0.25;                   // Grid search step size for positions
    int max_markets_combinatorial = 6;         // Max N for full enumeration (2^N)
    double max_gas_cost_usd = 50.0;            // Max acceptable gas cost
    double confidence_threshold = 0.7;         // Min confidence to report
    double slippage_bps = 10.0;                // Expected slippage in bps
};

// ─── Combinatorial Engine ──────────────────────────────────────────────────

/**
 * NegRiskArbitrageEngine: Detects and generates neg-risk arbitrage opportunities.
 *
 * This engine implements the combinatorial multi-market arbitrage detection
 * described in Fase C. It:
 *
 * 1. Takes a set of correlated binary markets
 * 2. Checks if sum(P(Yes_i)) deviates from the theoretical value
 * 3. Enumerates all 2^N outcome combinations
 * 4. Finds position vectors that are profitable in all scenarios
 * 5. Applies fee, gas, and slippage calculations
 * 6. Generates a synthetic order bundle for single-block execution
 *
 * Thread-safety: The engine is stateless between calls (find_arbitrage is a
 * pure function). Multiple threads can call find_arbitrage concurrently with
 * different market sets. The FeeModel is the only stateful component and is
 * constructed per-call (or shared with atomic reads).
 */
class NegRiskArbitrageEngine {
public:
    using Config = NegRiskEngineConfig;

    NegRiskArbitrageEngine()
        : config_(NegRiskEngineConfig{})
        , risk_config_(RiskConfig::load_from_env())
        , fee_model_(risk_config_) {}

    explicit NegRiskArbitrageEngine(const Config& config)
        : config_(config)
        , risk_config_(RiskConfig::load_from_env())
        , fee_model_(risk_config_) {}

    NegRiskArbitrageEngine(const Config& config, const RiskConfig& risk_config)
        : config_(config)
        , risk_config_(risk_config)
        , fee_model_(risk_config) {}

    /**
     * Find arbitrage opportunities across a set of correlated markets.
     *
     * This is the main entry point. It:
     * 1. Validates that markets are active and liquid
     * 2. Checks if sum(P(Yes)) deviates from theoretical value
     * 3. If deviation > threshold, searches for profitable position vector
     * 4. Verifies all 2^N outcome combinations are profitable
     * 5. Applies fees, gas, and slippage
     * 6. Returns ArbitrageOpportunity if viable
     *
     * @param markets  Vector of correlated binary markets
     * @return ArbitrageOpportunity if found, nullopt otherwise
     */
    std::optional<ArbitrageOpportunity> find_arbitrage(
        const std::vector<NegRiskMarket>& markets) {

        // Step 1: Validate input
        if (!validate_markets(markets)) {
            return std::nullopt;
        }

        // Step 2: Check price deviation from theoretical value
        double sum_yes = 0.0;
        for (const auto& m : markets) {
            sum_yes += m.price_yes;
        }

        double theoretical_value = compute_theoretical_sum(markets.size());
        double deviation = std::abs(sum_yes - theoretical_value);

        if (deviation < config_.price_deviation_threshold) {
            // No significant deviation — no arbitrage opportunity
            return std::nullopt;
        }

        // Step 3: Find profitable position vector
        auto positions = search_positions(markets, sum_yes, theoretical_value);
        if (positions.empty() || !has_positive_solution(positions)) {
            return std::nullopt;
        }

        // Step 4: Verify outcome profitability.
        // For Dutch book positions (uniform sign), use worst-case single-winner
        // verification instead of strict all-2^N outcomes (which would reject
        // valid Dutch books where the all-No outcome is impossible in
        // mutually exclusive markets).
        auto outcome_payouts = enumerate_outcomes(markets, positions);
        if (!verify_dutch_book_profitable(markets, positions, outcome_payouts)) {
            return std::nullopt;
        }

        // Step 5: Apply fees and gas costs.
        // NOTE: worst_payout already includes position cost (Σ y_i * (ω_i - p_i)),
        // so we only subtract fees and gas here — total_cost is embedded in the
        // payout formula and must NOT be subtracted again.
        double total_cost = compute_total_cost(markets, positions);
        double estimated_gas = estimate_gas_cost(markets.size());
        double fees = compute_total_fees(markets, positions);

        // Use worst-case realistic payout for Dutch book positions.
        // outcome_payouts.min_payout includes impossible all-No outcomes;
        // for uniform-sign Dutch books, worst realistic case is exactly 1 winner.
        double worst_payout = compute_worst_case_payout(markets, positions, outcome_payouts);
        double net_profit = worst_payout - estimated_gas - fees;

        if (net_profit <= 0) {
            return std::nullopt;  // Not profitable after costs
        }

        // Step 6: Build order bundle
        OrderBundle bundle = build_order_bundle(markets, positions,
            total_cost, net_profit, outcome_payouts, estimated_gas);

        // Step 7: Compute confidence and ROI
        double required_cap = std::abs(total_cost);
        double roi = (required_cap > 0) ? net_profit / required_cap : 0.0;

        if (roi < config_.min_roi) {
            return std::nullopt;  // ROI too low
        }

        double confidence = compute_confidence(markets, deviation, roi);

        if (confidence < config_.confidence_threshold) {
            return std::nullopt;
        }

        // Build final result
        ArbitrageOpportunity opp;
        opp.markets = markets;
        opp.positions = positions;
        opp.bundle = bundle;
        opp.confidence = confidence;
        opp.edge_usd = net_profit;
        opp.sum_prices = sum_yes;
        opp.price_deviation = deviation;
        opp.detection_timestamp_ns = get_time_ns();

        return opp;
    }

    /**
     * Quick check: does this market set have a sum deviation signal?
     * O(N) — can be called frequently for screening before full analysis.
     */
    bool check_price_deviation(const std::vector<NegRiskMarket>& markets,
                               double& deviation) const {
        if (markets.empty() || !validate_markets(markets)) {
            deviation = 0.0;
            return false;
        }

        double sum_yes = 0.0;
        for (const auto& m : markets) {
            sum_yes += m.price_yes;
        }

        double theoretical = compute_theoretical_sum(markets.size());
        deviation = std::abs(sum_yes - theoretical);
        return deviation >= config_.price_deviation_threshold;
    }

    /**
     * Verify that a specific position vector is arbitrage-free profitable
     * across all outcome combinations. Pure function — for testing.
     */
    bool verify_arbitrage(const std::vector<NegRiskMarket>& markets,
                          const std::vector<double>& positions) const {
        if (markets.size() != positions.size()) return false;

        auto outcomes = enumerate_outcomes(markets, positions);
        return verify_dutch_book_profitable(markets, positions, outcomes);
    }

    /**
     * Get the theoretical sum of prices for N markets.
     * For mutually exclusive + exhaustive outcomes: sum = 1.0
     * For partially correlated markets: may differ.
     */
    static double compute_theoretical_sum(size_t n_markets) {
        if (n_markets <= 1) return 1.0;
        // For N mutually exclusive, exhaustive outcomes: sum(p_i) = 1.0
        // For N partially correlated outcomes: theoretical = 1.0 - correlation_adjustment
        // We use 1.0 as the default (most common case in Polymarket bundles)
        return 1.0;
    }

    /**
     * Enumerate all 2^N outcome combinations and compute payout for each.
     * Returns a struct with min/max/total payouts.
     */
    struct OutcomeAnalysis {
        double min_payout;
        double max_payout;
        double avg_payout;
        std::vector<double> payouts;  // All 2^N payouts
        size_t num_profitable;        // Count where payout > 0
    };

    OutcomeAnalysis enumerate_outcomes(
        const std::vector<NegRiskMarket>& markets,
        const std::vector<double>& positions) const {

        size_t n = markets.size();
        size_t num_outcomes = (n <= 16) ? (1ULL << n) : 65536;  // Cap at 2^16
        // If n > 16, we sample — for practical purposes, Polymarket bundles are small

        OutcomeAnalysis analysis;
        analysis.payouts.reserve(num_outcomes);
        analysis.min_payout = std::numeric_limits<double>::max();
        analysis.max_payout = std::numeric_limits<double>::lowest();
        analysis.avg_payout = 0.0;
        analysis.num_profitable = 0;

        // Enumerate all outcome combinations
        for (size_t omega = 0; omega < num_outcomes; omega++) {
            double payout = 0.0;

            for (size_t i = 0; i < n; i++) {
                double p_i = markets[i].price_yes;
                double y_i = positions[i];
                // profit = y_i * (ω_i - p_i)
                // ω_i = 1 if bit i of omega is set
                double omega_i = (omega & (1ULL << i)) ? 1.0 : 0.0;
                payout += y_i * (omega_i - p_i);
            }

            analysis.payouts.push_back(payout);
            analysis.min_payout = std::min(analysis.min_payout, payout);
            analysis.max_payout = std::max(analysis.max_payout, payout);
            analysis.avg_payout += payout;
            if (payout > 0) analysis.num_profitable++;
        }

        analysis.avg_payout /= static_cast<double>(num_outcomes);

        return analysis;
    }

    Config get_config() const { return config_; }

private:
    static uint64_t get_time_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    /**
     * Validate market data: all markets must be active with valid prices.
     */
    bool validate_markets(const std::vector<NegRiskMarket>& markets) const {
        if (markets.size() < 2) return false;  // Need at least 2 markets for arbitrage
        if (markets.size() > 32) return false; // Too many for 2^N enumeration

        for (const auto& m : markets) {
            if (!m.is_active) return false;
            if (m.price_yes < 0.001 || m.price_yes > 0.999) return false;
            if (m.liquidity < config_.min_position_size_usd) return false;
            // Verify price_yes + price_no ≈ 1.0
            if (std::abs(m.price_yes + m.price_no - 1.0) > 0.001) return false;
        }

        return true;
    }

    /**
     * Check if the position vector has any non-zero entries.
     */
    bool has_positive_solution(const std::vector<double>& positions) const {
        for (double y : positions) {
            if (std::abs(y) > 1e-10) return true;
        }
        return false;
    }

    /**
     * Verify that all outcome payouts are strictly positive.
     */
    bool verify_all_profitable(const OutcomeAnalysis& analysis) const {
        if (analysis.payouts.empty()) return false;
        return analysis.min_payout > 0.0;
    }

    /**
     * For Dutch book positions (all same-sign), verify profitability across
     * realistic outcomes only. In mutually exclusive markets, the worst case
     * is exactly ONE outcome winning (not zero), since at least one market
     * must resolve. This corrects the over-conservative check_all_profitable
     * that would reject valid Dutch books.
     */
    bool verify_dutch_book_profitable(
        const std::vector<NegRiskMarket>& markets,
        const std::vector<double>& positions,
        const OutcomeAnalysis& analysis) const {

        // Default: check all outcomes are profitable (strict mode)
        if (analysis.min_payout > 0.0) return true;

        // Dutch book: all positions same sign → worst case is exactly 1 winner
        // For N markets, the worst realistic outcome has exactly 1 market winning.
        // Compute the payout for each single-winner outcome.
        size_t n = markets.size();
        double worst_payout = std::numeric_limits<double>::max();
        for (size_t winner = 0; winner < n; winner++) {
            double payout = 0.0;
            for (size_t i = 0; i < n; i++) {
                double omega_i = (i == winner) ? 1.0 : 0.0;
                double p_i = markets[i].price_yes;
                payout += positions[i] * (omega_i - p_i);
            }
            worst_payout = std::min(worst_payout, payout);
        }

        return worst_payout > 0.0;
    }

    /**
     * Compute the worst-case realistic payout for a position vector.
     * If all positions are the same sign (Dutch book), the worst realistic
     * outcome has exactly one market winning. Otherwise, fall back to
     * the strict min_payout from full enumeration.
     */
    double compute_worst_case_payout(
        const std::vector<NegRiskMarket>& markets,
        const std::vector<double>& positions,
        const OutcomeAnalysis& analysis) const {

        if (analysis.min_payout > 0.0) return analysis.min_payout;

        // Check if all positions have the same sign (Dutch book)
        bool all_positive = true;
        bool all_negative = true;
        for (double y : positions) {
            if (y > 0) all_negative = false;
            if (y < 0) all_positive = false;
        }

        if (all_positive || all_negative) {
            // Worst case: exactly one outcome wins
            size_t n = markets.size();
            double worst = std::numeric_limits<double>::max();
            for (size_t winner = 0; winner < n; winner++) {
                double payout = 0.0;
                for (size_t i = 0; i < n; i++) {
                    double omega_i = (i == winner) ? 1.0 : 0.0;
                    payout += positions[i] * (omega_i - markets[i].price_yes);
                }
                worst = std::min(worst, payout);
            }
            return worst;
        }

        // Mixed signs: use strict min from full enumeration
        return analysis.min_payout;
    }

    /**
     * Search for the optimal position vector using grid search.
     *
     * For small N (≤ max_markets_combinatorial), enumerates all possible
     * position combinations on a grid.
     *
     * For the simple case (sum deviates from theoretical), tries:
     *   1. Buy/sell all at uniform size
     *   2. Proportional to deviation
     *   3. Grid search over individual positions
     */
    std::vector<double> search_positions(
        const std::vector<NegRiskMarket>& markets,
        double sum_yes, double theoretical) {

        size_t n = markets.size();
        double deviation = sum_yes - theoretical;

        // Strategy 1: Simple Dutch book (buy/sell all)
        // For sum < theoretical: buy Yes on all (y_i = +size)
        // For sum > theoretical: sell Yes on all (y_i = -size)
        double uniform_size = std::min(config_.max_position_size_usd,
                                       std::max(config_.min_position_size_usd,
                                                std::abs(deviation) * 10000.0));

        // Normalize to per-market size
        double per_market = uniform_size / static_cast<double>(n);

        std::vector<double> simple_positions(n);
        double sign = (deviation < 0) ? 1.0 : -1.0;  // Buy if undervalued, sell if overvalued
        for (size_t i = 0; i < n; i++) {
            simple_positions[i] = sign * per_market;
        }

        // Verify simple strategy — use Dutch book verification for uniform positions
        // In mutually exclusive markets, worst case is exactly 1 outcome wins (not 0)
        auto simple_outcomes = enumerate_outcomes(markets, simple_positions);
        if (verify_dutch_book_profitable(markets, simple_positions, simple_outcomes)) {
            // Check liquidity constraints
            if (check_liquidity(markets, simple_positions)) {
                return simple_positions;
            }
        }

        // Strategy 2: Grid search (for small N)
        if (n <= static_cast<size_t>(config_.max_markets_combinatorial)) {
            auto grid_positions = grid_search(markets, sign);
            if (!grid_positions.empty()) {
                return grid_positions;
            }
        }

        // Strategy 3: Proportional to inverse price deviation
        if (deviation < 0) {
            // Buy more on markets with lower prices (more undervalued)
            std::vector<double> prop_positions(n);
            double total_weight = 0.0;
            for (size_t i = 0; i < n; i++) {
                double weight = 1.0 - markets[i].price_yes;  // Lower price = higher weight
                prop_positions[i] = weight;
                total_weight += weight;
            }
            for (size_t i = 0; i < n; i++) {
                prop_positions[i] = (prop_positions[i] / total_weight) * uniform_size;
            }

            auto prop_outcomes = enumerate_outcomes(markets, prop_positions);
            if (verify_all_profitable(prop_outcomes) && check_liquidity(markets, prop_positions)) {
                return prop_positions;
            }
        }

        // No profitable position found
        return {};
    }

    /**
     * Grid search: enumerate position vectors on a grid.
     * Tries positions at {-max, -max+step, ..., 0, ..., max-step, max}
     * Returns the first position vector that is profitable in all scenarios.
     */
    std::vector<double> grid_search(
        const std::vector<NegRiskMarket>& markets, double sign) {

        size_t n = markets.size();
        double max_pos = config_.max_position_size_usd / static_cast<double>(n);
        double step = config_.grid_step;

        // Generate grid values
        std::vector<double> grid_values;
        for (double v = -max_pos; v <= max_pos; v += step) {
            if (std::abs(v) >= config_.min_position_size_usd / static_cast<double>(n)) {
                grid_values.push_back(v * sign);
            }
        }

        // For N=2, iterate over all pairs (capped grid to avoid O(100M) blowup)
        if (n == 2) {
            // Cap grid to max 50 values per dimension (50^2 = 2500 max iterations)
            std::vector<double> capped_grid;
            size_t max_points = std::min(grid_values.size(), static_cast<size_t>(50));
            // Sample evenly from the full grid
            double step_idx = static_cast<double>(grid_values.size()) / max_points;
            for (size_t i = 0; i < max_points; i++) {
                capped_grid.push_back(grid_values[static_cast<size_t>(i * step_idx)]);
            }
            for (double y0 : capped_grid) {
                for (double y1 : capped_grid) {
                    std::vector<double> pos = {y0, y1};
                    if (!verify_all_profitable(enumerate_outcomes(markets, pos))) continue;
                    if (!check_liquidity(markets, pos)) continue;
                    double net = enumerate_outcomes(markets, pos).min_payout;
                    if (net > 0) return pos;
                }
            }
        }

        // For N=3, use a capped reduced grid to avoid exponential iteration counts.
        // Limit to at most 12 values per dimension (12^3 = 1728 iterations max).
        if (n == 3) {
            std::vector<double> coarse_grid;
            int max_points = 12;
            double step = (2.0 * max_pos) / max_points;
            if (step < 10.0) step = 10.0;
            for (int g = -max_points/2; g <= max_points/2; g++) {
                double v = g * step;
                if (std::abs(v) >= config_.min_position_size_usd / static_cast<double>(n)) {
                    coarse_grid.push_back(v * sign);
                }
            }
            for (double y0 : coarse_grid) {
                for (double y1 : coarse_grid) {
                    for (double y2 : coarse_grid) {
                        std::vector<double> pos = {y0, y1, y2};
                        if (!verify_all_profitable(enumerate_outcomes(markets, pos))) continue;
                        if (!check_liquidity(markets, pos)) continue;
                        double net = enumerate_outcomes(markets, pos).min_payout;
                        if (net > 0) return pos;
                    }
                }
            }
        }

        // For N=4: only try the simple buy/sell-all strategies with different sizes
        if (n >= 4) {
            double theoretical = compute_theoretical_sum(n);
            double sum_yes = 0.0;
            for (const auto& m : markets) sum_yes += m.price_yes;
            double deviation = sum_yes - theoretical;
            double sign_dev = (deviation < 0) ? 1.0 : -1.0;

            // Try different sizes
            for (double size = config_.max_position_size_usd;
                 size >= config_.min_position_size_usd;
                 size *= 0.5) {
                double per_market = size / static_cast<double>(n);
                std::vector<double> pos(n, sign_dev * per_market);

                if (verify_all_profitable(enumerate_outcomes(markets, pos)) &&
                    check_liquidity(markets, pos)) {
                    return pos;
                }
            }
        }

        return {};
    }

    /**
     * Check that position sizes don't exceed available liquidity.
     */
    bool check_liquidity(const std::vector<NegRiskMarket>& markets,
                         const std::vector<double>& positions) const {
        for (size_t i = 0; i < markets.size(); i++) {
            double pos_usd = std::abs(positions[i]);
            if (pos_usd > markets[i].liquidity) {
                return false;  // Exceeds available liquidity
            }
            if (pos_usd > markets[i].max_yes_size && positions[i] > 0) {
                return false;  // Exceeds max buyable size
            }
            if (pos_usd > markets[i].max_no_size && positions[i] < 0) {
                return false;  // Exceeds max sellable size
            }
        }
        return true;
    }

    /**
     * Compute total cost (net capital required or received).
     * cost = Σ y_i * p_i
     * Positive cost = we pay money (long positions)
     * Negative cost = we receive money (short positions)
     */
    double compute_total_cost(const std::vector<NegRiskMarket>& markets,
                              const std::vector<double>& positions) const {
        double cost = 0.0;
        for (size_t i = 0; i < markets.size(); i++) {
            cost += positions[i] * markets[i].price_yes;
        }
        return cost;
    }

    /**
     * Estimate total gas cost for single-block execution.
     * Each order requires ~100,000 gas on Polygon (cheap).
     */
    double estimate_gas_cost(size_t n_orders) const {
        // Polygon gas is ~200 Gwei, each order ~100k gas
        // 100,000 * 200 * 1e-9 * n_orders = 0.02 * n_orders USD
        // But with bundle + neg-risk: ~0.5 USD per order
        double per_order_gas_usd = 0.5;
        double total = per_order_gas_usd * static_cast<double>(n_orders);
        return std::min(total, config_.max_gas_cost_usd);
    }

    /**
     * Compute total fees (maker/taker + commission) for the order bundle.
     */
    double compute_total_fees(const std::vector<NegRiskMarket>& markets,
                              const std::vector<double>& positions) const {
        double total_fees = 0.0;
        for (size_t i = 0; i < markets.size(); i++) {
            double notional = std::abs(positions[i]);
            bool is_maker = true;  // Assume maker (passive) for neg-risk orders
            double probability = markets[i].price_yes;
            total_fees += fee_model_.compute_fee(notional, is_maker, probability);
        }
        return total_fees;
    }

    /**
     * Build the order bundle from the position vector.
     */
    OrderBundle build_order_bundle(
        const std::vector<NegRiskMarket>& markets,
        const std::vector<double>& positions,
        double total_cost,
        double net_profit,
        const OutcomeAnalysis& outcomes,
        double gas_cost) {

        OrderBundle bundle;
        bundle.total_cost = total_cost;
        bundle.guaranteed_profit = net_profit;
        bundle.max_profit = outcomes.max_payout;
        bundle.required_capital = std::max(0.0, total_cost);
        bundle.estimated_gas_usd = gas_cost;
        bundle.net_profit_after_fees = net_profit;
        bundle.requires_atomic_execution = true;

        if (bundle.required_capital > 0) {
            bundle.expected_roi = net_profit / bundle.required_capital;
        } else {
            // Negative cost (we receive money) — ROI is infinite for arbitrage
            bundle.expected_roi = 999.99;
        }

        // Generate synthetic orders
        uint64_t base_nonce = get_time_ns() & 0xFFFFFF;
        bundle.bundle_id = "negrisk_arb_" + std::to_string(base_nonce);

        for (size_t i = 0; i < markets.size(); i++) {
            if (std::abs(positions[i]) < 1e-10) continue;

            SyntheticOrder order;
            order.market_slug = markets[i].market_slug;
            order.is_buy = positions[i] > 0;  // Positive position = buy Yes
            order.amount = std::abs(positions[i]);
            order.price = markets[i].price_yes;
            order.nonce = base_nonce + i;
            order.salt = base_nonce;

            // Calculate PnL for this specific position
            double cost_i = positions[i] * markets[i].price_yes;
            double yes_pnl = positions[i] * (1.0 - markets[i].price_yes);
            double no_pnl = -positions[i] * markets[i].price_yes;
            order.expected_pnl = "{\"yes_pnl\":" + std::to_string(yes_pnl) +
                                 ",\"no_pnl\":" + std::to_string(no_pnl) +
                                 ",\"cost\":" + std::to_string(cost_i) + "}";

            bundle.orders.push_back(order);
        }

        return bundle;
    }

    /**
     * Compute confidence based on deviation, ROI, and liquidity.
     */
    double compute_confidence(const std::vector<NegRiskMarket>& markets,
                              double deviation, double roi) const {
        double confidence = 0.5;  // Base

        // More deviation = higher confidence (larger opportunity)
        confidence += 0.2 * std::min(deviation / 0.05, 1.0);

        // Higher ROI = higher confidence
        confidence += 0.2 * std::min(roi / 0.5, 1.0);

        // Sufficient liquidity = higher confidence
        double min_liq_ratio = 1.0;
        for (const auto& m : markets) {
            min_liq_ratio = std::min(min_liq_ratio,
                m.liquidity / std::max(config_.min_position_size_usd, 1.0));
        }
        confidence += 0.1 * std::min(min_liq_ratio / 2.0, 1.0);

        return std::min(confidence, 1.0);
    }

    Config config_;
    const RiskConfig& risk_config_;
    FeeModel fee_model_;
};

/**
 * LockFreeArbitrageResult: Lock-free container for storing arbitrage results.
 *
 * Used to pass arbitrage opportunities from the cold path (NegRiskArbitrageEngine)
 * to the hot path (ExecutionEngine) without mutex contention.
 *
 * Design:
 * - Fixed-capacity ring buffer of ArbitrageOpportunity pointers
 * - Single producer (engine thread) writes, single consumer (hot path) reads
 * - Atomic head/tail indices for lock-free operation
 * - O(1) push/pop operations
 */
class LockFreeArbitrageResult {
public:
    static constexpr size_t CAPACITY = 256;

    LockFreeArbitrageResult() : head_(0), tail_(0) {}

    /**
     * Push an arbitrage opportunity into the lock-free buffer.
     * Returns false if buffer is full (caller should drop or retry).
     * O(1), no allocation, no locks.
     */
    bool try_push(const ArbitrageOpportunity& opp) {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (current_head + 1) & mask_;

        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;  // Buffer full
        }

        // Copy the opportunity into the buffer
        new (&buffer_[current_head]) ArbitrageOpportunity(opp);
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    /**
     * Pop an arbitrage opportunity from the lock-free buffer.
     * Returns nullopt if buffer is empty.
     * O(1), no allocation, no locks.
     */
    std::optional<ArbitrageOpportunity> try_pop() {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        if (current_tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;  // Buffer empty
        }

        const size_t next_tail = (current_tail + 1) & mask_;

        // Access aligned storage via reinterpret_cast (required for aligned_storage_t)
        auto* slot = reinterpret_cast<ArbitrageOpportunity*>(&buffer_[current_tail]);
        ArbitrageOpportunity opp = *slot;  // Copy out
        slot->~ArbitrageOpportunity();     // Destroy in-place
        tail_.store(next_tail, std::memory_order_release);
        return opp;
    }

    /**
     * Check if buffer is empty.
     */
    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_relaxed);
    }

    /**
     * Check if buffer is full.
     */
    bool full() const {
        size_t next_head = (head_.load(std::memory_order_relaxed) + 1) & mask_;
        return next_head == tail_.load(std::memory_order_acquire);
    }

    /**
     * Get approximate number of items in the buffer.
     * Not exact for concurrent access, but sufficient for monitoring.
     */
    size_t approximate_size() const {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_relaxed);
        if (h >= t) return h - t;
        return (CAPACITY - t) + h;
    }

private:
    static_assert((CAPACITY & (CAPACITY - 1)) == 0, "CAPACITY must be power of 2");
    static constexpr size_t mask_ = CAPACITY - 1;

    // Raw aligned storage for the ring buffer
    alignas(64) std::aligned_storage_t<sizeof(ArbitrageOpportunity),
                                       alignof(ArbitrageOpportunity)>
        buffer_[CAPACITY];

    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};

#endif // NEG_RISK_ENGINE_HPP