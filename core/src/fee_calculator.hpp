// ─────────────────────────────────────────────────────────────────────────────
// fee_calculator.hpp — Phase 4: Deterministic fee math for Polymarket binary
// contracts.
//
// Fees are charged on the *payout* (not the notional) for taker orders. The
// taker fee rate is 7.2% of (price × (1 − price)), which equals 7.2% of the
// maximum achievable payout for a $1 binary contract at probability `price`.
// Maker orders post liquidity and pay 0 fee.
//
//   taker_fee(p) = 0.072 · p · (1 − p)
//
// Breakeven win-rate = the model probability at which edge_after_fees == 0:
//
//   edge(p_model, price) = p_model − price − fee
//   0 = p_model − price − fee  →  p_model = price + fee
//
// For a taker: breakeven = price + taker_fee(price).
//   e.g. price=0.50 → fee=0.018 → breakeven=0.518 (51.8%).
// For a maker: breakeven = price (no fee).
//
// CRITERIA:
//   - O(1), single-thread, deterministic
//   - Zero heap allocation
//   - Pure functions (no FP state, no exceptions)
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <cmath>

class FeeCalculator {
public:
    // Taker fee: 7.2% of price × (1 − price) (the binary payout at this price).
    static double taker_fee(double price) noexcept {
        // Clamp price to [0, 1] to keep the payout factor non-negative.
        const double p = price < 0.0 ? 0.0 : (price > 1.0 ? 1.0 : price);
        return 0.072 * p * (1.0 - p);
    }

    // Maker fee: always zero (liquidity provision).
    static double maker_fee(double /*price*/) noexcept {
        return 0.0;
    }

    // Breakeven win-rate: the model probability at which edge_after_fees == 0.
    // For taker: price + taker_fee(price).  For maker: price.
    static double breakeven_winrate(double price, bool is_taker) noexcept {
        const double fee = is_taker ? taker_fee(price) : 0.0;
        return price + fee;
    }

    // Edge after fees: p_model − market_price − fee.
    // Positive edge → trade has positive expected value.
    static double edge_after_fees(double p_model, double market_price,
                                  bool is_taker) noexcept {
        const double fee = is_taker ? taker_fee(market_price) : 0.0;
        return p_model - market_price - fee;
    }
};
