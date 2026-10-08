// ─────────────────────────────────────────────────────────────────────────────
// kelly_sizer.hpp — Phase 4: Quarter-Kelly Position Sizer (REPLACES old API)
//
// Determines order size based on model confidence, market price, bankroll,
// inventory, volatility, and OFI multiplier. Uses FeeCalculator for exact
// fee-aware edge computation.
//
// FÓRMULA:
//   edge  = p_model − market_price − fee         (fee via FeeCalculator)
//   b     = (1 / market_price) − 1                (net odds)
//   q     = 1 − p_model
//   f*    = (p_model · b − q) / b                 (full Kelly fraction)
//   f_q   = f* · fraction                         (quarter-Kelly)
//
//   inv_adj = 1 − |inventory|
//   vol_adj = 1 / (1 + max(0, vol_z_score))
//   f_adj   = f_q · inv_adj · vol_adj · ofi_multiplier
//
//   size_usdc = min(f_adj · bankroll, bankroll · max_bankroll_pct)
//   size_shares = min(size_usdc / market_price, max_position)
//   floor: size_shares must be ≥ 1 (else NO TRADE)
//
// RULES:
//   - Edge < min_edge → should_trade = false
//   - kelly_raw ≤ 0  → should_trade = false
//   - size_shares < 1 → should_trade = false
//   - Edge must exceed fee: p_model > breakeven_winrate
//
// INVARIANTS:
//   - O(1), single-thread, < 500ns
//   - Zero heap allocation
//   - All inputs clamped / guarded against degenerate values
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <cmath>
#include "fee_calculator.hpp"

struct Config {
    double kelly_fraction = 0.25;     // Quarter-Kelly (0.25 = 1/4)
    double max_bankroll_pct = 0.05;   // Max 5% of bankroll per trade
    double min_edge = 0.005;          // 0.5% min edge post-fees
    double max_position = 100.0;      // Max 100 shares
};

struct SizingResult {
    bool should_trade;
    double size_shares;
    double size_usdc;
    double kelly_raw;
    double kelly_fractioned;
    double edge;
    double fee_cost;
};

class KellySizer {
public:
    explicit KellySizer(const Config& config = {}) noexcept
        : config_(config) {}

    SizingResult compute(double p_model,
                         double market_price,
                         double bankroll_usdc,
                         double inventory,
                         double vol_z_score,
                         bool is_taker,
                         double ofi_size_multiplier = 1.0) const noexcept
    {
        SizingResult result{};

        // Guard against degenerate price / probability inputs.
        // bankroll_usdc == 0 is allowed: Kelly is still computed (informational)
        // but size resolves to 0 → no trade.
        if (market_price <= 0.0 || market_price >= 1.0 ||
            p_model < 0.0 || p_model > 1.0) {
            result.should_trade = false;
            result.size_shares = 0.0;
            result.size_usdc = 0.0;
            result.kelly_raw = 0.0;
            result.kelly_fractioned = 0.0;
            result.edge = 0.0;
            result.fee_cost = 0.0;
            return result;
        }
        if (bankroll_usdc < 0.0) bankroll_usdc = 0.0;

        // ── Fee & edge ───────────────────────────────────────────────────────
        const double fee = FeeCalculator::taker_fee(market_price) *
                           (is_taker ? 1.0 : 0.0);
        result.fee_cost = fee;

        const double edge = p_model - market_price - fee;
        result.edge = edge;

        if (edge < config_.min_edge) {
            result.should_trade = false;
            result.size_shares = 0.0;
            result.size_usdc = 0.0;
            result.kelly_raw = 0.0;
            result.kelly_fractioned = 0.0;
            return result;
        }

        // ── Full Kelly ───────────────────────────────────────────────────────
        const double odds = (1.0 / market_price) - 1.0;
        const double q = 1.0 - p_model;
        const double kelly_raw = (p_model * odds - q) / odds;
        result.kelly_raw = kelly_raw;

        if (kelly_raw <= 0.0) {
            result.should_trade = false;
            result.size_shares = 0.0;
            result.size_usdc = 0.0;
            result.kelly_fractioned = 0.0;
            return result;
        }

        // ── Quarter-Kelly ────────────────────────────────────────────────────
        double kelly_fractioned = kelly_raw * config_.kelly_fraction;

        // ── Adjustments ──────────────────────────────────────────────────────
        // Inventory adjustment: reduce as |inventory| → 1
        double inv_adjust = 1.0 - std::abs(inventory);
        if (inv_adjust < 0.0) inv_adjust = 0.0;

        // Volatility adjustment: shrink as vol_z_score grows
        double vol_adjust = 1.0 / (1.0 + std::max(0.0, vol_z_score));

        // OFI multiplier (market-signal driven size adjustment)
        double ofi_adjust = ofi_size_multiplier;
        if (ofi_adjust < 0.0) ofi_adjust = 0.0;

        kelly_fractioned *= inv_adjust * vol_adjust * ofi_adjust;

        // ── Cap to max_bankroll_pct ──────────────────────────────────────────
        double capped_fraction = std::min(kelly_fractioned,
                                          config_.max_bankroll_pct);
        result.kelly_fractioned = kelly_fractioned;

        // ── Convert to dollar size ───────────────────────────────────────────
        const double size_usdc = capped_fraction * bankroll_usdc;
        result.size_usdc = size_usdc;

        // ── Convert to shares, cap at max_position ───────────────────────────
        double size_shares = size_usdc / market_price;
        if (size_shares > config_.max_position) {
            size_shares = config_.max_position;
        }
        result.size_shares = size_shares;

        // ── Floor: minimum 1 share ───────────────────────────────────────────
        if (size_shares < 1.0) {
            result.should_trade = false;
            result.size_shares = 0.0;
            result.size_usdc = 0.0;
        } else {
            result.should_trade = true;
        }

        return result;
    }

private:
    Config config_;
};
