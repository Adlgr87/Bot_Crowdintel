#ifndef MARKET_MAKING_ENGINE_HPP
#define MARKET_MAKING_ENGINE_HPP

#include <cstddef>
#include <cstdint>

#include "../../core/include/order_book.hpp"
#include "kelly_engine.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// MarketMakingEngine (cold path): EWMA fair value + symmetric quotes sized by
// fractional Kelly. Produces quotes; the execution engine (or backtester)
// decides whether to act on them.
// ─────────────────────────────────────────────────────────────────────────────

class MarketMakingEngine {
public:
    struct Quote {
        uint64_t bid_price;   // ×1e6 fixed
        uint64_t ask_price;   // ×1e6 fixed
        uint64_t bid_size;    // ×1e6 fixed (shares)
        uint64_t ask_size;    // ×1e6 fixed (shares)
        bool     valid;
    };

    MarketMakingEngine(double initial_fair, double half_spread_frac,
                       double bankroll_usd, double kelly_fraction_cap)
        : fair_(initial_fair),
          half_spread_(half_spread_frac),
          bankroll_(bankroll_usd),
          kelly_cap_(kelly_fraction_cap) {}

    // EWMA update of the fair value from traded/new mid prices.
    void update_fair(uint64_t price_x1e6) {
        const double p = (double)price_x1e6;
        fair_ = (fair_ * (1.0 - alpha_)) + (p * alpha_);
    }

    // Symmetric quote around the EWMA fair value; sized by an assumed edge
    // (the caller can override with a real CrowdIntel signal).
    Quote generate_quote(double assumed_p_win) const {
        const uint64_t fair = (uint64_t)fair_;
        const uint64_t bid  = round_price_to_tick((uint64_t)(fair * (1.0 - half_spread_)), 10000);
        const uint64_t ask  = round_price_to_tick((uint64_t)(fair * (1.0 + half_spread_)), 10000);
        if (bid >= ask) return {0, 0, 0, 0, false};

        const double price = (double)bid / 1000000.0;
        const double k     = KellyEngine::kelly_buy(assumed_p_win, price);
        const double usd   = KellyEngine::position_usd(k, kelly_cap_, bankroll_);
        const uint64_t sz  = KellyEngine::usd_to_shares_fixed(usd, price);
        if (sz == 0) return {0, 0, 0, 0, false};
        return {bid, ask, sz, sz, true};
    }

    double fair_value() const { return fair_; }

private:
    double   fair_;
    double   half_spread_;
    double   bankroll_;
    double   kelly_cap_;
    double   alpha_ = 0.01;   // EWMA smoothing for the fair value
};

#endif // MARKET_MAKING_ENGINE_HPP
