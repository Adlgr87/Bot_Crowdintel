#ifndef FEE_MODEL_HPP
#define FEE_MODEL_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "order_book.hpp"
#include "market_config.hpp"

/**
 * FeeModel: Commission, gas, and slippage estimation for Polymarket CLOB V2.
 *
 * CRITICAL: The dynamic fee formula MUST be verified against Polymarket's official
 * documentation before production use. This implementation follows the formula
 * described in the workflow document and is configurable by flag.
 *
 * Formula: fee = C × 0.25 × (p·(1−p))²
 * Where:
 *   - C = base commission constant (configurable: FEE_DYNAMIC_C)
 *   - p = market probability (from q_value / confidence)
 *   - Max fee at p=0.5 (highest uncertainty)
 *   - Min fee at p=0.0 or p=1.0 (highest certainty)
 *
 * This formula is activated ONLY when dynamic_fees_enabled is set per-market
 * (never global). The base maker/taker fees are always applied.
 */
class FeeModel {
public:
    explicit FeeModel(const RiskConfig& config) : config_(config) {}

    /**
     * Compute total fee for an order.
     * Includes: maker/taker fee + dynamic component (if enabled) + gas.
     *
     * @param notional_usd  Order value in USD
     * @param is_maker      Whether this is a maker (limit) or taker (market) order
     * @param probability   Market probability (0.0 to 1.0), from q_value
     * @return Total fee in USD
     */
    double compute_fee(double notional_usd, bool is_maker, double probability) const {
        // 1. Base maker/taker fee
        double base_fee_rate = is_maker ? config_.maker_fee_rate : config_.taker_fee_rate;
        double base_fee = notional_usd * base_fee_rate;

        // 2. Dynamic component (if enabled for this market)
        double dynamic_fee = 0.0;
        if (config_.dynamic_fees_enabled) {
            dynamic_fee = compute_dynamic_fee(notional_usd, probability);
        }

        // 3. Base commission per order
        double order_commission = config_.base_commission_usd;

        // 4. Gas cost (Polygon)
        double gas_cost = config_.gas_cost_usd;

        return base_fee + dynamic_fee + order_commission + gas_cost;
    }

    /**
     * Dynamic fee: fee = C × 0.25 × (p·(1−p))²
     *
     * This produces a parabolic curve with maximum at p=0.5 and minimum
     * (zero) at p=0.0 and p=1.0.
     *
     * Verification vectors:
     *   p=0.5 → fee = C × 0.25 × (0.25)² = C × 0.015625 (maximum)
     *   p=0.0 → fee = C × 0.25 × 0 = 0 (minimum)
     *   p=1.0 → fee = C × 0.25 × 0 = 0 (minimum)
     *   p=0.25 → fee = C × 0.25 × (0.1875)² = C × 0.008789... (intermediate)
     */
    double compute_dynamic_fee(double notional_usd, double probability) const {
        // Clamp probability to [0, 1]
        double p = (std::max)(0.0, (std::min)(1.0, probability));
        double p_times_1_minus_p = p * (1.0 - p);
        double parabolic = p_times_1_minus_p * p_times_1_minus_p;  // (p·(1−p))²
        return config_.dynamic_C * 0.25 * parabolic * notional_usd;
    }

    /**
     * Estimate slippage based on order size relative to L2 depth.
     *
     * @param order_size_usd  Size in USD
     * @param spread_bps  Spread between best bid/ask in basis points
     * @param available_liquidity  Total liquidity at top of book (USD)
     * @return Expected slippage in USD
     */
    double estimate_slippage(double order_size_usd, double spread_bps,
                              double available_liquidity) const {
        if (available_liquidity <= 0.0) return order_size_usd * 0.01;  // 1% fallback

        // Linear slippage model: cost = order_size × spread × (1 + size/depth)
        double spread_usd = order_size_usd * (spread_bps / 10000.0);
        double depth_ratio = order_size_usd / available_liquidity;
        // Slippage increases with depth ratio
        double slippage_factor = 0.5 * depth_ratio * depth_ratio;
        return spread_usd * (1.0 + slippage_factor);
    }

    /**
     * Estimate total gas cost for the order lifecycle.
     *
     * @param num_transactions  Number of on-chain txs (approve + sign + submit + cancel)
     * @return Estimated gas cost in USD
     */
    double estimate_gas_cost(int num_transactions = 1) const {
        return config_.gas_cost_usd * num_transactions;
    }

    /**
     * Compute net expected value after fees.
     *
     * @param edge_usd  Gross expected value of the trade
     * @param notional_usd  Order size in USD
     * @param is_maker  Whether maker or taker
     * @param probability  Market probability
     * @param spread_bps  Spread in bps
     * @param available_liquidity  Liquidity at top of book (USD)
     * @return Net EV after all costs
     */
    double compute_net_ev(double edge_usd, double notional_usd, bool is_maker,
                           double probability, double spread_bps,
                           double available_liquidity) const {
        double fees = compute_fee(notional_usd, is_maker, probability);
        double slippage = estimate_slippage(notional_usd, spread_bps, available_liquidity);
        double gas = estimate_gas_cost();
        return edge_usd - fees - slippage - gas;
    }

private:
    const RiskConfig& config_;
};

#endif // FEE_MODEL_HPP
