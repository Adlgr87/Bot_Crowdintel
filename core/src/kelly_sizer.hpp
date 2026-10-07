// ─────────────────────────────────────────────────────────────────────────────
// kelly_sizer.hpp — Phase 4: Quarter-Kelly Position Sizer
//
// Determines order size based on model confidence, market price,
// bankroll, and inventory constraints.
//
// FÓRMULA:
//   f*      = (p·b - q) / b          // full Kelly
//   f_quarter = f* / 4                // quarter-Kelly (conservative)
//   f_clamped = min(f_quarter, MAX_BANKROLL_PCT)
//
//   donde:
//     p = prob de ganar (CfC + P4 bayesiano)
//     q = 1 - p
//     b = odds netas = (1/price) - 1
//     fees: maker=0%, taker≈2%
//
// INVARIANTS:
//   - O(1) single-thread, < 500ns
//   - Zero heap allocation
//   - Clamped to [0, MAX_BANKROLL_PCT]
//   - Inventory-aware: size *= (1 - |inv|/max_inv)
//   - Volatility-aware: size *= (1 / (1 + vol_z_score))
//
// TODO(P4-T1): Implement full fee-adjusted odds.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdint>
#include <algorithm>

struct KellyConfig {
    float fraction = 0.25f;             // quarter-Kelly
    float max_bankroll_pct = 0.05f;     // max 5% of bankroll per trade
    float min_edge_maker = 0.005f;      // 0.5% min edge (maker, no fee)
    float min_edge_taker = 0.025f;      // 2.5% min edge (taker, ~2% fee)
    float max_inventory_shares = 100.0f; // inventory cap
    float max_vol_zscore = 3.0f;        // cap vol multiplier
};

struct alignas(64) KellyOutput {
    uint64_t order_shares;       // quantized shares to order
    uint64_t order_usd_cents;    // USD cost (cents)
    float fraction_of_bankroll;  // actual fraction used
    bool should_trade;         // false if below min_edge or capped
    float edge_bps;            // informational: model edge in basis points
};

// ── KellySizer ───────────────────────────────────────────────────────────────
class KellySizer {
public:
    explicit KellySizer(const KellyConfig& cfg) : cfg_(cfg) {}

    // Compute order size. All inputs are scalars — no structs, no pointers.
    // p: model probability of win [0, 1]
    // price_cents: market price in cents (e.g., 2650000 for $265.00)
    // bankroll_cents: total bankroll in cents
    // inventory_shares: current position (positive = long)
    // vol_zscore: realized volatility z-score vs recent baseline
    // is_maker: true if expected to be maker (0% fee)
    KellyOutput compute(float p,
                        uint64_t price_cents,
                        uint64_t bankroll_cents,
                        float inventory_shares,
                        float vol_zscore,
                        bool is_maker) const noexcept;

    // Convenience: quarter-Kelly fraction (for P4 weighting)
    static float quarter_kelly_fraction(float p, float b) noexcept;

private:
    KellyConfig cfg_;

    // Fee-adjusted effective odds
    float effective_b(float b, bool is_maker) const noexcept {
        if (is_maker) return b;
        // Taker fee ~2% → effective b = b·(1-0.02)
        return b * 0.98f;
    }
};
