// ─────────────────────────────────────────────────────────────────────────────
// fee_calculator_v2.hpp — CLOB V2 Non-Linear Fee Engine
//
// Replaces the original linear fee model (0.072·p·(1-p)) with the exact
// Polymarket CLOB V2 fee structure: tiered absolute fees by market category.
//
// CRITICAL FIX: The old model produced false-positive arbitrage signals.
// Static pair at 0.973 appeared profitable (+0.27¢ edge) under old model,
// but with CLOB V2 fees produces a net LOSS of -23.33¢.
//
// ADR-002: https://github.com/Adlgr87/Bot_Crowdintel/blob/main/docs/adr/ADR-002-clob-v2-fee-engine.md
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#if defined(__GNUC__) || defined(__clang__)
#define FEE_V2_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define FEE_V2_ALWAYS_INLINE inline
#endif

namespace crowdintel {

// ── Market Categories ────────────────────────────────────────────────────────
// Categories determine fee tier structure on Polymarket CLOB V2
enum class MarketCategory {
    Standard,     // Standard markets: tiered fees
    Policy,       // Policy markets: flat $0.01 fee
    Geopolitics   // Geopol analytics: $0 fee (no transaction fees)
};

class FeeCalculatorV2 {
public:
    // Quantum: fees rounded to 5 decimal places (0.00001 USD)
    static constexpr double FEE_QUANTUM = 0.00001;

    /**
     * @brief Calculate taker fee in USD for a given price and market category
     *
     * CLOB V2 Fee Structure:
     * - Standard: $0.0175/contract at p ∈ (0.1, 0.9); $0.0063 at tails
     * - Policy:   $0.01000/contract (flat, all prices)
     * - Geopolitics: $0.00000 (no fees)
     *
     * @param price_normalized Normalized price in [0, 1]
     * @param category Market category
     * @return Fee in USD, rounded to 5 decimal places
     */
    FEE_V2_ALWAYS_INLINE
    static double taker_fee_usd(double price_normalized, MarketCategory category) noexcept {
        const double p = std::clamp(price_normalized, 0.0, 1.0);

        switch (category) {
            case MarketCategory::Standard:
                // Tiered: $0.0175 at center, $0.0063 at tails
                if (p <= 0.1 || p >= 0.9) [[unlikely]] {
                    return round_to_quantum(0.00630);
                }
                return round_to_quantum(0.01750);

            case MarketCategory::Policy:
                // Flat fee regardless of price
                return round_to_quantum(0.01000);

            case MarketCategory::Geopolitics:
                // No fees for geopolitics markets
                return 0.0;

            default:
                return round_to_quantum(0.01750);
        }
    }

    /**
     * @brief Convert USD fee to probability-space fee
     *
     * fee_prob = fee_usd / (1.0 - price)
     * At p=0.973: fee_usd=0.00630, fee_prob = 0.00630/0.027 = 0.23333
     *
     * @param price_normalized Normalized price in [0, 1]
     * @param category Market category
     * @return Fee in probability space, rounded to 5 decimals
     */
    FEE_V2_ALWAYS_INLINE
    static double taker_fee_prob(double price_normalized, MarketCategory category = MarketCategory::Standard) noexcept {
        const double p = std::clamp(price_normalized, 0.0, 1.0);
        const double fee_usd = taker_fee_usd(p, category);

        // Avoid division by zero at p=1.0
        if (p >= 0.99999) [[unlikely]] {
            return 0.0;
        }

        const double fee_prob = fee_usd / (1.0 - p);
        return round_to_quantum(fee_prob);
    }

    /**
     * @brief Round to nearest quantum (5 decimal places)
     */
    FEE_V2_ALWAYS_INLINE
    static double round_to_quantum(double value) noexcept {
        return std::round(value / FEE_QUANTUM) * FEE_QUANTUM;
    }

    /**
     * @brief Calculate edge after fees (probability space)
     *
     * edge = p_model - market_price - fee_prob
     * Positive edge means the trade has positive expected value.
     *
     * @param p_model Model probability [0, 1]
     * @param market_price Current market price [0, 1]
     * @param is_taker Whether this is a taker order (pays fees)
     * @param category Market category for fee calculation
     * @return Edge in probability space
     */
    FEE_V2_ALWAYS_INLINE
    static double edge_after_fees(
        double p_model,
        double market_price,
        bool is_taker = true,
        MarketCategory category = MarketCategory::Standard
    ) noexcept {
        if (!is_taker) {
            return p_model - market_price;
        }

        const double fee_prob = taker_fee_prob(market_price, category);
        return p_model - market_price - fee_prob;
    }

    /**
     * @brief Check if arbitrage opportunity is viable
     *
     * Arbitrage is viable only when:
     * 1. p_model - market_price - fee_prob > minimum_edge (default: 0.004 = 0.4¢)
     * 2. Both sides have sufficient liquidity
     *
     * @param p_model Model probability [0, 1]
     * @param market_price Current market price [0, 1]
     * @param category Market category
     * @param min_edge Minimum edge after fees (default 0.004)
     * @return true if arbitrage is viable
     */
    FEE_V2_ALWAYS_INLINE
    static bool is_arbitrage_viable(
        double p_model,
        double market_price,
        MarketCategory category = MarketCategory::Standard,
        double min_edge = 0.004
    ) noexcept {
        const double edge = edge_after_fees(p_model, market_price, true, category);
        return edge > min_edge;
    }
};

} // namespace crowdintel

// ── Legacy compatibility alias ───────────────────────────────────────────────
// For gradual migration from old FeeCalculator to V2
using FeeCalculatorV2 = crowdintel::FeeCalculatorV2;
using crowdintel_MarketCategory = crowdintel::MarketCategory;