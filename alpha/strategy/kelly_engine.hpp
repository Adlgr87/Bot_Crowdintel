#ifndef KELLY_ENGINE_HPP
#define KELLY_ENGINE_HPP

// ─────────────────────────────────────────────────────────────────────────────
// KellyEngine: exact Kelly criterion for binary prediction-market contracts.
//
// Polymarket contracts pay $1 if the outcome occurs, and trade at price p.
// Buying YES at p with true win probability w is a binary bet with net odds
// b = (1−p)/p, and the growth-optimal fraction of bankroll is:
//
//      f*_buy  = (w − p) / (1 − p)
//      f*_sell = (p − w) / p          (selling YES ≡ buying NO at 1−p)
//
// (Derivation: maximize E[log wealth] for a $1 payoff contract. The old
// "ev × confidence" heuristic was not Kelly and over-bet at high prices.)
//
// The engine applies a fractional-Kelly multiplier (variance control) and
// hard caps by bankroll.
// ─────────────────────────────────────────────────────────────────────────────

class KellyEngine {
public:
    // Exact Kelly fraction for buying YES at `price` with win prob `p_win`.
    // Returns 0 when there is no edge or inputs are degenerate.
    static double kelly_buy(double p_win, double price) {
        if (p_win <= price || price <= 0.0 || price >= 1.0 || p_win <= 0.0) return 0.0;
        return (p_win - price) / (1.0 - price);
    }

    // Exact Kelly fraction for selling YES at `price` with win prob `p_win`.
    static double kelly_sell(double p_win, double price) {
        if (p_win >= price || price <= 0.0 || price >= 1.0 || p_win >= 1.0) return 0.0;
        return (price - p_win) / price;
    }

    // Fractional Kelly → position size in USD (never exceeds the cap).
    static double position_usd(double kelly_f, double fraction_cap, double bankroll_usd) {
        if (kelly_f <= 0.0) return 0.0;
        double f = kelly_f * fraction_cap;
        if (f > 1.0) f = 1.0;
        return f * bankroll_usd;
    }

    // USD budget → shares (×1e6 fixed), floor-rounded, at `price`.
    static uint64_t usd_to_shares_fixed(double usd, double price) {
        if (usd <= 0.0 || price <= 0.0) return 0;
        const double shares = usd / price;
        if (shares <= 0.0) return 0;
        return (uint64_t)(shares * 1000000.0);
    }
};

#endif // KELLY_ENGINE_HPP
