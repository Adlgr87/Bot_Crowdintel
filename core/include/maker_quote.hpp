#ifndef CROWDINTEL_MAKER_QUOTE_HPP
#define CROWDINTEL_MAKER_QUOTE_HPP

// ─────────────────────────────────────────────────────────────────────────────
// maker_quote.hpp — Motor C decision core: where to rest a two-sided quote.
//
// WHY THIS IS A PURE FUNCTION
// The maker sleeve's economics were measured before writing any code
// (docs/EXPERIMENT_MOTOR_C.md): carry from liquidity rewards + half the spread,
// minus the adverse selection that fills you exactly when the price moves
// against you. Median net was +0.392c per fill on 19 of 22 markets, and the one
// number that can still kill the sleeve is the CONDITIONAL adverse move.
//
// That makes the placement decision arithmetic, not plumbing: given the book,
// the model's fair value, the venue's reward band, and one tunable risk number,
// the answer is deterministic. Keeping it out of the hot path means it can be
// tested exhaustively, which is the only way to trust a number that decides
// where every quote goes.
//
// THE DECISION, IN ORDER
//   1. the book must be sane (0 < bid < ask < 1)
//   2. the market's own spread must be wide enough to cover the cushion;
//      a market already tighter than our cost is not worth quoting
//   3. the required half-width must fit inside the venue's reward band,
//      otherwise we cannot earn rewards and cover adverse selection at once
//   4. the order must be at least rewardsMinSize shares, or it earns nothing
//   5. inventory skew reduces then stops the side we are already long
//
// Units are the repository's usual fixed point: prices are probability × 1e6,
// so 0.4450 == 445000 and one basis point == 100 units. Sizes are shares × 1e6.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>

namespace crowdintel {

// 1 basis point of probability in the 1e6 fixed-point domain.
inline constexpr uint64_t MAKER_BPS_UNITS = 100;
// 1 cent of probability (0.01) in the same domain.
inline constexpr uint64_t MAKER_CENT_UNITS = 10000;
// The venue reports rewardsMaxSpread in fractional cents (3.5, 4.5, 6.5), so
// the band is carried in basis points: 3.5 cents == 350 bps.

struct QuoteInput {
    uint64_t best_bid = 0;     // 1e6 units
    uint64_t best_ask = 0;     // 1e6 units
    uint64_t fair_value = 0;   // model probability, 1e6 units
    uint64_t tick_size = 10000;  // 0.01 by default; these markets use 1000
    int64_t inventory_shares = 0;  // signed, 1e6 scale (positive = long YES)
};

struct QuoteConfig {
    // The tunable that decides the sleeve. 25 bps = 0.25c of cushion against the
    // adverse move a fill implies. Too small and the sleeve bleeds; too large
    // and no quote ever fits the reward band.
    uint32_t adverse_sel_bps = 25;
    uint32_t min_edge_bps = 10;  // profit floor on top of the cushion
    // Venue eligibility for liquidity rewards, in basis points of probability.
    // The live venue reports rewardsMaxSpread in CENTS and the values seen are
    // fractional (3.5, 4.5, 6.5), so cents cannot be an integer field. 3.5c is
    // 350 bps. 0 means "no reward program".
    uint32_t rewards_max_spread_bps = 0;
    uint64_t rewards_min_size_shares = 0;  // 1e6 scale
    uint64_t order_shares = 0;             // 1e6 scale, per side
    uint64_t max_inventory_shares = 0;     // 1e6 scale, 0 = unlimited
};

enum class QuoteStatus : uint8_t {
    Placed = 0,
    InvalidBook,             // 0 < bid < ask < 1 violated
    InvalidFairValue,        // fair value outside (0, 1)
    TickTooCoarse,           // tick does not divide the probability domain
    SpreadBelowCost,         // the market is tighter than cushion + edge
    BandTooNarrow,           // reward band cannot cover cushion + edge
    SizeBelowRewardMinimum,  // order too small to earn rewards
    InventoryFull            // both sides blocked by inventory
};

inline const char* quote_status_name(QuoteStatus s) noexcept {
    switch (s) {
        case QuoteStatus::Placed:                 return "placed";
        case QuoteStatus::InvalidBook:            return "invalid_book";
        case QuoteStatus::InvalidFairValue:       return "invalid_fair_value";
        case QuoteStatus::TickTooCoarse:          return "tick_too_coarse";
        case QuoteStatus::SpreadBelowCost:        return "spread_below_cost";
        case QuoteStatus::BandTooNarrow:          return "band_too_narrow";
        case QuoteStatus::SizeBelowRewardMinimum: return "size_below_reward_min";
        case QuoteStatus::InventoryFull:          return "inventory_full";
    }
    return "unknown";
}

struct QuotePlan {
    QuoteStatus status = QuoteStatus::InvalidBook;
    uint64_t bid_price = 0;
    uint64_t ask_price = 0;
    uint64_t bid_size = 0;
    uint64_t ask_size = 0;
    // Diagnostic: the half-width the configuration demands, in 1e6 units.
    uint64_t required_half_width = 0;
};

namespace maker_detail {

inline uint64_t round_down(uint64_t value, uint64_t tick) noexcept {
    return tick == 0 ? value : (value / tick) * tick;
}

inline uint64_t round_up(uint64_t value, uint64_t tick) noexcept {
    if (tick == 0) return value;
    const uint64_t rem = value % tick;
    return rem == 0 ? value : value + (tick - rem);
}

}  // namespace maker_detail

// Decides a symmetric two-sided quote. Never touches I/O, never allocates.
inline QuotePlan plan_quotes(const QuoteInput& in,
                             const QuoteConfig& cfg) noexcept {
    QuotePlan plan;

    if (in.best_bid == 0 || in.best_ask == 0 || in.best_bid >= in.best_ask ||
        in.best_ask > 1000000u)
        return plan;  // InvalidBook

    if (in.fair_value == 0 || in.fair_value >= 1000000u) {
        plan.status = QuoteStatus::InvalidFairValue;
        return plan;
    }

    if (in.tick_size == 0 || 1000000u % in.tick_size != 0) {
        plan.status = QuoteStatus::TickTooCoarse;
        return plan;
    }

    // Required half-width, in probability units.
    const uint64_t required =
        (static_cast<uint64_t>(cfg.adverse_sel_bps) +
         static_cast<uint64_t>(cfg.min_edge_bps)) *
        MAKER_BPS_UNITS;
    plan.required_half_width = required;

    // (2) The market's own spread has to pay for that width. Quoting into a
    // book tighter than our cost is how a maker becomes exit liquidity.
    const uint64_t market_half = (in.best_ask - in.best_bid) / 2u;
    if (market_half < required) {
        plan.status = QuoteStatus::SpreadBelowCost;
        return plan;
    }

    // (3) Reward eligibility: the venue only pays for quotes inside
    // rewardsMaxSpread of the midpoint. If our cushion does not fit in that
    // band, we would be choosing between rewards and survival.
    const uint64_t mid = (in.best_bid + in.best_ask) / 2u;
    if (cfg.rewards_max_spread_bps > 0) {
        const uint64_t band_half =
            static_cast<uint64_t>(cfg.rewards_max_spread_bps) *
            MAKER_BPS_UNITS;
        if (required > band_half) {
            plan.status = QuoteStatus::BandTooNarrow;
            return plan;
        }
    }

    // (4) Size must clear the venue minimum or the quote earns no rewards.
    uint64_t size = cfg.order_shares;
    if (cfg.rewards_max_spread_bps > 0 &&
        size < cfg.rewards_min_size_shares) {
        plan.status = QuoteStatus::SizeBelowRewardMinimum;
        return plan;
    }
    if (size == 0) {
        plan.status = QuoteStatus::SizeBelowRewardMinimum;
        return plan;
    }

    // Prices around fair value. A maker IMPROVES the book, it does not join it:
    // if the touch sits outside the reward band, joining the touch forfeits the
    // rewards the sleeve depends on. Bidding higher than best_bid and offering
    // lower than best_ask is normal maker behaviour; crossing is not.
    const uint64_t band_half =
        cfg.rewards_max_spread_bps > 0
            ? static_cast<uint64_t>(cfg.rewards_max_spread_bps) * MAKER_BPS_UNITS
            : 0;
    const bool band_active = band_half > 0 && band_half <= mid;

    int64_t bid_want = static_cast<int64_t>(in.fair_value) -
                       static_cast<int64_t>(required);
    int64_t ask_want = static_cast<int64_t>(in.fair_value) +
                       static_cast<int64_t>(required);

    if (band_active) {
        // Stay inside the reward band measured from the midpoint.
        const int64_t lo = static_cast<int64_t>(mid - band_half);
        const int64_t hi = static_cast<int64_t>(mid + band_half);
        if (bid_want < lo) bid_want = lo;
        if (ask_want > hi) ask_want = hi;
    }

    // Never cross: a crossing order is a taker and pays the taker fee, which is
    // precisely what this sleeve exists to avoid.
    const int64_t max_bid = static_cast<int64_t>(in.best_ask) -
                            static_cast<int64_t>(in.tick_size);
    const int64_t min_ask = static_cast<int64_t>(in.best_bid) +
                            static_cast<int64_t>(in.tick_size);
    if (bid_want > max_bid) bid_want = max_bid;
    if (ask_want < min_ask) ask_want = min_ask;

    if (bid_want <= 0 || ask_want >= 1000000) {
        plan.status = QuoteStatus::InvalidBook;
        return plan;
    }

    uint64_t bid = maker_detail::round_down(static_cast<uint64_t>(bid_want),
                                            in.tick_size);
    uint64_t ask = maker_detail::round_up(static_cast<uint64_t>(ask_want),
                                          in.tick_size);
    if (ask <= bid) {
        plan.status = QuoteStatus::SpreadBelowCost;
        return plan;
    }

    // Verify the band still holds AFTER tick rounding and the no-cross clamp.
    if (band_active) {
        const uint64_t bid_dist = mid > bid ? mid - bid : 0;
        const uint64_t ask_dist = ask > mid ? ask - mid : 0;
        if (bid_dist > band_half || ask_dist > band_half) {
            plan.status = QuoteStatus::BandTooNarrow;
            return plan;
        }
    }

    // (5) Inventory skew: stop adding to the side we are already long.
    uint64_t bid_size = size;
    uint64_t ask_size = size;
    if (cfg.max_inventory_shares > 0) {
        const uint64_t cap = cfg.max_inventory_shares;
        const int64_t inv = in.inventory_shares;
        if (inv >= static_cast<int64_t>(cap)) {
            bid_size = 0;  // fully long: only offer
        } else if (inv <= -static_cast<int64_t>(cap)) {
            ask_size = 0;  // fully short: only bid
        } else if (inv > 0) {
            // Linear taper on the side that grows the position.
            const uint64_t used = static_cast<uint64_t>(inv);
            const uint64_t left =
                cap > used ? cap - used : 0;
            bid_size = (size / cap) * left;
        } else if (inv < 0) {
            const uint64_t used = static_cast<uint64_t>(-inv);
            const uint64_t left = cap > used ? cap - used : 0;
            ask_size = (size / cap) * left;
        }
    }

    if (bid_size == 0 && ask_size == 0) {
        plan.status = QuoteStatus::InventoryFull;
        return plan;
    }

    // A side that was tapered below the reward minimum no longer earns on that
    // side; drop it rather than rest a quote that pays nothing but carries risk.
    if (cfg.rewards_max_spread_bps > 0 && cfg.rewards_min_size_shares > 0) {
        if (bid_size < cfg.rewards_min_size_shares) bid_size = 0;
        if (ask_size < cfg.rewards_min_size_shares) ask_size = 0;
        if (bid_size == 0 && ask_size == 0) {
            plan.status = QuoteStatus::SizeBelowRewardMinimum;
            return plan;
        }
    }

    plan.status = QuoteStatus::Placed;
    plan.bid_price = bid;
    plan.ask_price = ask;
    plan.bid_size = bid_size;
    plan.ask_size = ask_size;
    return plan;
}

// Edge of a resting quote if it fills and the fair value is right, net of the
// maker fee (zero on this venue: feeSchedule.takerOnly == true) and of the
// adverse cushion. Positive means the quote is worth resting at all.
inline int64_t resting_edge_bps(uint64_t fair_value, uint64_t quote_price,
                               bool is_bid, uint32_t adverse_sel_bps,
                               uint32_t maker_fee_bps) noexcept {
    const int64_t gross = is_bid
        ? static_cast<int64_t>(fair_value) - static_cast<int64_t>(quote_price)
        : static_cast<int64_t>(quote_price) - static_cast<int64_t>(fair_value);
    const int64_t cost =
        (static_cast<int64_t>(adverse_sel_bps) +
         static_cast<int64_t>(maker_fee_bps)) *
        static_cast<int64_t>(MAKER_BPS_UNITS);
    // gross is already in 1e6 units; convert the bps cost the same way.
    return (gross - cost) / static_cast<int64_t>(MAKER_BPS_UNITS);
}

}  // namespace crowdintel

#endif  // CROWDINTEL_MAKER_QUOTE_HPP
