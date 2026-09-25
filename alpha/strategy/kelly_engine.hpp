#ifndef KELLY_ENGINE_HPP
#define KELLY_ENGINE_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

/**
 * KellyEngine: Optimal position sizing via the Kelly Criterion,
 * enhanced with a Slippage/Impact Model that penalizes position
 * size based on order-book depth.
 *
 * ── Original (pre-fase B) ──
 *   f* = ev × confidence × fraction
 *   size = f* × max_position
 *   (No slippage awareness — naive, can over-size in illiquid markets.)
 *
 * ── Refactored (Fase B) ──
 *   1. Compute gross Kelly fraction from EV and confidence.
 *   2. Estimate slippage from order-book depth, spread, and order size.
 *   3. Adjust effective edge: net_edge = gross_edge − slippage − fees − gas.
 *   4. If net_edge ≤ 0 → position size = 0 (not profitable after costs).
 *   5. Position size penalty: as slippage/edge ratio grows, shrink size
 *      proportionally to avoid walking the book into adverse selection.
 *
 * The slippage model is: cost = spread_cost × (1 + k × (size/depth)²)
 *   - spread_cost = notional × (spread_bps / 10 000)
 *   - depth ratio = notional / available_liquidity
 *   - k = shape parameter (default 0.5) — quadratic impact for illiquid tails
 *
 * This matches FeeModel::estimate_slippage for consistency, but KellyEngine
 * owns the *penalty* logic that scales position size down.
 */
class KellyEngine {
public:
    // ─── Backward-Compatible Interface ────────────────────────────────────

    /**
     * Calculate fractional Kelly for binary outcomes.
     * f* = (bp − q) / b  ≈  ev × confidence  (simplified)
     *
     * @param ev           Expected value per dollar (from AlphaSignal::ev_per_dollar)
     * @param confidence   Confidence in the signal (0.0−1.0)
     * @param fraction     Kelly fraction (e.g., 0.1 = 10% Kelly for volatility control)
     * @return Optimal fraction of max_pos to allocate (0.0−1.0)
     */
    static double calculate_fractional_kelly(double ev, double confidence, double fraction = 0.1) {
        if (ev <= 0) return 0.0;
        double optimal_f = ev * confidence;
        return std::max(0.0, std::min(1.0, optimal_f * fraction));
    }

    /** Convert Kelly fraction to USD position size. */
    static double calculate_position_size(double kelly_f, double max_position_usd) {
        return kelly_f * max_position_usd;
    }

    // ─── Slippage/Impact Model ───────────────────────────────────────────

    /**
     * Slippage parameters for the depth-aware Kelly model.
     *
     * All costs are in USD. Spread is in basis points (bps).
     */
    struct SlippageParams {
        double spread_bps;              ///< Current spread (best ask − best bid) in bps
        double available_liquidity;     ///< USD liquidity at top of book (level 0)
        double gas_cost_usd;            ///< Gas cost per order (Polygon)
        double maker_fee_rate;          ///< Maker (liquidity-add) fee rate
        double taker_fee_rate;          ///< Taker (liquidity-remove) fee rate
        double base_commission_usd;     ///< Fixed per-order commission
        double min_net_ev_usd;          ///< Minimum net EV to execute (profitability gate)
        double probability;             ///< Market probability p ∈ [0, 1] (for dynamic fees)
        bool dynamic_fees_enabled;      ///< Whether dynamic probability-weighted fees apply
        double dynamic_C;               ///< Dynamic fee constant C
        bool is_maker;                  ///< Whether the order is maker (limit) or taker (market)
    };

    /**
     * Estimate total slippage/impact cost for an order.
     *
     * Model: cost = spread_cost × (1 + k × (size / depth)²)
     *   - Linear in spread for the base cost
     *   - Quadratic impact term penalizes large orders relative to depth
     *   - k = 0.5 gives realistic convexity for CLOB markets
     *
     * @param order_size_usd     Order notional in USD
     * @param spread_bps         Spread in basis points (1 bp = 0.01%)
     * @param available_liquidity  Top-of-book liquidity in USD
     * @param k                  Impact shape parameter (default 0.5)
     * @return Expected slippage cost in USD
     */
    static double estimate_slippage(double order_size_usd,
                                    double spread_bps,
                                    double available_liquidity,
                                    double k = 0.5) {
        if (available_liquidity <= 0.0) {
            // No liquidity — flat 1% penalty
            return order_size_usd * 0.01;
        }

        // Spread cost: crossing the spread for a taker order
        double spread_cost = order_size_usd * (spread_bps / 10000.0);

        // Depth ratio: how much of the book we consume
        double depth_ratio = order_size_usd / available_liquidity;

        // Quadratic impact: cost grows with (size/depth)²
        // k = 0.5 → at size = depth, impact adds 50% on top of spread cost
        double impact_term = k * depth_ratio * depth_ratio;

        return spread_cost * (1.0 + impact_term);
    }

    /**
     * Estimate total execution cost: fees + slippage + gas.
     *
     * @param notional_usd   Order value in USD
     * @param params         Slippage/fee parameters
     * @return Total expected cost in USD
     */
    static double estimate_total_cost(double notional_usd, const SlippageParams& params) {
        // 1. Fee cost (maker or taker rate)
        double fee_rate = params.is_maker ? params.maker_fee_rate : params.taker_fee_rate;
        double fee_cost = notional_usd * fee_rate;

        // 2. Dynamic fee (probability-weighted, Polymarket-specific)
        double dynamic_fee = 0.0;
        if (params.dynamic_fees_enabled) {
            double p = std::clamp(params.probability, 0.0, 1.0);
            double entropy = p * (1.0 - p);
            double parabolic = entropy * entropy;  // (p·(1−p))²
            dynamic_fee = params.dynamic_C * 0.25 * parabolic * notional_usd;
        }

        // 3. Base commission
        double commission = params.base_commission_usd;

        // 4. Gas cost
        double gas = params.gas_cost_usd;

        // 5. Slippage/impact
        double slippage = estimate_slippage(
            notional_usd, params.spread_bps, params.available_liquidity);

        return fee_cost + dynamic_fee + commission + gas + slippage;
    }

    // ─── Depth-Aware Position Sizing (Kelly + Slippage Penalty) ───────────

    /**
     * Result of the depth-aware Kelly calculation.
     */
    struct KellyResult {
        double position_size_usd;     ///< Recommended position size (after slippage penalty)
        double kelly_fraction;         ///< Raw Kelly fraction (before penalty)
        double adjusted_fraction;      ///< Position fraction after slippage penalty
        double gross_edge_usd;         ///< EV before costs
        double net_edge_usd;           ///< EV after all costs (slippage + fees + gas)
        double expected_slippage_usd;  ///< Slippage + fees + gas for the recommended size
        double depth_ratio;            ///< size / available_liquidity at optimal size
        bool is_profitable;            ///< net_edge > min_net_ev (profitability gate)
        const char* rejection_reason;  ///< Why the trade was rejected (if not profitable)
    };

    /**
     * Calculate optimal position size using Kelly Criterion with
     * depth-aware slippage penalty.
     *
     * Algorithm:
     *   1. Compute gross Kelly fraction: f_gross = ev × confidence × kelly_fraction
     *   2. Compute gross position: pos_gross = f_gross × max_position_usd
     *   3. Estimate total cost at pos_gross: cost = fees + slippage + gas
     *   4. Net edge: net = gross_edge − cost
     *   5. If net < 0 → shrink position proportionally:
     *      pos_adjusted = pos_gross × max(0, net / gross_edge)
     *   6. If net < min_net_ev → reject (is_profitable = false)
     *
     * The penalty is: position_penalty = (gross_edge − cost) / gross_edge
     *   - When cost → 0: penalty → 1 (no reduction)
     *   - When cost → gross_edge: penalty → 0 (eliminate position)
     *   - When cost > gross_edge: penalty = 0 (position eliminated)
     *
     * @param ev_per_dollar   Expected value per dollar (from AlphaSignal)
     * @param confidence      Signal confidence (0.0−1.0)
     * @param max_position_usd  Maximum position size in USD (capital constraint)
     * @param params          Slippage and fee parameters
     * @param kelly_fraction   Kelly fraction multiplier (default 0.1 = 10% Kelly)
     * @return KellyResult with position size and detailed breakdown
     */
    static KellyResult calculate_kelly_with_slippage(
        double ev_per_dollar,
        double confidence,
        double max_position_usd,
        const SlippageParams& params,
        double kelly_fraction = 0.1) {

        // 1. Gross Kelly fraction (same as original)
        double f_gross = calculate_fractional_kelly(ev_per_dollar, confidence, kelly_fraction);

        // 2. If no available liquidity, position must be 0 (can't execute)
        if (params.available_liquidity <= 0.0) {
            return {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false, "zero_liquidity"};
        }

        // 3. Gross position size (capital-constrained)
        double pos_gross = f_gross * max_position_usd;
        if (pos_gross <= 0.0) {
            return {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, false, "non_positive_edge"};
        }

        // 3. Gross edge at full position
        double gross_edge_usd = ev_per_dollar * pos_gross;

        // 4. Estimate total cost at full position
        double total_cost = estimate_total_cost(pos_gross, params);

        // 5. Net edge after costs
        double net_edge_usd = gross_edge_usd - total_cost;

        // 6. Slippage penalty: proportional shrink based on cost-to-edge ratio
        //    penalty ∈ [0, 1] — 1 means no penalty (cost ≤ 0), 0 means eliminate
        double penalty = 1.0;
        if (gross_edge_usd > 0.0) {
            penalty = net_edge_usd / gross_edge_usd;
            penalty = std::max(0.0, penalty);
        }

        // 7. Adjusted position size
        double pos_adjusted = pos_gross * penalty;
        double adjusted_fraction = f_gross * penalty;

        // 8. Re-compute cost at the ADJUSTED size (iterative convergence)
        //    This gives a more accurate slippage estimate.
        //    One iteration is sufficient — the quadratic impact model converges fast.
        if (pos_adjusted > 0.0 && pos_adjusted < pos_gross) {
            double adjusted_cost = estimate_total_cost(pos_adjusted, params);
            double adjusted_net = ev_per_dollar * pos_adjusted - adjusted_cost;
            if (adjusted_net > net_edge_usd) {
                // Re-adjust if the smaller position is actually more profitable
                // (due to reduced slippage)
                double adjusted_penalty = adjusted_net / (ev_per_dollar * pos_adjusted);
                adjusted_penalty = std::max(0.0, std::min(1.0, adjusted_penalty));
                pos_adjusted = pos_gross * adjusted_penalty;
                adjusted_fraction = f_gross * adjusted_penalty;
                net_edge_usd = ev_per_dollar * pos_adjusted - estimate_total_cost(pos_adjusted, params);
                total_cost = estimate_total_cost(pos_adjusted, params);
            }
        }

        // 9. Compute depth ratio at final position
        double depth_ratio = (params.available_liquidity > 0.0)
            ? pos_adjusted / params.available_liquidity
            : 1.0;

        // 10. Profitability gate
        bool is_profitable = net_edge_usd >= params.min_net_ev_usd;

        return {
            pos_adjusted,          // position_size_usd
            f_gross,               // kelly_fraction (raw)
            adjusted_fraction,     // adjusted_fraction
            gross_edge_usd,        // gross_edge_usd
            net_edge_usd,          // net_edge_usd (at adjusted size)
            total_cost,            // expected_slippage_usd (total cost)
            depth_ratio,           // depth_ratio
            is_profitable,         // is_profitable
            is_profitable ? "" : "net_ev_below_min"
        };
    }

    /**
     * Simple depth-aware sizing: adjust Kelly fraction based on how much
     * of the book would be consumed.
     *
     * If the gross position exceeds available liquidity, scale it down
     * to a fraction of the book depth.
     *
     * @param kelly_f           Raw Kelly fraction
     * @param max_position_usd  Max USD position
     * @param available_liq_usd  Available USD liquidity at top of book
     * @param max_depth_ratio   Max fraction of depth to consume (default 0.3 = 30%)
     * @return Adjusted position size in USD
     */
    static double size_to_depth(double kelly_f, double max_position_usd,
                                double available_liq_usd,
                                double max_depth_ratio = 0.3) {
        double pos_gross = kelly_f * max_position_usd;
        if (available_liq_usd <= 0.0) return 0.0;

        double max_safe_size = available_liq_usd * max_depth_ratio;
        if (pos_gross > max_safe_size) {
            return max_safe_size;  // Don't exceed max_depth_ratio of available liquidity
        }
        return pos_gross;
    }
};

#endif // KELLY_ENGINE_HPP
